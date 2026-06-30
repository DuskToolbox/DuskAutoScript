#include <das/Core/GraphRuntime/AuthoringDocContract.h>

#include <algorithm>
#include <cassert>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

    bool DowngradeToFormSequence(Dto::GraphDocumentDto& document) noexcept
    {
        if (IsLinear(document))
        {
            return false;
        }
        document.tags.emplace_back(kLinearTag);
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
} // namespace Contract

DAS_CORE_GRAPHRUNTIME_NS_END
