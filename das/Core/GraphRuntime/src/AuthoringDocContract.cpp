#include <das/Core/GraphRuntime/AuthoringDocContract.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <das/Core/GraphRuntime/FormSequenceProjector.h>
#include <das/Core/GraphRuntime/GraphAuthoring.h>
#include <das/Core/GraphRuntime/GraphBoundaryComponents.h>
#include <das/Core/Logger/Logger.h>
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
        // componentGuid / targetKind extraction
        // -------------------------------------------------------------------

        std::string ComponentGuidOf(const Dto::GraphNodeDto& node)
        {
            if (node.target.component_ref.has_value())
            {
                return node.target.component_ref->component_guid;
            }
            return {};
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
                // Live keys carry no declaration to source a label from —
                // fall back to the key itself (the same label = name rule
                // the config-declared path applies) so the projected field
                // always satisfies the strict non-empty label contract.
                f.label = f.name;
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
        // Config-declared field projection: manifest config entries merge
        // into fields[] so a freshly added node is immediately editable.
        // Value priority: settings override > declared default > null.
        // -------------------------------------------------------------------

        /// Map a manifest config type onto the FieldDescriptor typing pair
        /// (value_type, input_type). inputType stays within the official 0-8
        /// vocabulary — component-domain type words (taskRepositoryRef) map
        /// conservatively like unknown types; their typed dispatch rides on
        /// the carried config type word (FieldDescriptor::type_word), never
        /// on inputType.
        std::pair<int32_t, int32_t> ConfigFieldInputTypes(std::string_view type)
        {
            if (type == std::string_view("int")
                || type == std::string_view("int64")
                || type == std::string_view("uint"))
            {
                return {0, 0};
            }
            if (type == std::string_view("float")
                || type == std::string_view("double"))
            {
                return {2, 0};
            }
            if (type == std::string_view("string"))
            {
                return {4, 0};
            }
            if (type == std::string_view("time"))
            {
                // HH:MM 钟点字（string 值 + TimePicker 通道）：契约自洽依据
                // TaskAuthoringContract validator「inputType 8 (time)
                // requires valueType=4」——Core 只强制类型契约，留空语义由
                // manifest description 文案承载（D-B2 零新机制）。
                return {4, 8};
            }
            if (type == std::string_view("bool"))
            {
                return {8, 0};
            }
            if (type == std::string_view("object"))
            {
                return {16, 0};
            }
            if (type == std::string_view("array<string>"))
            {
                // String arrays ride the multiselect comma channel (official
                // inputType vocabulary) so panel edits round-trip as a JSON
                // string array instead of being saved as one string that
                // array readers silently drop. Comma splitting breaks
                // arguments containing commas — precise editing waits for
                // the unified input-editor design.
                return {32, 2};
            }
            if (type.starts_with(std::string_view("array")))
            {
                // Generic arrays keep no control preference: the comma
                // channel would split non-string elements into text.
                return {32, 0};
            }
            return {16, 0};
        }

        /// Merge the config-declared fields (declaration order, label =
        /// declared label ?? name, value = settings override ?? declared
        /// default ?? null) ahead of the undeclared live settings keys
        /// (existing ProjectFields order). A null config pointer keeps the
        /// plain live-value projection.
        ///
        /// include_undeclared_live_keys=false（graph 节点视图）：fields 只含
        /// config 声明——未声明的 settings 键（端口 binding 默认值）经端口
        /// 投影的 value 键通道呈现，不得在 fields 里重复合成（同一数据双
        /// 通道会让编辑 UI 渲染两个联动控件）。formSequence item 视图无端口
        /// 渲染区，保持 true（live 合成是其唯一值编辑通道）。
        std::vector<FieldDescriptor> ProjectFieldsWithConfig(
            const yyjson::value&                      settings,
            const std::vector<ConfigFieldDescriptor>* config_fields,
            bool include_undeclared_live_keys = true)
        {
            if (config_fields == nullptr || config_fields->empty())
            {
                return include_undeclared_live_keys
                           ? ProjectFields(settings)
                           : std::vector<FieldDescriptor>{};
            }
            std::vector<FieldDescriptor> out;
            out.reserve(config_fields->size());
            auto                            settings_obj = settings.as_object();
            std::unordered_set<std::string> covered;
            for (const auto& config_field : *config_fields)
            {
                FieldDescriptor field;
                field.name = config_field.name;
                // 声明的显示文案（locale 已在 catalog 生产侧解析成字符串）；
                // 未声明（空）回退字段名——与端口 displayLabel 的回退链同构。
                field.label = config_field.label.empty() ? config_field.name
                                                         : config_field.label;
                // 声明的帮助文案（同一路径的 locale 解析产物）：未声明不落
                // 键（wire 序列化 engaged-only），已声明则作为字段下方的
                // 帮助文本呈现。
                if (!config_field.description.empty())
                {
                    field.description = config_field.description;
                }
                const auto [value_type, input_type] =
                    ConfigFieldInputTypes(config_field.type);
                field.value_type = value_type;
                field.input_type = input_type;
                // The manifest config type word rides the wire verbatim —
                // the typed-dispatch channel for component-domain words.
                field.type_word = config_field.type;
                // The declared target task type rides along the same
                // declaration channel (engaged only when the manifest named
                // one) so consumers can resolve a repositoryRef picker
                // without a component→task reverse lookup.
                field.task_guid = config_field.task_guid;
                // The required declaration rides onto the projected field —
                // BuildFieldsValue always emits the boolean, so the
                // declaration surfaces even for fresh nodes (no settings
                // yet).
                field.required = config_field.required;
                // The closed candidate-list declaration switches the control
                // to select: candidates are strict CandidateDescriptor
                // objects derived mechanically from the manifest string
                // array (each entry is both the value and the label — the
                // authoring side stays a single plain-string declaration).
                // inputType overrides the generic type mapping: a field with
                // a declared value domain is a dropdown, never free text.
                if (!config_field.options.empty())
                {
                    field.input_type = 1;
                    field.candidates.reserve(config_field.options.size());
                    for (const auto& option : config_field.options)
                    {
                        auto candidate = Das::Utils::MakeYyjsonObject();
                        auto candidate_object = candidate.as_object();
                        if (!candidate_object)
                        {
                            continue;
                        }
                        (*candidate_object)[std::string_view("value")] =
                            std::make_pair(
                                std::string_view(option),
                                yyjson::copy_string);
                        (*candidate_object)[std::string_view("label")] =
                            std::make_pair(
                                std::string_view(option),
                                yyjson::copy_string);
                        field.candidates.push_back(std::move(candidate));
                    }
                }
                bool value_assigned = false;
                if (settings_obj.has_value()
                    && settings_obj->contains(
                        std::string_view(config_field.name)))
                {
                    const auto& settings_value =
                        (*settings_obj)[std::string_view(config_field.name)];
                    // A null settings value reads as "no override" (the same
                    // semantics the live-value projection gives nulls).
                    if (!settings_value.is_null())
                    {
                        field.value =
                            Das::Utils::CloneYyjsonValue(settings_value);
                        value_assigned = true;
                    }
                }
                if (!value_assigned && !config_field.default_value.is_null())
                {
                    field.value = Das::Utils::CloneYyjsonValue(
                        config_field.default_value);
                }
                covered.insert(config_field.name);
                out.push_back(std::move(field));
            }
            // Undeclared settings keys keep the existing live-value
            // projection, appended after the declared fields (formSequence
            // item view only — see the include_undeclared_live_keys note).
            if (include_undeclared_live_keys)
            {
                for (auto& field : ProjectFields(settings))
                {
                    if (covered.count(field.name) != 0)
                    {
                        continue;
                    }
                    out.push_back(std::move(field));
                }
            }
            return out;
        }

        // -------------------------------------------------------------------
        // Reverse projection: linear signal chain -> FormSequenceItem[]
        // -------------------------------------------------------------------

        /// First catalog entry matching componentGuid == actionId, or null.
        /// The single merge point for everything a catalog entry contributes
        /// to a node/item projection: static ports, the default display name
        /// (entry.label carries the manifest kind), the presentation
        /// metadata (description/icon) and the config field declarations.
        const ActionCatalogEntry* FindCatalogEntry(
            const std::optional<ActionCatalog>& action_catalog,
            std::string_view                    component_guid)
        {
            if (!action_catalog.has_value())
            {
                return nullptr;
            }
            for (const auto& entry : action_catalog->entries)
            {
                if (entry.action_id == component_guid)
                {
                    return &entry;
                }
            }
            return nullptr;
        }

        std::vector<FormSequenceItem> ReverseProjectSequence(
            const Dto::GraphDocumentDto&        doc,
            const std::optional<ActionCatalog>& action_catalog)
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
                const ActionCatalogEntry* matched_entry =
                    FindCatalogEntry(action_catalog, item.component_guid);
                item.fields = ProjectFieldsWithConfig(
                    node.settings,
                    matched_entry != nullptr ? &matched_entry->config_fields
                                             : nullptr);
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

        constexpr std::string_view PORT_DIRECTION_INPUT = "input";

        constexpr std::string_view PORT_DIRECTION_OUTPUT = "output";

        constexpr std::string_view PORT_CATEGORY_EXECUTION = "execution";
        constexpr std::string_view PORT_CATEGORY_DATA = "data";

        // begin/end 动态口 value_type 白名单（九词）：签名类型域=全部类型
        // （D-20），词表与 schema AddNodePort.valueType enum、TaskScheduler
        // validator 三处 lockstep——白名单外一律拒绝，不借道未知词宽容。
        constexpr std::array<std::string_view, 9> NODE_PORT_VALUE_TYPES{
            "bool",
            "int",
            "int64",
            "float",
            "double",
            "string",
            "image",
            "base",
            "component"};

        constexpr std::size_t MAX_NODE_PORT_NAME_UTF8_BYTES = 100;

        /// begin/end 组件判定——边界口列表（节点 settings.boundaryPorts）
        /// 的持有人判据：addNodePort 家族对图文档的 dispatch 只路由到边界
        /// 节点，其它节点无口存储域。
        bool IsBoundaryComponent(std::string_view component_guid)
        {
            return component_guid == GRAPH_BEGIN_COMPONENT_GUID
                   || component_guid == GRAPH_END_COMPONENT_GUID;
        }

        /// 边界口条目（begin/end 节点 settings.boundaryPorts 的解析形态，
        /// 与边界会话状态域同形）。方向不存储——由节点角色固化推导。
        struct BoundaryPortEntry
        {
            std::string id;
            std::string type;
        };

        /// 读取 begin/end 节点 settings 的 boundaryPorts 数组：键缺席/
        /// 非数组/条目缺 id/type 字符串键 → 该条目跳过（fail-closed——
        /// 畸形不炸变更路径，口面按空处理）。
        std::vector<BoundaryPortEntry> LoadNodeBoundaryPorts(
            const Dto::GraphNodeDto& node)
        {
            std::vector<BoundaryPortEntry> entries;
            auto settings_obj = node.settings.as_object();
            if (!settings_obj
                || !settings_obj->contains(std::string_view("boundaryPorts")))
            {
                return entries;
            }
            auto arr =
                (*settings_obj)[std::string_view("boundaryPorts")].as_array();
            if (!arr)
            {
                return entries;
            }
            for (const auto& entry_value : *arr)
            {
                auto entry_obj = entry_value.as_object();
                if (!entry_obj)
                {
                    continue;
                }
                BoundaryPortEntry entry;
                if (auto id = (*entry_obj)[std::string_view("id")].as_string())
                {
                    entry.id = std::string(*id);
                }
                if (auto type =
                        (*entry_obj)[std::string_view("type")].as_string())
                {
                    entry.type = std::string(*type);
                }
                if (!entry.id.empty() && !entry.type.empty())
                {
                    entries.push_back(std::move(entry));
                }
            }
            return entries;
        }

        /// 把边界口条目写回节点 settings.boundaryPorts（其余 settings 键
        /// 原样保留；条目形 {id,type}——与读取形态一致）。
        void StoreNodeBoundaryPorts(
            Dto::GraphNodeDto&                    node,
            const std::vector<BoundaryPortEntry>& entries)
        {
            auto settings_obj = node.settings.as_object();
            if (!settings_obj)
            {
                node.settings = Das::Utils::MakeYyjsonObject();
                settings_obj = node.settings.as_object();
            }
            yyjson::value array_value(Das::Utils::MakeYyjsonArray());
            auto          arr = array_value.as_array();
            for (const auto& entry : entries)
            {
                yyjson::value entry_value(Das::Utils::MakeYyjsonObject());
                auto          entry_obj = entry_value.as_object();
                (*entry_obj)[std::string_view("id")] =
                    yyjson::value(std::string(entry.id));
                (*entry_obj)[std::string_view("type")] =
                    yyjson::value(std::string(entry.type));
                arr->emplace_back(std::move(entry_value));
            }
            (*settings_obj)[std::string_view("boundaryPorts")] =
                std::move(array_value);
        }

        /// 口名规则（add/rename 共用）：非空、UTF-8 字节数 ≤100、不以
        /// "$" 开头（前缀保留给机制内部命名空间，用户口名不得占用）。
        /// 返回违规描述，nullopt = 合法。
        std::optional<std::string> NodePortNameViolation(std::string_view name)
        {
            if (name.empty())
            {
                return std::string("port name required");
            }
            if (name.size() > MAX_NODE_PORT_NAME_UTF8_BYTES)
            {
                return std::string("port name must be at most 100 UTF-8 bytes");
            }
            if (name.front() == '$')
            {
                return std::string(
                    "port name must not start with '$' (reserved)");
            }
            return std::nullopt;
        }

        bool NodePortValueTypeIsAllowed(std::string_view value_type)
        {
            for (const auto allowed : NODE_PORT_VALUE_TYPES)
            {
                if (value_type == allowed)
                {
                    return true;
                }
            }
            return false;
        }

        /// The single point where the execution/data port category is
        /// derived. The rule recognizes exactly one Core-owned protocol
        /// literal: a port whose declared type is "signal" participates in
        /// execution flow, everything else carries data. This is the same
        /// signal/data vocabulary GraphEdgeDto.edge_type and signal-gated
        /// execution already define — the projection never interprets
        /// Provider semantics beyond this literal.
        std::string DerivePortCategory(std::string_view port_type)
        {
            return port_type == std::string_view("signal")
                       ? std::string(PORT_CATEGORY_EXECUTION)
                       : std::string(PORT_CATEGORY_DATA);
        }

        /// Resolve the execution/data category of a connect endpoint for the
        /// stored edge_type derivation. The canonical signal port names
        /// resolve execution outright — they are the wire-layer coordinate
        /// system, no component ever declares them; any other port resolves
        /// through the endpoint node's resolvable port surface, in the same
        /// precedence the compile path and the canvas projection apply:
        /// injected faces first (nodeFaces[本节点] — face 胜出同名声明口，
        /// 与 ResolveNodePorts/MapGraphView 同一知识源与去重规则：
        /// faces 口能连=编译不拒)，then the catalog-declared
        /// surface of the endpoint node's component, then the begin/end
        /// boundary ports stored in node settings. Nullopt = the endpoint
        /// names no resolvable port, and the change must reject rather than
        /// silently store a mistyped edge.
        std::optional<std::string> ResolveEndpointPortCategory(
            const Dto::GraphDocumentDto&        document,
            const std::optional<ActionCatalog>& action_catalog,
            const std::string&                  node_id,
            const std::string&                  port_id,
            const GraphCompiler::NodeFacesMap*  node_faces)
        {
            if (port_id == FormSequenceProjector::SignalInPort
                || port_id == FormSequenceProjector::SignalOutPort)
            {
                return std::string(PORT_CATEGORY_EXECUTION);
            }
            for (const auto& node : document.nodes)
            {
                if (node.node_id != node_id)
                {
                    continue;
                }
                // faces 注入口（消费与编译/投影同源）：nodeFaces[本节点]——
                // 命中按 DerivePortCategory(face.port_type) 归类。
                if (node_faces != nullptr)
                {
                    const auto found = node_faces->find(node.node_id);
                    if (found != node_faces->end())
                    {
                        for (const auto& face : found->second)
                        {
                            if (face.port_id == port_id)
                            {
                                return DerivePortCategory(face.port_type);
                            }
                        }
                    }
                }
                const ActionCatalogEntry* entry =
                    FindCatalogEntry(action_catalog, ComponentGuidOf(node));
                if (entry != nullptr)
                {
                    for (const auto& port : entry->ports)
                    {
                        if (port.port_id == port_id)
                        {
                            return DerivePortCategory(port.port_type);
                        }
                    }
                }
                // begin/end 边界口（节点 settings.boundaryPorts）：九词白
                // 名单不含 signal，category 恒为 data——边界口不在 catalog
                // 静态面里，connectGraphPorts 的边型推导据此解析，未声明
                // 的动态口端点照旧拒绝。
                for (const auto& entry : LoadNodeBoundaryPorts(node))
                {
                    if (entry.id == port_id)
                    {
                        return std::string(PORT_CATEGORY_DATA);
                    }
                }
                return std::nullopt;
            }
            return std::nullopt;
        }

        /// begin/end 动态口方向门：begin 的动态口只可作连线源（口对的值
        /// 来源是调用方图输入——图内入边占据它会把该图级输入从边界清单
        /// 顶掉，调用方传值被静默丢弃），end 的动态口只可作连线目标（out
        /// 侧是图输出坐标，不向图内发值）。返回违规描述，nullopt = 方向
        /// 合法（含非边界组件节点与静态目录口——静态口方向由目录声明携
        /// 带，ValidateEdgePorts 已按声明的 inputs/outputs 面校验）。
        std::optional<std::string> BoundaryDynamicPortDirectionViolation(
            const Dto::GraphDocumentDto& document,
            const std::string&           node_id,
            const std::string&           port_id,
            bool                         endpoint_is_source)
        {
            for (const auto& node : document.nodes)
            {
                if (node.node_id != node_id
                    || !node.target.component_ref.has_value())
                {
                    continue;
                }
                const std::string& component_guid =
                    node.target.component_ref->component_guid;
                const bool is_begin =
                    component_guid
                    == std::string_view(GRAPH_BEGIN_COMPONENT_GUID);
                const bool is_end =
                    component_guid
                    == std::string_view(GRAPH_END_COMPONENT_GUID);
                if (!is_begin && !is_end)
                {
                    return std::nullopt;
                }
                const auto boundary_entries = LoadNodeBoundaryPorts(node);
                const auto declared = std::find_if(
                    boundary_entries.begin(),
                    boundary_entries.end(),
                    [&](const BoundaryPortEntry& candidate)
                    { return candidate.id == port_id; });
                if (declared == boundary_entries.end())
                {
                    return std::nullopt; // 静态目录口，门不管。
                }
                if ((is_begin && !endpoint_is_source)
                    || (is_end && endpoint_is_source))
                {
                    const char* component_label = is_begin ? "Begin" : "End";
                    const char* allowed_side = is_begin ? "source" : "target";
                    const char* value_origin =
                        is_begin ? "caller graph input" : "graph output";
                    return std::string(
                               "connectGraphPorts endpoint direction "
                               "violation: dynamic port '")
                           + port_id + "' on " + component_label + " node '"
                           + node_id + "' is " + allowed_side
                           + "-only (the port pair's value source is the "
                           + value_origin + " side)";
                }
                return std::nullopt;
            }
            return std::nullopt;
        }

        /// The two engaged-only channels of an input data port, decided by
        /// the connected state. Unconnected: the default-value channel —
        /// when the node's settings carry a key matching the port id, the
        /// projected port exposes that raw value verbatim (no type
        /// conversion) so the UI can render and edit the port's current
        /// bound default; the settings residual survives for the
        /// disconnected state (reconnection default). Connected (an edge
        /// lands on that exact (node_id, port_id) endpoint): the
        /// source-indicator channel instead — the live value rides the wire
        /// so the residual must not resurface as an editable default, and
        /// the first matching incoming edge's source coordinates
        /// ("source_node_id::source_port_id") become the read-only origin
        /// the panel renders. Every other port shape — signal inputs,
        /// outputs, ports without a matching settings key — keeps both
        /// channels disengaged; a null settings value reads as "no value"
        /// (the same semantics the field projection gives nulls). The wire
        /// keys are then absent, never an explicit null.
        void AttachPortValueFromSettings(
            const yyjson::value& settings,
            const std::string&   node_id,
            const std::unordered_map<std::string, std::string>&
                           connected_input_sources,
            GraphNodePort& projected)
        {
            if (projected.direction != std::string_view(PORT_DIRECTION_INPUT)
                || projected.category != std::string_view(PORT_CATEGORY_DATA))
            {
                return;
            }
            const auto hit = connected_input_sources.find(
                node_id + "::" + projected.port_id);
            if (hit != connected_input_sources.end())
            {
                projected.connected_from = hit->second;
                return;
            }
            auto settings_obj = settings.as_object();
            if (!settings_obj.has_value()
                || !settings_obj->contains(std::string_view(projected.port_id)))
            {
                return;
            }
            const auto& settings_value =
                (*settings_obj)[std::string_view(projected.port_id)];
            if (settings_value.is_null())
            {
                return;
            }
            projected.value = Das::Utils::CloneYyjsonValue(settings_value);
        }

        GraphView MapGraphView(
            const Dto::GraphDocumentDto&        doc,
            const std::optional<ActionCatalog>& action_catalog,
            const GraphCompiler::NodeFacesMap*  node_faces)
        {
            GraphView view;
            // Connected input-port endpoints, keyed
            // "target_node_id::target_port_id" and mapped to the first
            // matching incoming edge's "source_node_id::source_port_id":
            // the exact coordinate pair both port channels gate on, so a
            // connected port never also projects an editable settings
            // default regardless of the edge type (the public connections
            // projection drops edge_type, so the UI client gate applies the
            // same type-agnostic rule).
            std::unordered_map<std::string, std::string>
                connected_input_sources;
            for (const auto& edge : doc.edges)
            {
                connected_input_sources.emplace(
                    edge.target_node_id + "::" + edge.target_port_id,
                    edge.source_node_id + "::" + edge.source_port_id);
            }
            view.nodes.reserve(doc.nodes.size());
            for (const auto& node : doc.nodes)
            {
                GraphNode out;
                out.id = node.node_id;
                out.component_guid = ComponentGuidOf(node);
                out.target_kind = TargetKindOf(node);

                // Catalog merge by componentGuid == actionId. The display
                // name follows the priority chain: the store user label (a
                // renameGraphNode write) wins; an unnamed node follows the
                // catalog default name so manifest kind renames propagate
                // automatically. description/icon merge from the matching
                // entry only, and the config declarations merge into the
                // fields projection (declared fields first).
                const ActionCatalogEntry* matched_entry =
                    FindCatalogEntry(action_catalog, out.component_guid);
                out.fields = ProjectFieldsWithConfig(
                    node.settings,
                    matched_entry != nullptr ? &matched_entry->config_fields
                                             : nullptr,
                    /*include_undeclared_live_keys=*/false);
                if (node.label.has_value() && !node.label->empty())
                {
                    out.label = *node.label;
                }
                else if (matched_entry != nullptr)
                {
                    out.label = matched_entry->label;
                }
                if (matched_entry != nullptr)
                {
                    out.description = matched_entry->description;
                    out.icon = matched_entry->icon;

                    // Registry-declared static port surface: the entry
                    // already carries the direction from the declaring
                    // inputs/outputs list. The category is derived here —
                    // the single derivation point — from the port's
                    // declared type.
                    out.ports.reserve(
                        out.ports.size() + matched_entry->ports.size());
                    for (const auto& port : matched_entry->ports)
                    {
                        GraphNodePort projected = port;
                        projected.category = DerivePortCategory(port.port_type);
                        AttachPortValueFromSettings(
                            node.settings,
                            node.node_id,
                            connected_input_sources,
                            projected);
                        out.ports.push_back(std::move(projected));
                    }
                }

                // begin/end 动态口投影分区读取段已随图文档口数据退役删
                // 除——画布动态口仅 faces 通道（下方 merge；begin/end 的
                // 文档口面由预备 pass 从节点 settings 建边界会话现算注
                // 入）。

                // faces 注入通道 merge（调用侧合并形态）：nodeFaces[本节
                // 点] 直接并入 GraphNodePort 列表——BuildPortsValue 的五
                // 键+dynamic 投影键形不变，faces 口以既有 dynamic 标记呈
                // 现。去重规则与编译侧一致：按口 id，face 胜出同名已投
                // 影口。绑条目节点的文档口面经插件侧 GetDocument 投影
                // （条目边界口现算现注入）供画布。
                const auto append_face_ports =
                    [&out, &node, &connected_input_sources](
                        const std::vector<GraphCompiler::DocumentFacePort>&
                            faces)
                {
                    for (const auto& face : faces)
                    {
                        const auto removed = std::remove_if(
                            out.ports.begin(),
                            out.ports.end(),
                            [&face](const GraphNodePort& projected)
                            { return projected.port_id == face.port_id; });
                        out.ports.erase(removed, out.ports.end());
                        GraphNodePort projected;
                        projected.port_id = face.port_id;
                        projected.display_label = face.label;
                        projected.port_type = face.port_type;
                        projected.direction = face.direction;
                        projected.category = DerivePortCategory(face.port_type);
                        projected.dynamic_port = true;
                        AttachPortValueFromSettings(
                            node.settings,
                            node.node_id,
                            connected_input_sources,
                            projected);
                        out.ports.push_back(std::move(projected));
                    }
                };
                if (node_faces != nullptr)
                {
                    const auto found = node_faces->find(node.node_id);
                    if (found != node_faces->end())
                    {
                        append_face_ports(found->second);
                    }
                }

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

    AuthoringDocument ToAuthoringDocument(
        const Dto::GraphDocumentDto&        document,
        const std::optional<ActionCatalog>& action_catalog,
        const GraphCompiler::NodeFacesMap*  node_faces)
    {
        AuthoringDocument out;
        out.revision = document.version;

        if (IsLinear(document))
        {
            out.view_kind = ViewKind::FormSequence;
            FormSequenceView view;
            view.items = ReverseProjectSequence(document, action_catalog);
            out.form_sequence = std::move(view);
        }
        else
        {
            out.view_kind = ViewKind::Graph;
            out.graph = MapGraphView(document, action_catalog, node_faces);
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
                if (f.type_word.has_value())
                {
                    fo["type"] = std::make_pair(
                        std::string_view(*f.type_word),
                        yyjson::copy_string);
                }
                // Optional, engaged-only: the declared target task type of a
                // component-domain field — emitted only when the manifest
                // declaration named one.
                if (f.task_guid.has_value())
                {
                    fo["taskGuid"] = std::make_pair(
                        std::string_view(*f.task_guid),
                        yyjson::copy_string);
                }
                if (f.description.has_value())
                {
                    fo["description"] = std::make_pair(
                        std::string_view(*f.description),
                        yyjson::copy_string);
                }
                fo["required"] = f.required;
                fo["readonly"] = f.readonly;
                // Optional, engaged-only: the closed candidate list of a
                // select/multiselect field — emitted only when the
                // projection engaged it (a config options declaration); a
                // field without one carries no key, never an explicit null.
                if (!f.candidates.empty())
                {
                    auto candidates_value = Das::Utils::MakeYyjsonArray();
                    auto candidates_array = candidates_value.as_array();
                    if (candidates_array)
                    {
                        for (const auto& candidate : f.candidates)
                        {
                            candidates_array->emplace_back(
                                Das::Utils::CloneYyjsonValue(candidate));
                        }
                    }
                    fo["candidates"] = std::move(candidates_value);
                }
                arr.emplace_back(std::move(f_v));
            }
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

        // Build the ports[] array as a standalone yyjson::value. Every element
        // carries the five locked keys (portId/displayLabel/portType/
        // direction/category) plus the two optional engaged-only channels —
        // the default-value key "value" (unconnected input data ports with a
        // matching settings key) and the source-indicator key "connectedFrom"
        // (input data ports an edge lands on) — each emitted only where the
        // projection engaged it; a node without ports still emits the key as
        // an empty array so the wire shape stays stable. Per
        // das-yyjson-string-safety every std::string payload is deep-copied
        // into the document arena.
        yyjson::value BuildPortsValue(const std::vector<GraphNodePort>& ports)
        {
            auto root = Das::Utils::MakeYyjsonArray();
            auto arr_o = root.as_array();
            if (!arr_o)
            {
                return root;
            }
            auto& arr = *arr_o;
            for (const auto& port : ports)
            {
                auto p_v = Das::Utils::MakeYyjsonObject();
                auto p_o = p_v.as_object();
                if (!p_o)
                {
                    continue;
                }
                auto po = *p_o;
                po["portId"] = std::make_pair(
                    std::string_view(port.port_id),
                    yyjson::copy_string);
                po["displayLabel"] = std::make_pair(
                    std::string_view(port.display_label),
                    yyjson::copy_string);
                po["portType"] = std::make_pair(
                    std::string_view(port.port_type),
                    yyjson::copy_string);
                po["direction"] = std::make_pair(
                    std::string_view(port.direction),
                    yyjson::copy_string);
                po["category"] = std::make_pair(
                    std::string_view(port.category),
                    yyjson::copy_string);
                // Optional, engaged-only: the emission gate mirrors the
                // attach gate so the key can never ride an output or
                // execution port.
                if (port.value.has_value()
                    && port.direction == std::string_view(PORT_DIRECTION_INPUT)
                    && port.category == std::string_view(PORT_CATEGORY_DATA))
                {
                    po["value"] = Das::Utils::CloneYyjsonValue(*port.value);
                }
                // Optional, engaged-only source indicator: the mirror gate
                // of the value channel for the connected state — a plain
                // string, deep-copied per das-yyjson-string-safety.
                if (!port.connected_from.empty()
                    && port.direction == std::string_view(PORT_DIRECTION_INPUT)
                    && port.category == std::string_view(PORT_CATEGORY_DATA))
                {
                    po["connectedFrom"] = std::make_pair(
                        std::string_view(port.connected_from),
                        yyjson::copy_string);
                }
                // Optional, engaged-only dynamic-port marker (D-16 merge):
                // emitted only on projected dynamic ports — a static manifest
                // port carries no key, never an explicit false.
                if (port.dynamic_port)
                {
                    po["dynamic"] = true;
                }
                arr.emplace_back(std::move(p_v));
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
            // Presentation metadata: the label is emitted only when a
            // source (store user label or catalog default name) provided a
            // non-empty value — never an empty string, clients fall back to
            // the node id themselves. description/icon are engaged-only
            // catalog merges.
            if (!node.label.empty())
            {
                obj["label"] = std::make_pair(
                    std::string_view(node.label),
                    yyjson::copy_string);
            }
            if (node.description.has_value())
            {
                obj["description"] = std::make_pair(
                    std::string_view(*node.description),
                    yyjson::copy_string);
            }
            if (node.icon.has_value())
            {
                obj["icon"] = std::make_pair(
                    std::string_view(*node.icon),
                    yyjson::copy_string);
            }
            if (!node.component_guid.empty())
            {
                obj["componentGuid"] = std::make_pair(
                    std::string_view(node.component_guid),
                    yyjson::copy_string);
            }
            obj["targetKind"] = std::make_pair(
                std::string_view(node.target_kind),
                yyjson::copy_string);
            if (!node.fields.empty())
            {
                obj["fields"] = BuildFieldsValue(node.fields);
            }
            obj["ports"] = BuildPortsValue(node.ports);
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
                        // Build actionIds[] element by element: every string
                        // is written through the (string, copy_string)
                        // primitive path so quotes, backslashes and control
                        // characters in an action id cannot corrupt the
                        // array (hand-building a JSON literal would).
                        auto aids = Das::Utils::MakeYyjsonArray();
                        auto aids_arr = aids.as_array();
                        for (const auto& action_id : group.action_ids)
                        {
                            aids_arr->emplace_back(
                                std::string_view(action_id),
                                yyjson::copy_string);
                        }
                        go[std::string_view("actionIds")] = std::move(aids);
                        groups_arr.emplace_back(std::move(group_v));
                    }
                }
            }
            return root;
        }
    } // anonymous namespace

    yyjson::value SerializeDocument(
        const Dto::GraphDocumentDto&                        document,
        const std::optional<ActionCatalog>&                 action_catalog,
        const GraphCompiler::NodeFacesMap*                  node_faces,
        const std::vector<GraphCompiler::DocumentFaceType>* entry_face_types)
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
                    for (const auto& item :
                         ReverseProjectSequence(document, action_catalog))
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
            const auto view =
                MapGraphView(document, action_catalog, node_faces);
            auto g_v = Das::Utils::MakeYyjsonObject();
            auto g_o = g_v.as_object();
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

            // types 注册表（graph 视图投影）：内建九词 ∪ faces 通道携带的
            // 文档 types[]——形状照根文档 types section（{name[,
            // compatibleWith]}）。消费面=连线规则表（Core 侧供源，UI 侧
            // 双表退役）。同名内建词冲突：内建胜出（注册表须恒过同文档
            // 唯一校验），冲突自定义条目丢弃 + WARN。
            {
                yyjson::value types_value(Das::Utils::MakeYyjsonArray());
                auto          types_arr = types_value.as_array();
                const auto    append_type =
                    [&types_arr](
                        std::string_view                name,
                        const std::vector<std::string>* compatible_with)
                {
                    yyjson::value entry(Das::Utils::MakeYyjsonObject());
                    auto          entry_obj = entry.as_object();
                    (*entry_obj)[std::string_view("name")] =
                        yyjson::value(std::string(name));
                    if (compatible_with != nullptr && !compatible_with->empty())
                    {
                        yyjson::value compat_value(
                            Das::Utils::MakeYyjsonArray());
                        auto compat_arr = compat_value.as_array();
                        for (const auto& word : *compatible_with)
                        {
                            compat_arr->emplace_back(yyjson::value(word));
                        }
                        (*entry_obj)[std::string_view("compatibleWith")] =
                            std::move(compat_value);
                    }
                    types_arr->emplace_back(std::move(entry));
                };
                for (const auto builtin : NODE_PORT_VALUE_TYPES)
                {
                    append_type(builtin, nullptr);
                }
                if (entry_face_types != nullptr)
                {
                    for (const auto& custom : *entry_face_types)
                    {
                        const bool collides_with_builtin = std::any_of(
                            NODE_PORT_VALUE_TYPES.begin(),
                            NODE_PORT_VALUE_TYPES.end(),
                            [&custom](std::string_view builtin)
                            { return builtin == custom.name; });
                        if (collides_with_builtin)
                        {
                            DAS_CORE_LOG_WARN(
                                "custom type name {} collides with a "
                                "built-in word; the built-in wins and the "
                                "custom entry is dropped from the registry",
                                custom.name);
                            continue;
                        }
                        append_type(custom.name, &custom.compatible_with);
                    }
                }
                root_obj["types"] = std::move(types_value);
            }

            // 推导边界 ports section（graph 视图投影，engaged-only）：复用
            // 编译侧 DeriveBoundaryPorts 同一函数（单一事实源——投影与编
            // 译永不漂移；本语境不绑 catalog provider，推导贡献=注入的
            // begin/end 文档口面）——begin faces→direction=input、end
            // faces→direction=output（推导层的边界排除规则挡下口对镜像
            // 侧）。图条目被调面由此统一「文档自述」（与 MaaPI 推导
            // ports section 同构），预备 pass 的抽取路径零改动即可命中；
            // faces 缺席（standalone 打开、无预备 pass 注入）时整 section
            // 缺省——standalone 图文档投影与现状逐字节一致。口 id 跨侧
            // 冲突（begin/end 动态口 id 是各自节点内计数器，同名可撞）：
            // 发射端零兜底——重名条目照实双双发射，撞上根 ports section
            // 契约（id 同文档唯一）时由消费侧校验整文档拒绝
            // （duplicatePortId）；产出重名口是组件 authoring 会话的实现
            // 不规范，由报错暴露而非静默跳过（自动分配侧别前缀后正常
            // 路径物理不撞）。
            if (node_faces != nullptr && !node_faces->empty())
            {
                GraphCompiler compiler;
                compiler.BindNodeFaces(node_faces);
                const auto    boundary = compiler.DeriveBoundaryPorts(document);
                yyjson::value ports_value(Das::Utils::MakeYyjsonArray());
                auto          ports_arr_o = ports_value.as_array();
                if (ports_arr_o)
                {
                    auto&      ports_arr = *ports_arr_o;
                    const auto append_boundary_port =
                        [&ports_arr, entry_face_types](
                            const Dto::BoundaryPortDto& port,
                            std::string_view            direction)
                    {
                        const bool builtin_type = std::any_of(
                            NODE_PORT_VALUE_TYPES.begin(),
                            NODE_PORT_VALUE_TYPES.end(),
                            [&port](std::string_view builtin)
                            { return builtin == port.port_type; });
                        const bool registered_type =
                            entry_face_types != nullptr
                            && std::any_of(
                                entry_face_types->begin(),
                                entry_face_types->end(),
                                [&port](
                                    const GraphCompiler::DocumentFaceType&
                                        custom)
                                { return custom.name == port.port_type; });
                        if (!builtin_type && !registered_type)
                        {
                            DAS_CORE_LOG_WARN(
                                "derived boundary port {} carries type {} "
                                "which the root type registry cannot resolve; "
                                "the port is skipped from the projected "
                                "ports section",
                                port.port_id,
                                port.port_type);
                            return;
                        }
                        yyjson::value entry(Das::Utils::MakeYyjsonObject());
                        auto          entry_obj = entry.as_object();
                        (*entry_obj)[std::string_view("id")] = std::make_pair(
                            std::string_view(port.port_id),
                            yyjson::copy_string);
                        if (!port.display_label.empty()
                            && port.display_label != port.port_id)
                        {
                            (*entry_obj)[std::string_view("label")] =
                                std::make_pair(
                                    std::string_view(port.display_label),
                                    yyjson::copy_string);
                        }
                        (*entry_obj)[std::string_view("type")] = std::make_pair(
                            std::string_view(port.port_type),
                            yyjson::copy_string);
                        (*entry_obj)[std::string_view("direction")] =
                            std::make_pair(
                                std::string_view(direction),
                                yyjson::copy_string);
                        ports_arr.emplace_back(std::move(entry));
                    };
                    for (const auto& port : boundary.inputs)
                    {
                        append_boundary_port(port, std::string_view("input"));
                    }
                    for (const auto& port : boundary.outputs)
                    {
                        append_boundary_port(port, std::string_view("output"));
                    }
                    if (!ports_arr.empty())
                    {
                        root_obj["ports"] = std::move(ports_value);
                    }
                }
            }
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
        // 显式以 value 接住（auto 会推导成 object 句柄，宿主 document 临
        // 时在语句尾析构——句柄即悬空）。转换发生在完整表达式内，value
        // 自持 arena。节点不携带口数据（图文档零口数据——边界口列表在
        // settings.boundaryPorts 里随 settings 序列化），聚合反射不再有
        // 空分区键需要擦除。
        yyjson::value serialized =
            yyjson::object(document, yyjson::copy_string);
        return serialized;
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

        /// 线性⊂图法则拒绝消息（定文，字节精确）：线性 change kinds 与
        /// graph→formSequence 的 convertView 对图文档一律拒绝——图文档
        /// 永不转换为线性，也永不进线性变更路径。
        constexpr std::string_view LINEAR_KINDS_NOT_APPLICABLE_TO_GRAPH =
            "linear change kinds are not applicable to graph documents; "
            "a graph is never converted to linear";

        /// True when the change kind only ever mutates a linear
        /// (formSequence) document: the item family plus the linear setValue
        /// targeting a formSequenceItem. A graph document never enters the
        /// linear mutation path — the dispatch front door rejects these kinds
        /// up front, so the store can never be rebuilt through the linear
        /// reverse projection (which would drop viewData, node labels and
        /// dynamic port partitions).
        bool IsLinearOnlyChange(const AuthoringChange& change) noexcept
        {
            if (change.kind == "addFormSequenceItem"
                || change.kind == "removeFormSequenceItem"
                || change.kind == "moveFormSequenceItem")
            {
                return true;
            }
            return change.kind == "setValue" && change.target.has_value()
                   && change.target->kind == "formSequenceItem";
        }

        /// View-only kinds ride the lightweight viewData channel and skip the
        /// semantic revision bump so coordinate updates never participate in
        /// revision conflict detection (singular moveGraphNode and the
        /// moveGraphNodes batch share the exemption family). Every other kind
        /// keeps the success-bumps-revision invariant.
        bool BumpsRevision(std::string_view kind) noexcept
        {
            return kind != "moveGraphNode" && kind != "moveGraphNodes";
        }

        /// Erase viewData.positions[node_id] when present so node removal
        /// never leaves an orphan coordinate entry in the persisted
        /// partition.
        void EraseViewPosition(
            Dto::GraphDocumentDto& document,
            std::string_view       node_id)
        {
            auto view = document.view_data.as_object();
            if (!view || !view->contains(std::string_view("positions")))
            {
                return;
            }
            auto positions = (*view)[std::string_view("positions")].as_object();
            if (positions)
            {
                positions->erase(node_id);
            }
        }

        /// Linear node existence lookup over the authoritative store.
        bool NodeExists(
            const Dto::GraphDocumentDto& document,
            std::string_view             node_id) noexcept
        {
            for (const auto& node : document.nodes)
            {
                if (node.node_id == node_id)
                {
                    return true;
                }
            }
            return false;
        }

        /// Defaults for disengaged batch slots — iterating over these is a
        /// no-op, keeping the batch loops branch-free.
        const std::vector<std::string>     EMPTY_NODE_IDS{};
        const std::vector<GraphConnection> EMPTY_CONNECTIONS{};

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
            Dto::GraphDocumentDto&              document,
            const AuthoringChange&              change,
            const std::vector<std::string>&     valid_action_ids,
            const std::optional<ActionCatalog>& action_catalog,
            const GraphCompiler::NodeFacesMap*  node_faces)
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
                auto node_result = ApplySettingsChange(document, gac);
                if (!node_result.Ok())
                {
                    return {
                        MapGraphError(node_result.error_kind),
                        std::move(node_result.message),
                        {},
                        std::nullopt};
                }
                // Node removal must not leave an orphan coordinate entry
                // behind in the persisted viewData partition.
                EraseViewPosition(document, *change.item_or_node_id);
                return {ChangeErrorKind::None, "", {}, std::nullopt};
            }
            if (change.kind == "removeGraphNodes")
            {
                const auto& batch_node_ids = change.removed_node_ids;
                const auto& batch_connections = change.removed_connections;
                const bool  has_node_ids =
                    batch_node_ids.has_value() && !batch_node_ids->empty();
                const bool has_connections = batch_connections.has_value()
                                             && !batch_connections->empty();
                if (!has_node_ids && !has_connections)
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "removeGraphNodes requires at least one nodeIds "
                        "entry or connections entry",
                        {},
                        std::nullopt};
                }
                // Atomic batch: validate ALL node ids and resolve ALL
                // connections before touching the store — any miss rejects
                // the whole message with zero mutation.
                for (const auto& node_id :
                     batch_node_ids.value_or(EMPTY_NODE_IDS))
                {
                    if (!NodeExists(document, node_id))
                    {
                        return {
                            ChangeErrorKind::NodeNotFound,
                            "removeGraphNodes node not found: " + node_id,
                            {},
                            std::nullopt};
                    }
                }
                std::vector<std::string> edge_ids;
                for (const auto& connection :
                     batch_connections.value_or(EMPTY_CONNECTIONS))
                {
                    std::string edge_id;
                    for (const auto& edge : document.edges)
                    {
                        if (edge.source_node_id == connection.from_node_id
                            && edge.source_port_id == connection.from_port_id
                            && edge.target_node_id == connection.to_node_id
                            && edge.target_port_id == connection.to_port_id)
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
                    // Duplicate connection entries are idempotent — the
                    // second disconnect would otherwise fail mid-mutation
                    // and break batch atomicity.
                    if (std::find(edge_ids.begin(), edge_ids.end(), edge_id)
                        == edge_ids.end())
                    {
                        edge_ids.push_back(std::move(edge_id));
                    }
                }
                // Mutation phase: explicit connections first (their endpoint
                // nodes survive), then node removal with edge cascade and
                // coordinate cleanup.
                for (const auto& edge_id : edge_ids)
                {
                    gac.payload = DisconnectPortsChange{edge_id};
                    auto edge_result = ApplySettingsChange(document, gac);
                    if (!edge_result.Ok())
                    {
                        return {
                            MapGraphError(edge_result.error_kind),
                            std::move(edge_result.message),
                            {},
                            std::nullopt};
                    }
                }
                for (const auto& node_id :
                     batch_node_ids.value_or(EMPTY_NODE_IDS))
                {
                    gac.payload = RemoveNodeChange{node_id};
                    auto node_result = ApplySettingsChange(document, gac);
                    if (!node_result.Ok())
                    {
                        return {
                            MapGraphError(node_result.error_kind),
                            std::move(node_result.message),
                            {},
                            std::nullopt};
                    }
                    EraseViewPosition(document, node_id);
                }
                return {ChangeErrorKind::None, "", {}, std::nullopt};
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
                const auto& conn = *change.connection;
                // Server-side edge-type derivation: the stored type follows
                // the endpoint port categories (canonical signal port names
                // resolve execution with no declaration; any other endpoint
                // resolves through the catalog-declared surface). An
                // unresolved endpoint or an execution/data mismatch rejects
                // the change — a silent data fallback would store a
                // sequence wire the compiler then treats as data. Node
                // existence stays HandleConnectPorts' rejection, so a
                // missing node skips the category check and fails
                // downstream with the established error kind; the payload's
                // placeholder type never reaches the store on that path.
                std::string edge_type = "data";
                if (NodeExists(document, conn.from_node_id)
                    && NodeExists(document, conn.to_node_id))
                {
                    // 方向门先于类别推导：错向端点（图内边指向 begin 动态
                    // 口/从 end 动态口发出）静默废掉图级输入，必须显性拒绝。
                    if (const auto source_violation =
                            BoundaryDynamicPortDirectionViolation(
                                document,
                                conn.from_node_id,
                                conn.from_port_id,
                                /*endpoint_is_source=*/true);
                        source_violation.has_value())
                    {
                        return {
                            ChangeErrorKind::InvalidOp,
                            *source_violation,
                            {},
                            std::nullopt};
                    }
                    if (const auto target_violation =
                            BoundaryDynamicPortDirectionViolation(
                                document,
                                conn.to_node_id,
                                conn.to_port_id,
                                /*endpoint_is_source=*/false);
                        target_violation.has_value())
                    {
                        return {
                            ChangeErrorKind::InvalidOp,
                            *target_violation,
                            {},
                            std::nullopt};
                    }
                    const auto source_category = ResolveEndpointPortCategory(
                        document,
                        action_catalog,
                        conn.from_node_id,
                        conn.from_port_id,
                        node_faces);
                    if (!source_category.has_value())
                    {
                        return {
                            ChangeErrorKind::InvalidOp,
                            "connectGraphPorts source port not declared: node "
                                + conn.from_node_id + " port "
                                + conn.from_port_id,
                            {},
                            std::nullopt};
                    }
                    const auto target_category = ResolveEndpointPortCategory(
                        document,
                        action_catalog,
                        conn.to_node_id,
                        conn.to_port_id,
                        node_faces);
                    if (!target_category.has_value())
                    {
                        return {
                            ChangeErrorKind::InvalidOp,
                            "connectGraphPorts target port not declared: node "
                                + conn.to_node_id + " port " + conn.to_port_id,
                            {},
                            std::nullopt};
                    }
                    if (*source_category != *target_category)
                    {
                        return {
                            ChangeErrorKind::InvalidOp,
                            "connectGraphPorts endpoint categories mismatch: "
                            "from node "
                                + conn.from_node_id + " port "
                                + conn.from_port_id + " (" + *source_category
                                + ") vs to node " + conn.to_node_id + " port "
                                + conn.to_port_id + " (" + *target_category
                                + ")",
                            {},
                            std::nullopt};
                    }
                    edge_type =
                        *source_category
                                == std::string_view(PORT_CATEGORY_EXECUTION)
                            ? "signal"
                            : "data";
                    if (edge_type == "signal")
                    {
                        // Sequence outlets are out-degree 1: a second signal
                        // edge from the same source port forks the unique
                        // next-step, and the compiler has no ordering to
                        // arbitrate the fork. Data fan-out (one outlet
                        // feeding many consumers) and signal in-edge
                        // merging stay legal — signal and data ports are
                        // category-mutually-exclusive, so matching the
                        // stored edge_type keeps a legacy data edge on an
                        // execution port out of the count.
                        for (const auto& edge : document.edges)
                        {
                            if (edge.source_node_id == conn.from_node_id
                                && edge.source_port_id == conn.from_port_id
                                && edge.edge_type == "signal")
                            {
                                return {
                                    ChangeErrorKind::InvalidOp,
                                    "connectGraphPorts execution outlet "
                                    "allows a single out edge: node "
                                        + conn.from_node_id + " port "
                                        + conn.from_port_id
                                        + " already has a signal edge",
                                    {},
                                    std::nullopt};
                            }
                        }
                    }
                }
                ConnectPortsChange c;
                c.edge.edge_id =
                    "edge::" + conn.from_node_id + "::" + conn.from_port_id
                    + "::" + conn.to_node_id + "::" + conn.to_port_id;
                c.edge.source_node_id = conn.from_node_id;
                c.edge.source_port_id = conn.from_port_id;
                c.edge.target_node_id = conn.to_node_id;
                c.edge.target_port_id = conn.to_port_id;
                c.edge.edge_type = std::move(edge_type);
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
                // Unknown nodes are rejected instead of writing an orphan
                // coordinate entry into the persisted partition.
                if (!NodeExists(document, *change.item_or_node_id))
                {
                    return {
                        ChangeErrorKind::NodeNotFound,
                        "moveGraphNode node not found: "
                            + *change.item_or_node_id,
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
            else if (change.kind == "moveGraphNodes")
            {
                if (!change.node_moves.has_value()
                    || change.node_moves->empty())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "moveGraphNodes.moves required",
                        {},
                        std::nullopt};
                }
                // Atomic batch: every node must exist before any coordinate
                // is written so a rejected batch never leaks a partial
                // position update.
                for (const auto& node_move : *change.node_moves)
                {
                    if (!NodeExists(document, node_move.node_id))
                    {
                        return {
                            ChangeErrorKind::NodeNotFound,
                            "moveGraphNodes node not found: "
                                + node_move.node_id,
                            {},
                            std::nullopt};
                    }
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
                for (const auto& node_move : *change.node_moves)
                {
                    auto position = Das::Utils::MakeYyjsonObject();
                    auto position_obj = position.as_object();
                    if (position_obj)
                    {
                        (*position_obj)[std::string_view("x")] =
                            node_move.position_x;
                        (*position_obj)[std::string_view("y")] =
                            node_move.position_y;
                    }
                    (*positions)[std::string_view(node_move.node_id)] =
                        std::move(position);
                }
                return {ChangeErrorKind::None, "", {}, std::nullopt};
            }
            else if (change.kind == "renameGraphNode")
            {
                if (!change.item_or_node_id.has_value()
                    || change.item_or_node_id->empty())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "renameGraphNode.nodeId required",
                        {},
                        std::nullopt};
                }
                if (!change.rename_label.has_value()
                    || change.rename_label->empty())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "renameGraphNode.label required",
                        {},
                        std::nullopt};
                }
                // The label is pure presentation data written straight onto
                // the store node — the addressing surface (node id,
                // viewData.positions keys, edge endpoints) is untouched. A
                // missing node rejects the whole change with zero mutation
                // (no orphan label ever enters the store).
                bool found = false;
                for (auto& node : document.nodes)
                {
                    if (node.node_id == *change.item_or_node_id)
                    {
                        node.label = *change.rename_label;
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    return {
                        ChangeErrorKind::NodeNotFound,
                        "renameGraphNode node not found: "
                            + *change.item_or_node_id,
                        {},
                        std::nullopt};
                }
                return {ChangeErrorKind::None, "", {}, std::nullopt};
            }
            else if (change.kind == "addNodePort")
            {
                if (!change.item_or_node_id.has_value()
                    || change.item_or_node_id->empty())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "addNodePort.nodeId required",
                        {},
                        std::nullopt};
                }
                if (!change.node_port_name.has_value())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "addNodePort.name required",
                        {},
                        std::nullopt};
                }
                const auto& direction = change.node_port_direction;
                if (!direction.has_value()
                    || (*direction != std::string_view(PORT_DIRECTION_INPUT)
                        && *direction
                               != std::string_view(PORT_DIRECTION_OUTPUT)))
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "addNodePort.direction must be input|output",
                        {},
                        std::nullopt};
                }
                const auto& value_type = change.node_port_value_type;
                if (!value_type.has_value()
                    || !NodePortValueTypeIsAllowed(*value_type))
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "addNodePort.valueType must be one of bool/int/int64/"
                        "float/double/string/image/base/component",
                        {},
                        std::nullopt};
                }
                // 校验全部前置后才找节点：任何拒绝都零突变。
                Dto::GraphNodeDto* target_node = nullptr;
                for (auto& node : document.nodes)
                {
                    if (node.node_id == *change.item_or_node_id)
                    {
                        target_node = &node;
                        break;
                    }
                }
                if (target_node == nullptr)
                {
                    return {
                        ChangeErrorKind::NodeNotFound,
                        "addNodePort node not found: "
                            + *change.item_or_node_id,
                        {},
                        std::nullopt};
                }
                // 图文档零口存储域：仅 begin/end 节点持口列表（节点
                // settings.boundaryPorts——边界会话状态域）；其它节点的
                // 动态口属其自己的 authoring 文档，图文档无处可写——
                // fail-closed 拒绝并指认所属文档（零突变）。
                const std::string target_component_guid =
                    ComponentGuidOf(*target_node);
                if (!IsBoundaryComponent(target_component_guid))
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "graph documents store no port partitions; edit the "
                        "port list in the owning document (node: "
                            + *change.item_or_node_id + ")",
                        {},
                        std::nullopt};
                }
                // 方向门（begin/end 文档校验规则）：direction 是签名方
                // 向——begin 只收入参口、end 只收出参口（角色固化，任何
                // 变更路径都产不出反向口）。
                {
                    const bool direction_mirrors_role =
                        (*direction == std::string_view(PORT_DIRECTION_INPUT))
                        == (target_component_guid
                            == std::string_view(GRAPH_BEGIN_COMPONENT_GUID));
                    if (!direction_mirrors_role)
                    {
                        return {
                            ChangeErrorKind::InvalidOp,
                            "addNodePort.direction mismatch: Begin accepts "
                            "input ports, End accepts output ports",
                            {},
                            std::nullopt};
                    }
                }
                if (const auto name_violation =
                        NodePortNameViolation(*change.node_port_name);
                    name_violation.has_value())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "addNodePort.name " + *name_violation,
                        {},
                        std::nullopt};
                }
                // 边界条目单 id 形态（{id,type}）：portId 在场且与 name
                // 分歧即拒绝——口 id 与口名是同一通道。
                if (change.node_port_id.has_value()
                    && *change.node_port_id != *change.node_port_name)
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "addNodePort.portId must equal name for boundary "
                        "ports (the port id and its name are one)",
                        {},
                        std::nullopt};
                }
                auto boundary_entries = LoadNodeBoundaryPorts(*target_node);
                for (const auto& existing_entry : boundary_entries)
                {
                    if (existing_entry.id == *change.node_port_name)
                    {
                        return {
                            ChangeErrorKind::InvalidOp,
                            "addNodePort.name duplicates an existing port "
                            "id on the node: "
                                + *change.node_port_name,
                            {},
                            std::nullopt};
                    }
                }
                boundary_entries.push_back(
                    BoundaryPortEntry{*change.node_port_name, *value_type});
                StoreNodeBoundaryPorts(*target_node, boundary_entries);
                return {
                    ChangeErrorKind::None,
                    "",
                    {},
                    std::optional<std::string>{*change.node_port_name}};
            }
            else if (change.kind == "removeNodePort")
            {
                if (!change.item_or_node_id.has_value()
                    || change.item_or_node_id->empty())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "removeNodePort.nodeId required",
                        {},
                        std::nullopt};
                }
                if (!change.node_port_id.has_value()
                    || change.node_port_id->empty())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "removeNodePort.portId required",
                        {},
                        std::nullopt};
                }
                Dto::GraphNodeDto* target_node = nullptr;
                for (auto& node : document.nodes)
                {
                    if (node.node_id == *change.item_or_node_id)
                    {
                        target_node = &node;
                        break;
                    }
                }
                if (target_node == nullptr)
                {
                    return {
                        ChangeErrorKind::NodeNotFound,
                        "removeNodePort node not found: "
                            + *change.item_or_node_id,
                        {},
                        std::nullopt};
                }
                // 边界口列表命中判定（settings.boundaryPorts）：静态
                // manifest 口不在口列表（如 end 的「值」）——remove 只删
                // 边界条目，拒绝而非静默误删声明面。
                auto boundary_entries = LoadNodeBoundaryPorts(*target_node);
                const auto removed_entry = std::find_if(
                    boundary_entries.begin(),
                    boundary_entries.end(),
                    [&](const BoundaryPortEntry& entry)
                    { return entry.id == *change.node_port_id; });
                if (removed_entry == boundary_entries.end())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "removeNodePort port is not declared on the node's "
                        "boundary list: "
                            + *change.node_port_id,
                        {},
                        std::nullopt};
                }
                boundary_entries.erase(removed_entry);
                StoreNodeBoundaryPorts(*target_node, boundary_entries);
                // 级联删线：引用该口 id 的连线（源侧或目标侧）在同一
                // change 内原子删除——与 removeGraphNodes 的级联同语义。
                const auto& removed_port_id = *change.node_port_id;
                document.edges.erase(
                    std::remove_if(
                        document.edges.begin(),
                        document.edges.end(),
                        [&](const Dto::GraphEdgeDto& edge)
                        {
                            return (edge.source_node_id
                                        == *change.item_or_node_id
                                    && edge.source_port_id == removed_port_id)
                                   || (edge.target_node_id
                                           == *change.item_or_node_id
                                       && edge.target_port_id
                                              == removed_port_id);
                        }),
                    document.edges.end());
                return {ChangeErrorKind::None, "", {}, std::nullopt};
            }
            else if (change.kind == "renameNodePort")
            {
                if (!change.item_or_node_id.has_value()
                    || change.item_or_node_id->empty())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "renameNodePort.nodeId required",
                        {},
                        std::nullopt};
                }
                if (!change.node_port_id.has_value()
                    || change.node_port_id->empty())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "renameNodePort.portId required",
                        {},
                        std::nullopt};
                }
                if (!change.node_port_name.has_value())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "renameNodePort.name required",
                        {},
                        std::nullopt};
                }
                if (const auto name_violation =
                        NodePortNameViolation(*change.node_port_name);
                    name_violation.has_value())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "renameNodePort.name " + *name_violation,
                        {},
                        std::nullopt};
                }
                Dto::GraphNodeDto* target_node = nullptr;
                for (auto& node : document.nodes)
                {
                    if (node.node_id == *change.item_or_node_id)
                    {
                        target_node = &node;
                        break;
                    }
                }
                if (target_node == nullptr)
                {
                    return {
                        ChangeErrorKind::NodeNotFound,
                        "renameNodePort node not found: "
                            + *change.item_or_node_id,
                        {},
                        std::nullopt};
                }
                // 边界条目单 id 形态：rename 改条目 id（口 id 即寻址坐
                // 标），该节点上引用旧口 id 的连线坐标同步改写——寻址面
                // 与口面同步演进；拒绝路径零突变。
                auto boundary_entries = LoadNodeBoundaryPorts(*target_node);
                const auto target_entry = std::find_if(
                    boundary_entries.begin(),
                    boundary_entries.end(),
                    [&](const BoundaryPortEntry& entry)
                    { return entry.id == *change.node_port_id; });
                if (target_entry == boundary_entries.end())
                {
                    return {
                        ChangeErrorKind::InvalidOp,
                        "renameNodePort port is not declared on the node's "
                        "boundary list: "
                            + *change.node_port_id,
                        {},
                        std::nullopt};
                }
                // 重名门排除自身：改成同名是幂等 no-op 语义（仍是成功、
                // revision 照常 bump——与 renameGraphNode 同惯例）。
                for (auto iter = boundary_entries.begin();
                     iter != boundary_entries.end();
                     ++iter)
                {
                    if (iter != target_entry
                        && iter->id == *change.node_port_name)
                    {
                        return {
                            ChangeErrorKind::InvalidOp,
                            "renameNodePort.name duplicates an existing port "
                            "id on the node: "
                                + *change.node_port_name,
                            {},
                            std::nullopt};
                    }
                }
                const std::string old_port_id = target_entry->id;
                target_entry->id = *change.node_port_name;
                StoreNodeBoundaryPorts(*target_node, boundary_entries);
                for (auto& edge : document.edges)
                {
                    if (edge.source_node_id == *change.item_or_node_id
                        && edge.source_port_id == old_port_id)
                    {
                        edge.source_port_id = *change.node_port_name;
                    }
                    if (edge.target_node_id == *change.item_or_node_id
                        && edge.target_port_id == old_port_id)
                    {
                        edge.target_port_id = *change.node_port_name;
                    }
                }
                return {ChangeErrorKind::None, "", {}, std::nullopt};
            }
            else if (change.kind == "convertView")
            {
                // 线性⊂图法则：graph→formSequence 的 best-effort 翻转
                // （LINEAR_TAG 塞入段）已退役——单链是图特例，只有
                // formSequence→graph 的无损升级方向存在（线性派发段的
                // RemoveTag 语义保留）。图文档一律 InvalidOp 拒绝，
                // 拒绝零突变。
                return {
                    ChangeErrorKind::InvalidOp,
                    std::string(LINEAR_KINDS_NOT_APPLICABLE_TO_GRAPH),
                    {},
                    std::nullopt};
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
        Dto::GraphDocumentDto&              document,
        const AuthoringChange&              change,
        const std::vector<std::string>&     valid_action_ids,
        const std::optional<ActionCatalog>& action_catalog,
        const GraphCompiler::NodeFacesMap*  node_faces)
    {
        // 线性⊂图法则前门：图文档 + 线性 change kinds（item 家族与
        // formSequenceItem 定向 setValue）一律 InvalidOp 拒绝，拒绝零突
        // 变——图文档根本不进线性变更路径（线性反向投影重建会丢
        // viewData/节点 label/动态口分区，守卫使该路径不可达）。
        if (!IsLinearMode(document) && IsLinearOnlyChange(change))
        {
            return {
                ChangeErrorKind::InvalidOp,
                std::string(LINEAR_KINDS_NOT_APPLICABLE_TO_GRAPH),
                {},
                std::nullopt};
        }
        AuthoringChangeResult r =
            IsLinearMode(document)
                ? ApplyLinearChange(document, change, valid_action_ids)
                : ApplyGraphChange(
                      document,
                      change,
                      valid_action_ids,
                      action_catalog,
                      node_faces);
        if (r.Ok() && BumpsRevision(change.kind))
        {
            ++document.version;
        }
        return r;
    }
} // namespace Contract

DAS_CORE_GRAPHRUNTIME_NS_END
