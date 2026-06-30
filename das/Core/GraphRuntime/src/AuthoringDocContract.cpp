#include <das/Core/GraphRuntime/AuthoringDocContract.h>

#include <algorithm>
#include <cassert>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <das/Core/GraphRuntime/FormSequenceProjector.h>
#include <das/Core/GraphRuntime/GraphAuthoring.h>
#include <das/Utils/DasJsonCore.h>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

namespace Contract
{
    namespace
    {
        // -------------------------------------------------------------------
        // Tag handling
        // -------------------------------------------------------------------

        bool HasTag(const Dto::GraphDocumentDto& doc, std::string_view tag)
        {
            for (const auto& t : doc.tags)
            {
                if (t == tag)
                {
                    return true;
                }
            }
            return false;
        }

        void RemoveTag(Dto::GraphDocumentDto& doc, std::string_view tag)
        {
            doc.tags.erase(
                std::remove_if(
                    doc.tags.begin(),
                    doc.tags.end(),
                    [&](const std::string& t) { return t == tag; }),
                doc.tags.end());
        }

        // -------------------------------------------------------------------
        // componentGuid extraction from a node target
        // -------------------------------------------------------------------

        std::string ComponentGuidOf(const Dto::GraphNodeDto& node)
        {
            if (node.target.component_ref.has_value())
            {
                return node.target.component_ref->component_guid;
            }
            return {};
        }

        // -------------------------------------------------------------------
        // Reverse projection: linear signal chain -> SequenceItem[]
        // -------------------------------------------------------------------
        //
        // The linear state is a single signal chain. We recover its order by
        // walking signal edges: find the head (the node with no incoming
        // signal edge), then follow signal_out -> signal_in. Non-signal edges
        // and disconnected nodes are ignored for the sequence ordering; every
        // node still appears (disconnected ones appended at the tail in array
        // order so nothing is dropped).
        // -------------------------------------------------------------------

        std::vector<SequenceItem> ReverseProjectSequence(
            const Dto::GraphDocumentDto& doc)
        {
            std::unordered_map<std::string, std::string> signal_next;
            std::unordered_set<std::string>              has_incoming;

            for (const auto& node : doc.nodes)
            {
                has_incoming.insert(node.node_id);
            }
            // has_incoming starts as "all nodes"; we remove heads below by
            // flagging nodes that ARE signal targets.
            std::unordered_set<std::string> signal_targets;
            for (const auto& edge : doc.edges)
            {
                if (edge.edge_type != "signal")
                {
                    continue;
                }
                // First signal edge out of a node wins; the formSequence
                // projection produces exactly one out per node.
                signal_next.emplace(edge.source_node_id, edge.target_node_id);
                signal_targets.insert(edge.target_node_id);
            }

            std::vector<SequenceItem> items;
            items.reserve(doc.nodes.size());

            // Head = a node that is never a signal target.
            std::string head;
            bool        found_head = false;
            for (const auto& node : doc.nodes)
            {
                if (signal_targets.count(node.node_id) == 0)
                {
                    head       = node.node_id;
                    found_head = true;
                    break;
                }
            }

            std::unordered_set<std::string> visited;
            auto build_item = [&](const Dto::GraphNodeDto& node)
            {
                SequenceItem item;
                item.id       = node.node_id;
                item.type     = ComponentGuidOf(node);
                item.settings = Das::Utils::CloneYyjsonValue(node.settings);
                items.push_back(std::move(item));
                visited.insert(node.node_id);
            };

            if (found_head)
            {
                const Dto::GraphNodeDto* cur = nullptr;
                for (const auto& node : doc.nodes)
                {
                    if (node.node_id == head)
                    {
                        cur = &node;
                        break;
                    }
                }
                while (cur != nullptr)
                {
                    build_item(*cur);
                    auto it = signal_next.find(cur->node_id);
                    if (it == signal_next.end())
                    {
                        break;
                    }
                    const std::string& next_id = it->second;
                    cur = nullptr;
                    for (const auto& node : doc.nodes)
                    {
                        if (node.node_id == next_id)
                        {
                            cur = &node;
                            break;
                        }
                    }
                }
            }

            // Append any nodes not reachable along the chain (preserves all
            // nodes so the projection is total — matches Project()'s totality).
            for (const auto& node : doc.nodes)
            {
                if (visited.count(node.node_id) == 0)
                {
                    build_item(node);
                }
            }

            return items;
        }

        // -------------------------------------------------------------------
        // Thin graph mapping (drops runtime-private fields)
        // -------------------------------------------------------------------

        GraphView MapGraphView(const Dto::GraphDocumentDto& doc)
        {
            GraphView view;
            view.nodes.reserve(doc.nodes.size());
            for (const auto& node : doc.nodes)
            {
                GraphNode out;
                out.id              = node.node_id;
                out.component_guid  = ComponentGuidOf(node);
                out.settings        = Das::Utils::CloneYyjsonValue(node.settings);
                view.nodes.push_back(std::move(out));
            }

            view.connections.reserve(doc.edges.size());
            for (const auto& edge : doc.edges)
            {
                GraphConnection out;
                out.from_node_id = edge.source_node_id;
                out.from_port_id = edge.source_port_id;
                out.to_node_id   = edge.target_node_id;
                out.to_port_id   = edge.target_port_id;
                // edge_id and edge_type are deliberately dropped — they are
                // runtime-private and must NOT leak across the ABI boundary.
                view.connections.push_back(std::move(out));
            }
            return view;
        }
    } // anonymous namespace

    // -------------------------------------------------------------------
    // Serializable JSON-shape aggregates. Declared at Contract namespace scope
    // (NOT anonymous) because boost::pfr aggregate reflection rejects
    // local/anonymous-namespace types. camelCase member names emit the
    // schema-draft keys directly via yyjson reflection; no field_name_rule.
    // -------------------------------------------------------------------

    struct FormSequenceDocShape
    {
        int32_t          contract_version = 1;
        std::string      kind{"formSequence"};
        int32_t          revision = 0;
        std::string      source_fingerprint;
        FormSequenceView form_sequence;
    };

    struct GraphDocShape
    {
        int32_t     contract_version = 1;
        std::string kind{"graph"};
        int32_t     revision = 0;
        std::string source_fingerprint;
        GraphView   graph;
    };

    // field_name_rule for the shapes — declared after the complete type and
    // before Serialize instantiates yyjson::object(shape).
} // namespace Contract
DAS_CORE_GRAPHRUNTIME_NS_END

template <>
struct yyjson::field_name_rule<
    Das::Core::GraphRuntime::Contract::FormSequenceDocShape>
{
    using type = yyjson::snake_to_camel_transform;
};
template <>
struct yyjson::field_name_rule<
    Das::Core::GraphRuntime::Contract::GraphDocShape>
{
    using type = yyjson::snake_to_camel_transform;
};

DAS_CORE_GRAPHRUNTIME_NS_BEGIN
namespace Contract
{

    // -------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------

    bool IsLinear(const Dto::GraphDocumentDto& document) noexcept
    {
        return HasTag(document, kLinearTag);
    }

    AuthoringDocument ToAuthoringDocument(const Dto::GraphDocumentDto& document)
    {
        AuthoringDocument out;
        out.revision          = document.version;
        out.source_fingerprint = document.fingerprint;

        if (IsLinear(document))
        {
            out.kind = Kind::FormSequence;
            FormSequenceView view;
            view.sequence = ReverseProjectSequence(document);
            out.form_sequence = std::move(view);
        }
        else
        {
            out.kind = Kind::Graph;
            out.graph = MapGraphView(document);
        }
        return out;
    }

    bool UpgradeToGraph(Dto::GraphDocumentDto& document) noexcept
    {
        if (!IsLinear(document))
        {
            return false;
        }
        RemoveTag(document, kLinearTag);
        return true;
    }

    yyjson::value SerializeDocument(const Dto::GraphDocumentDto& document)
    {
        // Build the serialisable shape and create the yyjson document with
        // yyjson::copy_string: string payloads are deep-copied into the
        // document's own arena, so the returned value is self-contained and
        // safe to return / write after the local shape is gone. (Without
        // copy_string yyjson would reference the std::string buffers of the
        // local shape → use-after-free on write.)
        if (IsLinear(document))
        {
            FormSequenceDocShape shape;
            shape.contract_version   = 1;
            shape.revision           = document.version;
            shape.source_fingerprint = document.fingerprint;
            shape.form_sequence.sequence = ReverseProjectSequence(document);
            return yyjson::object(shape, yyjson::copy_string);
        }
        GraphDocShape shape;
        shape.contract_version   = 1;
        shape.revision           = document.version;
        shape.source_fingerprint = document.fingerprint;
        shape.graph              = MapGraphView(document);
        return yyjson::object(shape, yyjson::copy_string);
    }

    // -------------------------------------------------------------------
    // Authoring change dispatch
    // -------------------------------------------------------------------

    namespace
    {
        using FormSeqDto  = Dto::FormSequenceDto;
        using FormSeqItem = Dto::FormSequenceItemDto;

        /// Ordered node ids of a linear signal chain (head → tail), then any
        /// disconnected nodes in array order. Shared by the reverse projections.
        std::vector<std::string> LinearOrder(const Dto::GraphDocumentDto& doc)
        {
            std::unordered_map<std::string, std::string> signal_next;
            std::unordered_set<std::string>              signal_targets;
            for (const auto& edge : doc.edges)
            {
                if (edge.edge_type != "signal")
                {
                    continue;
                }
                signal_next.emplace(edge.source_node_id, edge.target_node_id);
                signal_targets.insert(edge.target_node_id);
            }

            std::string head;
            bool        found_head = false;
            for (const auto& node : doc.nodes)
            {
                if (signal_targets.count(node.node_id) == 0)
                {
                    head       = node.node_id;
                    found_head = true;
                    break;
                }
            }

            std::vector<std::string>      order;
            std::unordered_set<std::string> visited;
            auto push = [&](const std::string& id)
            {
                order.push_back(id);
                visited.insert(id);
            };

            if (found_head)
            {
                std::string cur = head;
                while (!cur.empty() && visited.count(cur) == 0)
                {
                    push(cur);
                    auto it = signal_next.find(cur);
                    if (it == signal_next.end())
                    {
                        break;
                    }
                    cur = it->second;
                }
            }
            for (const auto& node : doc.nodes)
            {
                if (visited.count(node.node_id) == 0)
                {
                    push(node.node_id);
                }
            }
            return order;
        }

        /// Reverse-project the linear store onto a FormSequenceDto so the
        /// existing FormSequenceProjector mutation logic can be reused.
        FormSeqDto GraphToSequence(const Dto::GraphDocumentDto& doc)
        {
            FormSeqDto seq;
            seq.document_id = doc.document_id;
            seq.version     = doc.version;
            seq.fingerprint = doc.fingerprint;

            std::unordered_map<std::string, const Dto::GraphNodeDto*> by_id;
            for (const auto& node : doc.nodes)
            {
                by_id.emplace(node.node_id, &node);
            }
            for (const auto& id : LinearOrder(doc))
            {
                auto it = by_id.find(id);
                if (it == by_id.end())
                {
                    continue;
                }
                const auto& node = *it->second;
                FormSeqItem item;
                item.item_id = node.node_id;
                item.target  = node.target;
                item.settings = Das::Utils::CloneYyjsonValue(node.settings);
                seq.items.push_back(std::move(item));
            }
            return seq;
        }

        ChangeErrorKind MapGraphError(AuthoringErrorKind k)
        {
            switch (k)
            {
                case AuthoringErrorKind::None:           return ChangeErrorKind::None;
                case AuthoringErrorKind::NodeNotFound:   return ChangeErrorKind::NodeNotFound;
                case AuthoringErrorKind::DuplicateNodeId:return ChangeErrorKind::DuplicateNodeId;
                case AuthoringErrorKind::EmptyNodeId:    return ChangeErrorKind::EmptyNodeId;
                case AuthoringErrorKind::InvalidEdge:    return ChangeErrorKind::InvalidEdge;
                case AuthoringErrorKind::EdgeNotFound:   return ChangeErrorKind::EdgeNotFound;
                default:                                 return ChangeErrorKind::InvalidEdge;
            }
        }

        ChangeErrorKind MapSeqError(FormSequenceErrorKind k)
        {
            switch (k)
            {
                case FormSequenceErrorKind::None:            return ChangeErrorKind::None;
                case FormSequenceErrorKind::EmptyItemId:     return ChangeErrorKind::EmptyNodeId;
                case FormSequenceErrorKind::DuplicateItemId: return ChangeErrorKind::DuplicateNodeId;
                case FormSequenceErrorKind::ItemNotFound:    return ChangeErrorKind::ItemNotFound;
                case FormSequenceErrorKind::InvalidIndex:    return ChangeErrorKind::InvalidIndex;
                case FormSequenceErrorKind::NoChange:        return ChangeErrorKind::NoChange;
            }
            return ChangeErrorKind::InvalidOp;
        }

        /// graph-mode dispatch: delegate to GraphAuthoring::ApplySettingsChange.
        AuthoringChangeResult ApplyGraphChange(
            Dto::GraphDocumentDto&  document,
            const AuthoringChange&  change)
        {
            GraphAuthoringChange gac;
            if (change.op == "addNode")
            {
                if (!change.node.has_value())
                {
                    return {ChangeErrorKind::InvalidOp, "addNode requires node"};
                }
                AddNodeChange c;
                c.node.node_id = change.node->id;
                c.node.settings = Das::Utils::CloneYyjsonValue(change.node->settings);
                c.node.target.target_kind   = "componentRef";
                c.node.target.component_ref =
                    Dto::ComponentRefDto{"componentRef", change.node->component_guid, {}};
                gac.payload = c;
            }
            else if (change.op == "removeNode")
            {
                gac.payload = RemoveNodeChange{change.node_id};
            }
            else if (change.op == "connectPorts")
            {
                if (!change.connection.has_value())
                {
                    return {ChangeErrorKind::InvalidOp, "connectPorts requires connection"};
                }
                const auto& conn = *change.connection;
                ConnectPortsChange c;
                c.edge.edge_id        = "edge::" + conn.from_node_id + "::"
                                        + conn.from_port_id + "::" + conn.to_node_id
                                        + "::" + conn.to_port_id;
                c.edge.source_node_id = conn.from_node_id;
                c.edge.source_port_id = conn.from_port_id;
                c.edge.target_node_id = conn.to_node_id;
                c.edge.target_port_id = conn.to_port_id;
                c.edge.edge_type      = "data";
                gac.payload = c;
            }
            else if (change.op == "disconnectPorts")
            {
                if (!change.connection.has_value())
                {
                    return {ChangeErrorKind::InvalidOp, "disconnectPorts requires connection"};
                }
                const auto& conn = *change.connection;
                std::string edge_id;
                for (const auto& edge : document.edges)
                {
                    if (edge.source_node_id == conn.from_node_id
                        && edge.source_port_id == conn.from_port_id
                        && edge.target_node_id == conn.to_node_id
                        && edge.target_port_id == conn.to_port_id)
                    {
                        edge_id = edge.edge_id;
                        break;
                    }
                }
                if (edge_id.empty())
                {
                    return {ChangeErrorKind::EdgeNotFound,
                            "no edge matches the given connection endpoints"};
                }
                gac.payload = DisconnectPortsChange{edge_id};
            }
            else if (change.op == "updateNodeConfig" || change.op == "setValue")
            {
                UpdateNodeConfigChange c;
                c.node_id  = change.node_id;
                c.settings = Das::Utils::CloneYyjsonValue(change.settings);
                gac.payload = c;
            }
            else
            {
                return {ChangeErrorKind::InvalidOp,
                        "op '" + change.op + "' is not a graph-mode op"};
            }

            auto r = ApplySettingsChange(document, gac);
            if (!r.Ok())
            {
                return {MapGraphError(r.error_kind), std::move(r.message)};
            }
            return {ChangeErrorKind::None, ""};
        }

        /// linear-mode dispatch: reverse-project → FormSequenceProjector →
        /// re-project, preserving store identity.
        AuthoringChangeResult ApplyLinearChange(
            Dto::GraphDocumentDto&  document,
            const AuthoringChange&  change)
        {
            FormSequenceChange fc;
            if (change.op == "addSequenceItem")
            {
                if (!change.item.has_value())
                {
                    return {ChangeErrorKind::InvalidOp, "addSequenceItem requires item"};
                }
                AppendSequenceItemChange c;
                c.item.item_id = change.item->id;
                c.item.target.target_kind   = "componentRef";
                c.item.target.component_ref =
                    Dto::ComponentRefDto{"componentRef", change.item->type, {}};
                c.item.settings = Das::Utils::CloneYyjsonValue(change.item->settings);
                fc.payload = c;
            }
            else if (change.op == "moveSequenceItem")
            {
                if (!change.from.has_value() || !change.to.has_value())
                {
                    return {ChangeErrorKind::InvalidOp, "moveSequenceItem requires from/to"};
                }
                fc.payload = MoveSequenceItemChange{*change.from, *change.to};
            }
            else if (change.op == "removeSequenceItem")
            {
                fc.payload = RemoveSequenceItemChange{change.node_id};
            }
            else if (change.op == "setValue")
            {
                SetSequenceItemValueChange c;
                c.item_id  = change.node_id;
                c.settings = Das::Utils::CloneYyjsonValue(change.settings);
                fc.payload = c;
            }
            else
            {
                return {ChangeErrorKind::InvalidOp,
                        "op '" + change.op + "' is not a linear-mode op"};
            }

            auto seq = GraphToSequence(document);
            auto r   = FormSequenceProjector::ApplyChange(seq, fc);
            if (!r.Ok())
            {
                return {MapSeqError(r.error_kind), std::move(r.message)};
            }

            // Re-project the mutated sequence back onto the store, preserving
            // identity (tags / fingerprint). version is bumped by the caller.
            //
            // PREMISE: a linear (formsequence) document is a single signal
            // chain — it carries no non-signal structure. Project() rebuilds
            // nodes + the linear signal edges from scratch, so any non-signal
            // data edges that hypothetically existed on the store are dropped
            // by this full rebuild. That is correct for the linear mode: a
            // document that needed non-signal edges would not be linear and
            // would be in graph mode (where GraphAuthoring mutates in place
            // and never comes through this path). Do not route a non-linear
            // document through linear-mode changes.
            auto rebuilt = FormSequenceProjector::Project(seq);
            rebuilt.tags       = std::move(document.tags);
            rebuilt.fingerprint = document.fingerprint;
            document = std::move(rebuilt);
            return {ChangeErrorKind::None, ""};
        }
    } // anonymous namespace

    AuthoringChangeResult ApplyAuthoringChange(
        Dto::GraphDocumentDto&  document,
        const AuthoringChange&  change)
    {
        AuthoringChangeResult r = IsLinear(document)
            ? ApplyLinearChange(document, change)
            : ApplyGraphChange(document, change);
        if (r.Ok())
        {
            ++document.version;
        }
        return r;
    }
} // namespace Contract

DAS_CORE_GRAPHRUNTIME_NS_END
