#include <das/Core/GraphRuntime/AuthoringDocContract.h>

#include <algorithm>
#include <cassert>
#include <random>
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
        // componentGuid / entryRef / targetKind extraction
        // -------------------------------------------------------------------

        std::string ComponentGuidOf(const Dto::GraphNodeDto& node)
        {
            if (node.target.component_ref.has_value())
            {
                return node.target.component_ref->component_guid;
            }
            return {};
        }

        // D-84-39: only entryId crosses the boundary. The internal
        // expected_revision/source_fingerprint stay inside GraphDocumentDto.
        std::optional<EntryRef> EntryRefOf(const Dto::GraphNodeDto& node)
        {
            if (!node.target.entry_ref.has_value())
            {
                return std::nullopt;
            }
            return EntryRef{node.target.entry_ref->entry_id};
        }

        std::string TargetKindOf(const Dto::GraphNodeDto& node)
        {
            if (!node.target.target_kind.empty())
            {
                return node.target.target_kind;
            }
            return "componentRef";
        }

        // -------------------------------------------------------------------
        // fields[] projection (D-84-18): derive data-driven render descriptors
        // from the node's current settings values. Each settings key becomes a
        // FieldDescriptor whose valueType is inferred from the JSON value type
        // (0=int 2=float 4=string 8=bool 16=object 32=array) and inputType is
        // plain (0). A dedicated settings-schema registry does not yet exist
        // for GraphTask nodes, so the live settings are the available source.
        // -------------------------------------------------------------------

        std::vector<FieldDescriptor> ProjectFields(
            const yyjson::value& settings)
        {
            std::vector<FieldDescriptor> out;
            auto                         obj = settings.as_object();
            if (!obj.has_value())
            {
                return out;
            }
            for (const auto& kv : *obj)
            {
                const auto& v = kv.second;
                if (v.is_null())
                {
                    continue;
                }
                FieldDescriptor f;
                f.name = std::string(kv.first);
                f.input_type = 0;
                if (v.is_bool())
                {
                    f.value_type = 8;
                }
                else if (v.is_int())
                {
                    f.value_type = 0;
                }
                else if (v.is_real())
                {
                    f.value_type = 2;
                }
                else if (v.is_string())
                {
                    f.value_type = 4;
                }
                else if (v.is_object())
                {
                    f.value_type = 16;
                }
                else if (v.is_array())
                {
                    f.value_type = 32;
                }
                else
                {
                    continue;
                }
                f.value = Das::Utils::CloneYyjsonValue(v);
                out.push_back(std::move(f));
            }
            return out;
        }

        // -------------------------------------------------------------------
        // Reverse projection: linear signal chain -> FormSequenceItem[]
        // -------------------------------------------------------------------

        std::vector<FormSequenceItem> ReverseProjectSequence(
            const Dto::GraphDocumentDto& doc)
        {
            std::unordered_map<std::string, std::string> signal_next;
            std::unordered_set<std::string>              signal_targets;
            for (const auto& node : doc.nodes)
            {
                signal_next.emplace(node.node_id, std::string{});
            }
            for (const auto& edge : doc.edges)
            {
                if (edge.edge_type != "signal")
                {
                    continue;
                }
                signal_next.emplace(edge.source_node_id, edge.target_node_id);
                signal_targets.insert(edge.target_node_id);
            }

            std::vector<FormSequenceItem> items;
            items.reserve(doc.nodes.size());

            // Head = a node that is never a signal target.
            std::string head;
            bool        found_head = false;
            for (const auto& node : doc.nodes)
            {
                if (signal_targets.count(node.node_id) == 0)
                {
                    head = node.node_id;
                    found_head = true;
                    break;
                }
            }

            std::unordered_set<std::string> visited;
            auto build_item = [&](const Dto::GraphNodeDto& node)
            {
                FormSequenceItem item;
                item.id = node.node_id;
                item.component_guid = ComponentGuidOf(node);
                item.target_kind = TargetKindOf(node);
                item.entry_ref = EntryRefOf(node);
                item.fields = ProjectFields(node.settings);
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
                    if (it == signal_next.end() || it->second.empty())
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
                out.id = node.node_id;
                out.component_guid = ComponentGuidOf(node);
                out.target_kind = TargetKindOf(node);
                out.entry_ref = EntryRefOf(node);
                out.fields = ProjectFields(node.settings);
                view.nodes.push_back(std::move(out));
            }

            view.connections.reserve(doc.edges.size());
            for (const auto& edge : doc.edges)
            {
                GraphConnection out;
                out.from_node_id = edge.source_node_id;
                out.from_port_id = edge.source_port_id;
                out.to_node_id = edge.target_node_id;
                out.to_port_id = edge.target_port_id;
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
    // local/anonymous-namespace types. camelCase member names emit the strict
    // Phase 84 keys directly via yyjson reflection; no field_name_rule.
    // -------------------------------------------------------------------

    struct FormSequenceDocShape
    {
        int32_t          contract_version = 1;
        std::string      view_kind{"formSequence"};
        int32_t          revision = 0;
        FormSequenceView form_sequence;
    };

    struct GraphDocShape
    {
        int32_t     contract_version = 1;
        std::string view_kind{"graph"};
        int32_t     revision = 0;
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
struct yyjson::field_name_rule<Das::Core::GraphRuntime::Contract::GraphDocShape>
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
        return HasTag(document, LINEAR_TAG);
    }

    AuthoringDocument ToAuthoringDocument(const Dto::GraphDocumentDto& document)
    {
        AuthoringDocument out;
        out.revision = document.version;

        if (IsLinear(document))
        {
            out.view_kind = ViewKind::FormSequence;
            FormSequenceView view;
            view.items = ReverseProjectSequence(document);
            out.form_sequence = std::move(view);
        }
        else
        {
            out.view_kind = ViewKind::Graph;
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
        RemoveTag(document, LINEAR_TAG);
        return true;
    }

    namespace
    {
        // Manual strict-wire serializer helpers. The earlier shape-based
        // yyjson::object(shape, copy_string) approach emitted every optional
        // field as null, which violates the Phase 84 validator (Optionals must
        // be omitted, not null). These helpers emit only engaged fields, with
        // per das-yyjson-string-safety every std::string payload deep-copied
        // into the document arena via std::make_pair(string_view, copy_string).

        // Build a FieldDescriptor array as a standalone yyjson::value.
        yyjson::value BuildFieldsValue(
            const std::vector<FieldDescriptor>& fields)
        {
            auto root = Das::Utils::MakeYyjsonArray();
            auto arr_o = root.as_array();
            if (!arr_o)
            {
                return root;
            }
            auto& arr = *arr_o;
            for (const auto& f : fields)
            {
                auto f_v = Das::Utils::MakeYyjsonObject();
                auto f_o = f_v.as_object();
                if (!f_o)
                {
                    continue;
                }
                auto fo = *f_o;
                fo["name"] = std::make_pair(
                    std::string_view(f.name),
                    yyjson::copy_string);
                fo["label"] = std::make_pair(
                    std::string_view(f.label),
                    yyjson::copy_string);
                fo["valueType"] = f.value_type;
                fo["inputType"] = f.input_type;
                fo["value"] = Das::Utils::CloneYyjsonValue(f.value);
                if (f.description.has_value())
                {
                    fo["description"] = std::make_pair(
                        std::string_view(*f.description),
                        yyjson::copy_string);
                }
                fo["required"] = f.required;
                fo["readonly"] = f.readonly;
                arr.emplace_back(std::move(f_v));
            }
            return root;
        }

        yyjson::value BuildEntryRefValue(
            const std::optional<EntryRef>& entry_ref)
        {
            auto root = Das::Utils::MakeYyjsonObject();
            if (!entry_ref.has_value())
            {
                return root;
            }
            auto obj_o = root.as_object();
            if (!obj_o)
            {
                return root;
            }
            (*obj_o)["entryId"] = entry_ref->entry_id;
            return root;
        }

        yyjson::value BuildItemValue(const FormSequenceItem& item)
        {
            auto root = Das::Utils::MakeYyjsonObject();
            auto obj_o = root.as_object();
            if (!obj_o)
            {
                return root;
            }
            auto obj = *obj_o;
            obj["id"] =
                std::make_pair(std::string_view(item.id), yyjson::copy_string);
            if (!item.component_guid.empty())
            {
                obj["componentGuid"] = std::make_pair(
                    std::string_view(item.component_guid),
                    yyjson::copy_string);
            }
            obj["targetKind"] = std::make_pair(
                std::string_view(item.target_kind),
                yyjson::copy_string);
            if (item.entry_ref.has_value())
            {
                obj["entryRef"] = BuildEntryRefValue(item.entry_ref);
            }
            if (!item.fields.empty())
            {
                obj["fields"] = BuildFieldsValue(item.fields);
            }
            if (item.enabled.has_value())
            {
                obj["enabled"] = *item.enabled;
            }
            return root;
        }

        yyjson::value BuildNodeValue(const GraphNode& node)
        {
            auto root = Das::Utils::MakeYyjsonObject();
            auto obj_o = root.as_object();
            if (!obj_o)
            {
                return root;
            }
            auto obj = *obj_o;
            obj["id"] =
                std::make_pair(std::string_view(node.id), yyjson::copy_string);
            if (!node.component_guid.empty())
            {
                obj["componentGuid"] = std::make_pair(
                    std::string_view(node.component_guid),
                    yyjson::copy_string);
            }
            obj["targetKind"] = std::make_pair(
                std::string_view(node.target_kind),
                yyjson::copy_string);
            if (node.entry_ref.has_value())
            {
                obj["entryRef"] = BuildEntryRefValue(node.entry_ref);
            }
            if (!node.fields.empty())
            {
                obj["fields"] = BuildFieldsValue(node.fields);
            }
            return root;
        }

        // Build the actionCatalog as a standalone yyjson::value subtree. Per
        // das-yyjson-string-safety: every std::string payload is written via
        // std::make_pair(string_view, yyjson::copy_string) so the data is
        // deep-copied into the document arena. The caller attaches the
        // returned value to the serialized document root.
        yyjson::value BuildActionCatalogValue(const ActionCatalog& catalog)
        {
            auto root = Das::Utils::MakeYyjsonObject();
            auto root_obj_o = root.as_object();
            if (!root_obj_o)
            {
                return root;
            }
            auto root_obj = *root_obj_o;

            // entries[]
            root_obj[std::string_view("entries")] =
                Das::Utils::MakeYyjsonArray();
            auto entries_arr_o =
                root_obj[std::string_view("entries")].as_array();
            if (entries_arr_o)
            {
                auto& entries_arr = *entries_arr_o;
                for (const auto& entry : catalog.entries)
                {
                    auto entry_v = Das::Utils::MakeYyjsonObject();
                    auto eo_o = entry_v.as_object();
                    if (!eo_o)
                    {
                        continue;
                    }
                    auto eo = *eo_o;
                    eo[std::string_view("actionId")] = std::make_pair(
                        std::string_view(entry.action_id),
                        yyjson::copy_string);
                    eo[std::string_view("label")] = std::make_pair(
                        std::string_view(entry.label),
                        yyjson::copy_string);
                    if (entry.description.has_value())
                    {
                        eo[std::string_view("description")] = std::make_pair(
                            std::string_view(*entry.description),
                            yyjson::copy_string);
                    }
                    if (entry.icon.has_value())
                    {
                        eo[std::string_view("icon")] = std::make_pair(
                            std::string_view(*entry.icon),
                            yyjson::copy_string);
                    }
                    entries_arr.emplace_back(std::move(entry_v));
                }
            }

            // groups[] (optional). Built via the same copy_string pair pattern
            // for object keys; each group's actionIds[] is emitted as a JSON
            // array value constructed standalone (per das-yyjson-string-safety:
            // mutable_array::emplace_back has no pair_like overload, so we
            // build the string values explicitly via yyjson::value).
            if (!catalog.groups.empty())
            {
                root_obj[std::string_view("groups")] =
                    Das::Utils::MakeYyjsonArray();
                auto groups_arr_o =
                    root_obj[std::string_view("groups")].as_array();
                if (groups_arr_o)
                {
                    auto& groups_arr = *groups_arr_o;
                    for (const auto& group : catalog.groups)
                    {
                        auto group_v = Das::Utils::MakeYyjsonObject();
                        auto go_o = group_v.as_object();
                        if (!go_o)
                        {
                            continue;
                        }
                        auto go = *go_o;
                        go[std::string_view("id")] = std::make_pair(
                            std::string_view(group.id),
                            yyjson::copy_string);
                        go[std::string_view("label")] = std::make_pair(
                            std::string_view(group.label),
                            yyjson::copy_string);
                        if (group.description.has_value())
                        {
                            go[std::string_view("description")] =
                                std::make_pair(
                                    std::string_view(*group.description),
                                    yyjson::copy_string);
                        }
                        if (group.icon.has_value())
                        {
                            go[std::string_view("icon")] = std::make_pair(
                                std::string_view(*group.icon),
                                yyjson::copy_string);
                        }
                        // Build actionIds[] as a JSON-encoded string value:
                        // per the yyjson skill, mutable arrays do not accept
                        // pair_like, so we construct the entire array value via
                        // yyjson::read on a JSON literal and assign directly.
                        // This is a rare path (groups are Provider-authored,
                        // not hot).
                        std::string aids_json = "[";
                        for (std::size_t k = 0; k < group.action_ids.size();
                             ++k)
                        {
                            if (k != 0)
                            {
                                aids_json += ',';
                            }
                            aids_json += '"';
                            aids_json += group.action_ids[k];
                            aids_json += '"';
                        }
                        aids_json += "]";
                        auto aids_value = yyjson::read(aids_json);
                        go[std::string_view("actionIds")] = aids_value;
                        groups_arr.emplace_back(std::move(group_v));
                    }
                }
            }
            return root;
        }
    } // anonymous namespace

    yyjson::value SerializeDocument(
        const Dto::GraphDocumentDto&        document,
        const std::optional<ActionCatalog>& action_catalog)
    {
        // Build the strict Phase 84 wire by hand. Earlier shape-based
        // yyjson::object(shape, copy_string) emitted every optional as null;
        // the strict validator rejects null for absent optionals, so we emit
        // only engaged fields here. Per das-yyjson-string-safety: every
        // std::string payload is deep-copied into the document arena via
        // std::make_pair(string_view, copy_string), and the resulting value
        // is self-contained.
        auto root = Das::Utils::MakeYyjsonObject();
        auto root_obj_o = root.as_object();
        if (!root_obj_o)
        {
            return root;
        }
        auto root_obj = *root_obj_o;
        root_obj["contractVersion"] = 1;
        root_obj["revision"] = document.version;
        if (IsLinear(document))
        {
            root_obj["viewKind"] = std::make_pair(
                std::string_view("formSequence"),
                yyjson::copy_string);
            // Build formSequence.items[] manually to honor optional<enabled>
            // (omit when disengaged) per D-84-25.
            auto fs_v = Das::Utils::MakeYyjsonObject();
            auto fs_o = fs_v.as_object();
            if (fs_o)
            {
                auto fs = *fs_o;
                auto items_a = Das::Utils::MakeYyjsonArray();
                auto items_o = items_a.as_array();
                if (items_o)
                {
                    auto& items_arr = *items_o;
                    for (const auto& item : ReverseProjectSequence(document))
                    {
                        items_arr.emplace_back(BuildItemValue(item));
                    }
                }
                fs["items"] = std::move(items_a);
            }
            root_obj["formSequence"] = std::move(fs_v);
        }
        else
        {
            root_obj["viewKind"] =
                std::make_pair(std::string_view("graph"), yyjson::copy_string);
            const auto view = MapGraphView(document);
            auto       g_v = Das::Utils::MakeYyjsonObject();
            auto       g_o = g_v.as_object();
            if (g_o)
            {
                auto g = *g_o;
                auto nodes_a = Das::Utils::MakeYyjsonArray();
                auto nodes_o = nodes_a.as_array();
                if (nodes_o)
                {
                    auto& nodes_arr = *nodes_o;
                    for (const auto& node : view.nodes)
                    {
                        nodes_arr.emplace_back(BuildNodeValue(node));
                    }
                }
                g["nodes"] = std::move(nodes_a);
                if (!view.connections.empty())
                {
                    auto conns_a = Das::Utils::MakeYyjsonArray();
                    auto conns_o = conns_a.as_array();
                    if (conns_o)
                    {
                        auto& conns_arr = *conns_o;
                        for (const auto& c : view.connections)
                        {
                            auto c_v = Das::Utils::MakeYyjsonObject();
                            auto c_o = c_v.as_object();
                            if (!c_o)
                            {
                                continue;
                            }
                            auto co = *c_o;
                            co["fromNodeId"] = std::make_pair(
                                std::string_view(c.from_node_id),
                                yyjson::copy_string);
                            co["fromPortId"] = std::make_pair(
                                std::string_view(c.from_port_id),
                                yyjson::copy_string);
                            co["toNodeId"] = std::make_pair(
                                std::string_view(c.to_node_id),
                                yyjson::copy_string);
                            co["toPortId"] = std::make_pair(
                                std::string_view(c.to_port_id),
                                yyjson::copy_string);
                            conns_arr.emplace_back(std::move(c_v));
                        }
                    }
                    g["connections"] = std::move(conns_a);
                }
            }
            root_obj["graph"] = std::move(g_v);
        }

        // The viewData partition is emitted at the document root (same level
        // as actionCatalog) whenever engaged, so clients receive the layout
        // coordinates on every GetDocument/Apply round-trip. SerializeSource
        // emits the same key via the GraphDocumentDto field reflection.
        if (!document.view_data.is_null())
        {
            root_obj["viewData"] =
                Das::Utils::CloneYyjsonValue(document.view_data);
        }

        if (action_catalog.has_value())
        {
            root_obj["actionCatalog"] =
                BuildActionCatalogValue(*action_catalog);
        }
        return root;
    }

    yyjson::value SerializeSource(const Dto::GraphDocumentDto& document)
    {
        // The D-41 source blob: the full GraphDocumentDto (the persisted truth
        // fed to compilation/execution). copy_string deep-copies every string
        // payload into the document arena so the returned value is
        // self-contained and safe to hand across the boundary.
        return yyjson::object(document, yyjson::copy_string);
    }

    // -------------------------------------------------------------------
    // Authoring change dispatch (strict Phase 84 kinds)
    // -------------------------------------------------------------------

    namespace
    {
        using FormSeqDto = Dto::FormSequenceDto;
        using FormSeqItem = Dto::FormSequenceItemDto;

        bool IsLinearMode(const Dto::GraphDocumentDto& doc) noexcept
        {
            return IsLinear(doc);
        }

        /// Ordered node ids of a linear signal chain (head → tail), then any
        /// disconnected nodes in array order. Shared by the reverse
        /// projections.
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
                    head = node.node_id;
                    found_head = true;
                    break;
                }
            }

            std::vector<std::string>        order;
            std::unordered_set<std::string> visited;
            auto                            push = [&](const std::string& id)
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
            seq.version = doc.version;
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
                item.target = node.target;
                item.settings = Das::Utils::CloneYyjsonValue(node.settings);
                seq.items.push_back(std::move(item));
            }
            return seq;
        }

        ChangeErrorKind MapGraphError(AuthoringErrorKind k)
        {
            switch (k)
            {
            case AuthoringErrorKind::None:
                return ChangeErrorKind::None;
            case AuthoringErrorKind::NodeNotFound:
                return ChangeErrorKind::NodeNotFound;
            case AuthoringErrorKind::DuplicateNodeId:
                return ChangeErrorKind::DuplicateNodeId;
            case AuthoringErrorKind::EmptyNodeId:
                return ChangeErrorKind::EmptyNodeId;
            case AuthoringErrorKind::InvalidEdge:
                return ChangeErrorKind::InvalidEdge;
            case AuthoringErrorKind::EdgeNotFound:
                return ChangeErrorKind::EdgeNotFound;
            default:
                return ChangeErrorKind::InvalidEdge;
            }
        }

        ChangeErrorKind MapSeqError(FormSequenceErrorKind k)
        {
            switch (k)
            {
            case FormSequenceErrorKind::None:
                return ChangeErrorKind::None;
            case FormSequenceErrorKind::EmptyItemId:
                return ChangeErrorKind::EmptyNodeId;
            case FormSequenceErrorKind::DuplicateItemId:
                return ChangeErrorKind::DuplicateNodeId;
            case FormSequenceErrorKind::ItemNotFound:
                return ChangeErrorKind::ItemNotFound;
            case FormSequenceErrorKind::InvalidIndex:
                return ChangeErrorKind::InvalidIndex;
            case FormSequenceErrorKind::NoChange:
                return ChangeErrorKind::NoChange;
            }
            return ChangeErrorKind::InvalidOp;
        }

        bool ActionIdIsRegistered(
            const std::vector<std::string>& valid_action_ids,
            std::string_view                action_id)
        {
            for (const auto& candidate : valid_action_ids)
            {
                if (candidate == action_id)
                {
                    return true;
                }
            }
            return false;
        }

        /// Allocate a fresh stable node/item id that does not collide with any
        /// id already present on the store. The id is opaque and stable across
        /// formSequence/graph projection (D-84-38) because it lives on the
        /// shared GraphDocumentDto node_id and the linear projection copies it
        /// verbatim into FormSequenceItem.id.
        std::string AllocateStableNodeId(const Dto::GraphDocumentDto& doc)
        {
            std::unordered_set<std::string> existing;
            for (const auto& node : doc.nodes)
            {
                existing.insert(node.node_id);
            }
            // Counter-based id; deterministic within a session and unique
            // because it scans the live store.
            std::size_t counter = existing.size();
            std::string candidate;
            do
            {
                candidate = "n_" + std::to_string(counter);
                ++counter;
            } while (existing.count(candidate) != 0);
            return candidate;
        }

        // -----------------------------------------------------------------
        // Linear-mode dispatch (D-84-35): setValue/addFormSequenceItem/
        // moveFormSequenceItem/removeFormSequenceItem/convertView.
        // -----------------------------------------------------------------

        AuthoringChangeResult ApplyLinearChange(
            Dto::GraphDocumentDto&          document,
            const AuthoringChange&          change,
            const std::vector<std::string>& valid_action_ids)
        {
            FormSequenceChange fc;
            if (change.kind == "addFormSequenceItem")
            {
                if (!change.action_id.has_value() || change.action_id->empty())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "addFormSequenceItem.actionId required",
                        {},
                        std::nullopt};
                }
                if (!ActionIdIsRegistered(valid_action_ids, *change.action_id))
                {
                    return {
                        ChangeErrorKind::UnknownActionId,
                        "addFormSequenceItem.actionId not registered",
                        {{"error",
                          "addFormSequenceItem.actionId not registered",
                          std::nullopt,
                          "addFormSequenceItem.actionId"}},
                        std::nullopt};
                }
                const auto new_id = AllocateStableNodeId(document);
                AppendSequenceItemChange c;
                c.item.item_id = new_id;
                c.item.target.target_kind = "componentRef";
                c.item.target.component_ref =
                    Dto::ComponentRefDto{"componentRef", *change.action_id, {}};
                // Default fields: empty settings blob. Provider may refine via
                // future manifest descriptor projection.
                fc.payload = c;

                auto seq = GraphToSequence(document);
                if (change.insertion_index.has_value())
                {
                    // Honour the caller's insertionIndex by using
                    // InsertSequenceItem semantics when given. The fc payload
                    // already encodes Append; if the caller wants an insert,
                    // rebuild as Insert.
                    std::size_t index =
                        static_cast<std::size_t>(*change.insertion_index);
                    if (index > seq.items.size())
                    {
                        index = seq.items.size();
                    }
                    InsertSequenceItemChange ic;
                    ic.index = index;
                    ic.item = std::move(c.item);
                    fc.payload = ic;
                }

                auto r = FormSequenceProjector::ApplyChange(seq, fc);
                if (!r.Ok())
                {
                    return {
                        MapSeqError(r.error_kind),
                        std::move(r.message),
                        {},
                        std::nullopt};
                }
                auto rebuilt = FormSequenceProjector::Project(seq);
                rebuilt.tags = std::move(document.tags);
                rebuilt.fingerprint = document.fingerprint;
                document = std::move(rebuilt);
                return {
                    ChangeErrorKind::None,
                    "",
                    {},
                    std::optional<std::string>{new_id}};
            }
            if (change.kind == "moveFormSequenceItem")
            {
                if (!change.form_sequence_item_id.has_value()
                    || change.form_sequence_item_id->empty()
                    || !change.destination_index.has_value())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "moveFormSequenceItem requires formSequenceItemId "
                        "and destinationIndex",
                        {},
                        std::nullopt};
                }
                auto        seq = GraphToSequence(document);
                std::size_t from = 0;
                bool        found = false;
                for (std::size_t i = 0; i < seq.items.size(); ++i)
                {
                    if (seq.items[i].item_id == *change.form_sequence_item_id)
                    {
                        from = i;
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    return {
                        ChangeErrorKind::ItemNotFound,
                        "moveFormSequenceItem.formSequenceItemId not found",
                        {},
                        std::nullopt};
                }
                const std::size_t to =
                    static_cast<std::size_t>(*change.destination_index);
                if (to >= seq.items.size())
                {
                    return {
                        ChangeErrorKind::InvalidIndex,
                        "moveFormSequenceItem.destinationIndex out of range",
                        {},
                        std::nullopt};
                }
                fc.payload = MoveSequenceItemChange{from, to};
                auto r = FormSequenceProjector::ApplyChange(seq, fc);
                if (!r.Ok())
                {
                    return {
                        MapSeqError(r.error_kind),
                        std::move(r.message),
                        {},
                        std::nullopt};
                }
                auto rebuilt = FormSequenceProjector::Project(seq);
                rebuilt.tags = std::move(document.tags);
                rebuilt.fingerprint = document.fingerprint;
                document = std::move(rebuilt);
                return {ChangeErrorKind::None, "", {}, std::nullopt};
            }
            if (change.kind == "removeFormSequenceItem")
            {
                if (!change.item_or_node_id.has_value()
                    || change.item_or_node_id->empty())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "removeFormSequenceItem.formSequenceItemId required",
                        {},
                        std::nullopt};
                }
                fc.payload = RemoveSequenceItemChange{*change.item_or_node_id};
                auto seq = GraphToSequence(document);
                auto r = FormSequenceProjector::ApplyChange(seq, fc);
                if (!r.Ok())
                {
                    return {
                        MapSeqError(r.error_kind),
                        std::move(r.message),
                        {},
                        std::nullopt};
                }
                auto rebuilt = FormSequenceProjector::Project(seq);
                rebuilt.tags = std::move(document.tags);
                rebuilt.fingerprint = document.fingerprint;
                document = std::move(rebuilt);
                return {ChangeErrorKind::None, "", {}, std::nullopt};
            }
            if (change.kind == "setValue")
            {
                if (!change.target.has_value())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "setValue.target required",
                        {},
                        std::nullopt};
                }
                if (change.target->kind == "document")
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "setValue document-level not implemented for linear mode",
                        {},
                        std::nullopt};
                }
                if (change.target->kind != "formSequenceItem")
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "setValue.target.kind must be formSequenceItem in linear mode",
                        {},
                        std::nullopt};
                }
                if (!change.target->id.has_value()
                    || change.target->id->empty())
                {
                    return {
                        ChangeErrorKind::EmptyNodeId,
                        "setValue.target.id required",
                        {},
                        std::nullopt};
                }
                // Build the new settings blob by replacing the named field on
                // the matched item's settings.
                if (!change.field_name.has_value()
                    || change.field_name->empty())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "setValue.fieldName required",
                        {},
                        std::nullopt};
                }
                auto seq = GraphToSequence(document);
                bool found = false;
                for (auto& item : seq.items)
                {
                    if (item.item_id == *change.target->id)
                    {
                        auto settings_obj = item.settings.as_object();
                        if (!settings_obj)
                        {
                            item.settings = Das::Utils::MakeYyjsonObject();
                            settings_obj = item.settings.as_object();
                        }
                        (*settings_obj)[std::string_view(*change.field_name)] =
                            Das::Utils::CloneYyjsonValue(change.value);
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    return {
                        ChangeErrorKind::ItemNotFound,
                        "setValue.target.id not found",
                        {},
                        std::nullopt};
                }
                auto rebuilt = FormSequenceProjector::Project(seq);
                rebuilt.tags = std::move(document.tags);
                rebuilt.fingerprint = document.fingerprint;
                document = std::move(rebuilt);
                return {ChangeErrorKind::None, "", {}, std::nullopt};
            }
            if (change.kind == "convertView")
            {
                if (!change.target_view_kind.has_value())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "convertView.targetViewKind required",
                        {},
                        std::nullopt};
                }
                if (*change.target_view_kind != "graph")
                {
                    return {
                        ChangeErrorKind::UnsupportedView,
                        "linear mode can only convert to graph",
                        {},
                        std::nullopt};
                }
                // RemoveTag is lossless. D-84-38: ids are stable because the
                // store carries them verbatim and the projection copies them.
                RemoveTag(document, LINEAR_TAG);
                return {ChangeErrorKind::None, "", {}, std::nullopt};
            }
            return {
                ChangeErrorKind::InvalidOp,
                "kind '" + change.kind + "' is not a linear-mode op",
                {},
                std::nullopt};
        }

        // -----------------------------------------------------------------
        // Graph-mode dispatch (D-84-37): addGraphNode/removeGraphNode/
        // connectGraphPorts/disconnectGraphPorts/setValue/convertView.
        // -----------------------------------------------------------------

        AuthoringChangeResult ApplyGraphChange(
            Dto::GraphDocumentDto&          document,
            const AuthoringChange&          change,
            const std::vector<std::string>& valid_action_ids)
        {
            GraphAuthoringChange gac;
            if (change.kind == "addGraphNode")
            {
                if (!change.action_id.has_value() || change.action_id->empty())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "addGraphNode.actionId required",
                        {},
                        std::nullopt};
                }
                if (!ActionIdIsRegistered(valid_action_ids, *change.action_id))
                {
                    return {
                        ChangeErrorKind::UnknownActionId,
                        "addGraphNode.actionId not registered",
                        {{"error",
                          "addGraphNode.actionId not registered",
                          std::nullopt,
                          "addGraphNode.actionId"}},
                        std::nullopt};
                }
                const auto    new_id = AllocateStableNodeId(document);
                AddNodeChange c;
                c.node.node_id = new_id;
                c.node.target.target_kind = "componentRef";
                c.node.target.component_ref =
                    Dto::ComponentRefDto{"componentRef", *change.action_id, {}};
                c.node.settings = Das::Utils::MakeYyjsonObject();
                gac.payload = c;

                auto r = ApplySettingsChange(document, gac);
                if (!r.Ok())
                {
                    return {
                        MapGraphError(r.error_kind),
                        std::move(r.message),
                        {},
                        std::nullopt};
                }
                return {
                    ChangeErrorKind::None,
                    "",
                    {},
                    std::optional<std::string>{new_id}};
            }
            if (change.kind == "removeGraphNode")
            {
                if (!change.item_or_node_id.has_value()
                    || change.item_or_node_id->empty())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "removeGraphNode.nodeId required",
                        {},
                        std::nullopt};
                }
                gac.payload = RemoveNodeChange{*change.item_or_node_id};
            }
            else if (change.kind == "connectGraphPorts")
            {
                if (!change.connection.has_value())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "connectGraphPorts requires fromNodeId/fromPortId/toNodeId/toPortId",
                        {},
                        std::nullopt};
                }
                const auto&        conn = *change.connection;
                ConnectPortsChange c;
                c.edge.edge_id =
                    "edge::" + conn.from_node_id + "::" + conn.from_port_id
                    + "::" + conn.to_node_id + "::" + conn.to_port_id;
                c.edge.source_node_id = conn.from_node_id;
                c.edge.source_port_id = conn.from_port_id;
                c.edge.target_node_id = conn.to_node_id;
                c.edge.target_port_id = conn.to_port_id;
                c.edge.edge_type = "data";
                gac.payload = c;
            }
            else if (change.kind == "disconnectGraphPorts")
            {
                if (!change.connection.has_value())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "disconnectGraphPorts requires fromNodeId/fromPortId/toNodeId/toPortId",
                        {},
                        std::nullopt};
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
                    return {
                        ChangeErrorKind::EdgeNotFound,
                        "no edge matches the given connection endpoints",
                        {},
                        std::nullopt};
                }
                gac.payload = DisconnectPortsChange{edge_id};
            }
            else if (change.kind == "setValue")
            {
                if (!change.target.has_value())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "setValue.target required",
                        {},
                        std::nullopt};
                }
                if (change.target->kind == "document")
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "setValue document-level not implemented for graph mode",
                        {},
                        std::nullopt};
                }
                if (change.target->kind != "graphNode")
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "setValue.target.kind must be graphNode in graph mode",
                        {},
                        std::nullopt};
                }
                if (!change.target->id.has_value() || change.target->id->empty()
                    || !change.field_name.has_value()
                    || change.field_name->empty())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "setValue.target.id and fieldName required",
                        {},
                        std::nullopt};
                }
                bool found = false;
                for (auto& node : document.nodes)
                {
                    if (node.node_id == *change.target->id)
                    {
                        auto settings_obj = node.settings.as_object();
                        if (!settings_obj)
                        {
                            node.settings = Das::Utils::MakeYyjsonObject();
                            settings_obj = node.settings.as_object();
                        }
                        (*settings_obj)[std::string_view(*change.field_name)] =
                            Das::Utils::CloneYyjsonValue(change.value);
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    return {
                        ChangeErrorKind::NodeNotFound,
                        "setValue.target.id not found",
                        {},
                        std::nullopt};
                }
                return {ChangeErrorKind::None, "", {}, std::nullopt};
            }
            else if (change.kind == "moveGraphNode")
            {
                if (!change.item_or_node_id.has_value()
                    || change.item_or_node_id->empty())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "moveGraphNode.nodeId required",
                        {},
                        std::nullopt};
                }
                // The coordinate pair is written inline into the
                // document-level viewData partition (viewData is a
                // document-level partition, not node.settings, so the
                // GraphAuthoringChange variant is not involved). Key-set
                // validation is the scheduler payload validator's job; a
                // disengaged numeric slot is still rejected here so the store
                // never gains a null coordinate pair.
                if (!change.position_x.has_value()
                    || !change.position_y.has_value())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "moveGraphNode.position.x/y required",
                        {},
                        std::nullopt};
                }
                auto view_obj = document.view_data.as_object();
                if (!view_obj)
                {
                    document.view_data = Das::Utils::MakeYyjsonObject();
                    view_obj = document.view_data.as_object();
                }
                auto positions =
                    (*view_obj)[std::string_view("positions")].as_object();
                if (!positions)
                {
                    (*view_obj)[std::string_view("positions")] =
                        Das::Utils::MakeYyjsonObject();
                    positions =
                        (*view_obj)[std::string_view("positions")].as_object();
                }
                auto position = Das::Utils::MakeYyjsonObject();
                auto position_obj = position.as_object();
                if (position_obj)
                {
                    (*position_obj)[std::string_view("x")] = *change.position_x;
                    (*position_obj)[std::string_view("y")] = *change.position_y;
                }
                (*positions)[std::string_view(*change.item_or_node_id)] =
                    std::move(position);
                return {ChangeErrorKind::None, "", {}, std::nullopt};
            }
            else if (change.kind == "convertView")
            {
                if (!change.target_view_kind.has_value())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "convertView.targetViewKind required",
                        {},
                        std::nullopt};
                }
                if (*change.target_view_kind != "formSequence")
                {
                    return {
                        ChangeErrorKind::UnsupportedView,
                        "graph mode can only convert to formSequence if the topology is linear",
                        {},
                        std::nullopt};
                }
                // Best-effort one-way conversion: tag the document so the
                // projection treats it as linear. Real graph→formSequence is
                // only valid when the topology is a single signal chain; that
                // invariant is the Provider's responsibility (D-84-29).
                document.tags.emplace_back(LINEAR_TAG);
                return {ChangeErrorKind::None, "", {}, std::nullopt};
            }
            else
            {
                return {
                    ChangeErrorKind::InvalidOp,
                    "kind '" + change.kind + "' is not a graph-mode op",
                    {},
                    std::nullopt};
            }

            auto r = ApplySettingsChange(document, gac);
            if (!r.Ok())
            {
                return {
                    MapGraphError(r.error_kind),
                    std::move(r.message),
                    {},
                    std::nullopt};
            }
            return {ChangeErrorKind::None, "", {}, std::nullopt};
        }
    } // anonymous namespace

    AuthoringChangeResult ApplyAuthoringChange(
        Dto::GraphDocumentDto&          document,
        const AuthoringChange&          change,
        const std::vector<std::string>& valid_action_ids)
    {
        AuthoringChangeResult r =
            IsLinearMode(document)
                ? ApplyLinearChange(document, change, valid_action_ids)
                : ApplyGraphChange(document, change, valid_action_ids);
        if (r.Ok())
        {
            ++document.version;
        }
        return r;
    }
} // namespace Contract

DAS_CORE_GRAPHRUNTIME_NS_END
