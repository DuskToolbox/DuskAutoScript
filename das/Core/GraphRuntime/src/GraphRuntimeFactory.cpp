#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>

#include "GraphBoundaryAuthoringSession.h"
#include <cpp_yyjson.hpp>
#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>
#include <das/Core/GraphRuntime/AuthoringDocContract.h>
#include <das/Core/GraphRuntime/CompiledPlanFingerprint.h>
#include <das/Core/GraphRuntime/FormSequenceProjector.h>
#include <das/Core/GraphRuntime/GraphAuthoring.h>
#include <das/Core/GraphRuntime/GraphBoundaryComponents.h>
#include <das/Core/GraphRuntime/GraphCompiler.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/Utils/DasJsonCore.h>
#include <new>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// --- GraphRuntimeImpl implementation ---

GraphRuntimeImpl::GraphRuntimeImpl(
    Das::PluginInterface::IDasTaskComponentHost* p_host)
    : host_(p_host)
{
}

DasResult GraphRuntimeImpl::GetErrorMessage(
    IDasReadOnlyString** pp_out_error_message)
{
    if (!pp_out_error_message)
    {
        return DAS_E_INVALID_POINTER;
    }

    return CreateIDasReadOnlyStringFromUtf8(
        last_error_.c_str(),
        pp_out_error_message);
}

DasResult GraphRuntimeImpl::GetLastExecutedNodeId(
    IDasReadOnlyString** pp_out_node_id)
{
    if (!pp_out_node_id)
    {
        return DAS_E_INVALID_POINTER;
    }

    return CreateIDasReadOnlyStringFromUtf8(
        engine_.GetLastExecutedNodeId().c_str(),
        pp_out_node_id);
}

DasResult GraphRuntimeImpl::ParseVerifiedPlan(
    IDasReadOnlyString*        p_compiled_artifact_json,
    Dto::CompiledGraphPlanDto& plan)
{
    if (!p_compiled_artifact_json)
    {
        // 空指针不是「空图」：静默跑空计划会把调用方的装配缺漏吞成成
        // 功，必须显性拒绝。
        last_error_ = "Execute: compiled artifact is null";
        DAS_CORE_LOG_ERROR("{}", last_error_);
        return DAS_E_INVALID_ARGUMENT;
    }
    const char*     utf8 = nullptr;
    const DasResult hr = p_compiled_artifact_json->GetUtf8(&utf8);
    if (DAS::IsFailed(hr) || !utf8)
    {
        last_error_ = "Execute: failed to read compiled artifact JSON";
        DAS_CORE_LOG_ERROR("{}", last_error_);
        return DAS_E_INVALID_POINTER;
    }

    std::string json_str(utf8);
    if (json_str.empty())
    {
        last_error_ = "Execute: compiled artifact is empty";
        DAS_CORE_LOG_ERROR("{}", last_error_);
        return DAS_E_INVALID_ARGUMENT;
    }
    try
    {
        // The wire artifact is a {"fingerprint","plan"} shell. Verify
        // the plan payload against the shell fingerprint (re-serialize
        // the subtree with the same standard serializer and compare)
        // before executing: a mismatch means the artifact was modified
        // or corrupted after compilation.
        auto shell = yyjson::read(json_str);
        auto shell_obj = shell.as_object();
        if (!shell_obj || !shell_obj->contains(std::string_view("fingerprint"))
            || !shell_obj->contains(std::string_view("plan")))
        {
            throw yyjson::bad_cast{
                "compiled artifact is not a {fingerprint,plan} shell"};
        }
        auto fingerprint =
            (*shell_obj)[std::string_view("fingerprint")].as_string();
        if (!fingerprint || fingerprint->empty())
        {
            throw yyjson::bad_cast{
                "compiled artifact shell carries no fingerprint"};
        }

        const auto&       plan_subtree = (*shell_obj)[std::string_view("plan")];
        const std::string computed_fingerprint =
            ComputeCompiledPlanFingerprint(plan_subtree);
        if (computed_fingerprint.empty()
            || computed_fingerprint != *fingerprint)
        {
            last_error_ = "Execute: compiled artifact fingerprint "
                          "mismatch — the plan payload was modified "
                          "or corrupted";
            DAS_CORE_LOG_ERROR("{}", last_error_);
            return DAS_E_FAIL;
        }

        plan = yyjson::cast<Dto::CompiledGraphPlanDto>(plan_subtree);
    }
    catch (const std::exception& exception)
    {
        last_error_ = std::string{"Execute: invalid compiled artifact JSON: "}
                      + exception.what();
        DAS_CORE_LOG_ERROR("{}", last_error_);
        return DAS_E_INVALID_JSON;
    }
    return DAS_S_OK;
}

DasResult GraphRuntimeImpl::Execute(
    IDasReadOnlyString*                  p_compiled_artifact_json,
    Das::PluginInterface::IDasStopToken* p_stop_token,
    Das::ExportInterface::IDasJson**     pp_out_result_json)
{
    if (!pp_out_result_json)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_result_json = nullptr;

    // Parse compiled graph plan from JSON string.
    Dto::CompiledGraphPlanDto plan;
    {
        const DasResult parse_result =
            ParseVerifiedPlan(p_compiled_artifact_json, plan);
        if (DAS::IsFailed(parse_result))
        {
            return parse_result;
        }
    }

    // Execute via engine. The artifact is self-contained — its integrity gate
    // is the shell fingerprint verified above. There is no live authoring
    // document at execution time to compare freshness against, so the
    // engine's staleness check is explicitly neutralized by self-comparison.
    const DasResult result = engine_.RunWithHost(
        plan,
        plan.source_fingerprint,
        p_stop_token,
        host_.Get());

    if (DAS::IsFailed(result))
    {
        last_error_ = engine_.GetLastErrorMessage();
        if (last_error_.empty())
        {
            last_error_ = "Graph execution failed";
        }
        DAS_CORE_LOG_ERROR("{}", last_error_);
        return result;
    }

    // 成功结果携带 graphOutputs（引擎侧按编译产物边界清单从帧导出的悬空
    // 输出口值映射；空清单/无值时为空对象——消费方按缺键处理即可）。
    yyjson::value result_payload(Das::Utils::MakeYyjsonObject());
    (*result_payload.as_object())[std::string_view("graphOutputs")] =
        Das::Utils::CloneYyjsonValue(engine_.GetLastGraphOutputs());
    auto result_serialized = Das::Utils::SerializeYyjsonValue(result_payload);
    if (!result_serialized.has_value())
    {
        return CreateEmptyDasJson(pp_out_result_json);
    }
    return ParseDasJsonFromString(
        result_serialized->c_str(),
        pp_out_result_json);
}

DasResult GraphRuntimeImpl::ExecuteWithTypedIO(
    IDasReadOnlyString*                        p_compiled_artifact_json,
    Das::PluginInterface::IDasStopToken*       p_stop_token,
    Das::ExportInterface::IDasReadOnlyPortMap* p_graph_inputs,
    Das::ExportInterface::IDasPortMap**        pp_out_outputs)
{
    if (!pp_out_outputs)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_outputs = nullptr;

    Dto::CompiledGraphPlanDto plan;
    const DasResult           parse_result =
        ParseVerifiedPlan(p_compiled_artifact_json, plan);
    if (DAS::IsFailed(parse_result))
    {
        return parse_result;
    }

    // graph_inputs 的生命周期=本调用栈：进引擎前挂载、返回前摘除，旧
    // Execute 路径恒见空（默认物化兜底不受染）。RunWithHost 非全程
    // noexcept（组件工厂调用、JSON 序列化均可抛），异常逃逸也要摘除——
    // 否则悬挂指针驻留可复用实例，旧 Execute 路径的覆盖层查询会解引用
    // 已释放的调用方对象。
    engine_.SetGraphInputs(p_graph_inputs);
    DasResult result = DAS_E_FAIL;
    try
    {
        result = engine_.RunWithHost(
            plan,
            plan.source_fingerprint,
            p_stop_token,
            host_.Get(),
            pp_out_outputs);
    }
    catch (...)
    {
        engine_.SetGraphInputs(nullptr);
        throw;
    }
    engine_.SetGraphInputs(nullptr);

    if (DAS::IsFailed(result))
    {
        last_error_ = engine_.GetLastErrorMessage();
        if (last_error_.empty())
        {
            last_error_ = "Graph execution failed";
        }
        DAS_CORE_LOG_ERROR("{}", last_error_);
        return result;
    }

    return DAS_S_OK;
}

DAS_CORE_GRAPHRUNTIME_NS_END

// ============================================================================
// C ABI factory function
// ============================================================================

DAS_C_API DasResult
CreateGraphRuntime(Das::ExportInterface::IDasGraphRuntime** pp_out_runtime)
{
    if (!pp_out_runtime)
    {
        return DAS_E_INVALID_POINTER;
    }

    try
    {
        auto* impl = new Das::Core::GraphRuntime::GraphRuntimeImpl{nullptr};
        impl->AddRef();
        *pp_out_runtime = impl;
        DAS_CORE_LOG_INFO("CreateGraphRuntime: runtime created successfully");
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        DAS_CORE_LOG_ERROR("CreateGraphRuntime: out of memory");
        return DAS_E_OUT_OF_MEMORY;
    }
}

DAS_C_API DasResult CreateGraphRuntimeWithHost(
    Das::PluginInterface::IDasTaskComponentHost* p_host,
    Das::ExportInterface::IDasGraphRuntime**     pp_out_runtime)
{
    if (!pp_out_runtime)
    {
        return DAS_E_INVALID_POINTER;
    }
    if (!p_host)
    {
        return DAS_E_INVALID_POINTER;
    }

    try
    {
        auto* impl = new Das::Core::GraphRuntime::GraphRuntimeImpl{p_host};
        impl->AddRef();
        *pp_out_runtime = impl;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

// ============================================================================
// Stateful authoring session C ABI (DAS-77)
// ============================================================================
//
// The authoritative store (GraphDocumentDto) lives in Core behind an opaque
// handle. Plugins hold the handle and call these per GetDocument / ApplyChange
// / Compile. The public projection is the schema-contract authoring doc
// (AuthoringDocContract); the runtime GraphDocumentDto never crosses.
// ============================================================================

// The Core-owned authoritative store. Opaque to plugins (forward-declared in
// the header); full definition lives only here, at global scope to match the
// header's forward declaration (the C ABI entry points are also global).
//
// `action_entries` carries the registered IDasTaskComponent catalog
// ({actionId,label} objects) translated by the Provider from
// TaskComponentFactoryManager::EnumerateDefinitions() output; the label is
// the component manifest definition's kind. When the session is created
// without a registry handle, the set is empty and actionId-bearing change
// kinds reject as UnknownActionId — the production wiring (Scheduler)
// populates this set when the session is bound to a PluginManager.
struct GraphAuthoringSessionState
{
    Das::Core::GraphRuntime::Dto::GraphDocumentDto document;
    std::vector<Das::Core::GraphRuntime::Contract::ActionCatalogEntry>
        action_entries;
    // faces 通道（nodeFaces context 键）：node_id → 注入的文档动态口面快
    // 照。按节点索引——同 guid 的 A/B 节点口不串面（按节点合并，绝不按
    // guid 聚合）；编译与画布投影按节点消费。faces 随 context 整体进入
    // 会话创建，与文档快照同 freshness（会话缓存按 revision 键控）。
    Das::Core::GraphRuntime::GraphCompiler::NodeFacesMap node_faces;
    // entryFaces 通道：entry_id（节点 settings 所持正整数的十进制文本）
    // → 绑定该条目的调用节点要并入的文档口面（被调条目文档的动态口快
    // 照——签名物化存储副本的替代数据源）。types[] 为可选的注册类型透
    // 传容器（消费随分区退役的投影侧类型注册表落地）。
    Das::Core::GraphRuntime::GraphCompiler::EntryFacesMap entry_faces;
    std::vector<Das::Core::GraphRuntime::GraphCompiler::DocumentFaceType>
        entry_face_types;
};

namespace
{
    using FormSeqDto = Das::Core::GraphRuntime::Dto::FormSequenceDto;
    using GraphDocDto = Das::Core::GraphRuntime::Dto::GraphDocumentDto;

    bool ReadUtf8(IDasReadOnlyString* p_str, std::string& out)
    {
        if (p_str == nullptr)
        {
            out.clear();
            return true;
        }
        const char* utf8 = nullptr;
        if (DAS::IsFailed(p_str->GetUtf8(&utf8)) || utf8 == nullptr)
        {
            return false;
        }
        out = utf8;
        return true;
    }

    DasResult WrapValue(
        const yyjson::value&             value,
        Das::ExportInterface::IDasJson** out)
    {
        auto serialized = Das::Utils::SerializeYyjsonValue(value);
        if (!serialized)
        {
            return DAS_E_FAIL;
        }
        return ParseDasJsonFromString(serialized->c_str(), out);
    }

    // -------------------------------------------------------------------
    // Context seeding
    // -------------------------------------------------------------------

    template <typename Obj>
    bool ReadStringField(const Obj& obj, std::string_view key, std::string& out)
    {
        if (!obj.contains(key))
        {
            return false;
        }
        if (auto v = obj[key].as_string())
        {
            out = std::string(*v);
            return true;
        }
        return false;
    }

    /// 旧节点 ports 分区的会话创建兼容读取（一轮迁移）：begin/end 节点的
    /// 分区条目 {id,name,direction,valueType} 等价迁入节点 settings
    /// .boundaryPorts[{id,type}]（类型字段名对齐边界会话状态域；方向不
    /// 迁——由角色固化推导，name 不迁——边界条目单 id 形态）；其余节点
    /// 的分区数据（调用节点物化口通道/历史遗留）丢弃 + WARN（口面经
    /// entryFaces 现算/文档口面推导，存储域已退役）。分区键随 DTO 退役
    /// 不再进序列化——本迁移是在途图数据保全的唯一防线（caster 对未知
    /// 键静默忽略，等价读取先于它发生）。
    void MigrateLegacyPortPartition(
        const yyjson::value&                        node_value,
        Das::Core::GraphRuntime::Dto::GraphNodeDto& node)
    {
        auto node_obj = node_value.as_object();
        if (!node_obj || !node_obj->contains(std::string_view("ports")))
        {
            return;
        }
        auto legacy_ports = (*node_obj)[std::string_view("ports")].as_array();
        if (!legacy_ports)
        {
            return;
        }
        const bool is_boundary =
            node.target.component_ref.has_value()
            && (node.target.component_ref->component_guid
                    == Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID
                || node.target.component_ref->component_guid
                       == Das::Core::GraphRuntime::GRAPH_END_COMPONENT_GUID);
        if (!is_boundary)
        {
            if (!legacy_ports->empty())
            {
                DAS_CORE_LOG_WARN(
                    "legacy ports partition dropped on node {} (the graph "
                    "document stores no port data; port faces derive from "
                    "the owning documents)",
                    node.node_id);
            }
            return;
        }
        yyjson::value ports_array(Das::Utils::MakeYyjsonArray());
        auto          ports_arr = ports_array.as_array();
        std::size_t   migrated = 0;
        for (const auto& entry_value : *legacy_ports)
        {
            auto entry_obj = entry_value.as_object();
            if (!entry_obj)
            {
                continue;
            }
            const auto id = (*entry_obj)[std::string_view("id")].as_string();
            const auto type =
                (*entry_obj)[std::string_view("valueType")].as_string();
            if (!id || id->empty() || !type || type->empty())
            {
                continue;
            }
            yyjson::value entry(Das::Utils::MakeYyjsonObject());
            auto          entry_o = entry.as_object();
            (*entry_o)[std::string_view("id")] =
                yyjson::value(std::string(*id));
            (*entry_o)[std::string_view("type")] =
                yyjson::value(std::string(*type));
            ports_arr->emplace_back(std::move(entry));
            ++migrated;
        }
        if (migrated == 0)
        {
            return;
        }
        auto settings_obj = node.settings.as_object();
        if (!settings_obj)
        {
            node.settings = Das::Utils::MakeYyjsonObject();
            settings_obj = node.settings.as_object();
        }
        if (settings_obj->contains(std::string_view("boundaryPorts")))
        {
            auto existing =
                (*settings_obj)[std::string_view("boundaryPorts")].as_array();
            if (existing)
            {
                for (const auto& entry : *ports_arr)
                {
                    existing->emplace_back(Das::Utils::CloneYyjsonValue(entry));
                }
                return;
            }
        }
        (*settings_obj)[std::string_view("boundaryPorts")] =
            std::move(ports_array);
    }

    /// Read a GraphDocumentDto field-by-field from a JSON object (robust manual
    /// parse at the boundary; aggregate cast would also work but this avoids
    /// throwing on partial input).
    template <typename Obj>
    bool ParseGraphDoc(const Obj& obj, GraphDocDto& out)
    {
        out = GraphDocDto{};
        // The metadata keys are optional for raw graph docs: an unguarded
        // operator[] on a missing key throws out_of_range, so guard every
        // read with contains first.
        if (obj.contains(std::string_view("documentId")))
        {
            if (auto v = obj[std::string_view("documentId")].as_string())
            {
                out.document_id = std::string(*v);
            }
        }
        if (obj.contains(std::string_view("version")))
        {
            if (auto v = obj[std::string_view("version")].as_int())
            {
                out.version = static_cast<int32_t>(*v);
            }
        }
        if (obj.contains(std::string_view("fingerprint")))
        {
            if (auto v = obj[std::string_view("fingerprint")].as_string())
            {
                out.fingerprint = std::string(*v);
            }
        }
        if (obj.contains(std::string_view("nodes")))
        {
            auto arr = obj[std::string_view("nodes")].as_array();
            if (arr)
            {
                for (const auto& nv : *arr)
                {
                    try
                    {
                        // yyjson::cast routes through the hand-written
                        // GraphNodeDto caster (missing-key tolerant);
                        // Detail::CastDtoObject would bypass it and hit the
                        // strict aggregate, dropping pre-label nodes.
                        out.nodes.push_back(
                            yyjson::cast<
                                Das::Core::GraphRuntime::Dto::GraphNodeDto>(
                                nv));
                        MigrateLegacyPortPartition(nv, out.nodes.back());
                    }
                    catch (...)
                    {
                        // skip malformed node
                    }
                }
            }
        }
        if (obj.contains(std::string_view("edges")))
        {
            auto arr = obj[std::string_view("edges")].as_array();
            if (arr)
            {
                for (const auto& ev : *arr)
                {
                    try
                    {
                        out.edges.push_back(
                            yyjson::cast<
                                Das::Core::GraphRuntime::Dto::GraphEdgeDto>(
                                ev));
                    }
                    catch (...)
                    {
                    }
                }
            }
        }
        if (obj.contains(std::string_view("tags")))
        {
            auto arr = obj[std::string_view("tags")].as_array();
            if (arr)
            {
                for (const auto& tv : *arr)
                {
                    if (auto s = tv.as_string())
                    {
                        out.tags.emplace_back(std::string(*s));
                    }
                }
            }
        }
        // The viewData partition is preserved verbatim (pass-through, no
        // interpretation) so layout coordinates survive the
        // acceptedProperties -> re-seed round-trip.
        if (obj.contains(std::string_view("viewData")))
        {
            out.view_data =
                Das::Utils::CloneYyjsonValue(obj[std::string_view("viewData")]);
        }
        return true;
    }

    // Reverse-map a contract authoring doc (the shape SerializeDocument emits,
    // and the shape the scheduler persists in `properties`) back onto the
    // authoritative GraphDocumentDto store. The formSequence branch accepts
    // both the current contract shape (formSequence.items[]) and the legacy
    // pre-cutover shape (formSequence.sequence[]); graph seeds accept only the
    // full GraphDocumentDto shape (SerializeSource / acceptedProperties —
    // edges carry their declared edgeType), via the raw {nodes} fallback.
    void SeedFromDocValue(
        GraphAuthoringSessionState& s,
        const yyjson::value&        doc)
    {
        auto obj = doc.as_object();
        if (!obj)
        {
            return;
        }

        // Resolve the kind. The contract doc carries {"kind": "formSequence"};
        // a raw formSequence carries "items"; a raw graph doc "nodes".
        std::string kind;
        ReadStringField(*obj, std::string_view("kind"), kind);

        const bool has_form_seq =
            obj->contains(std::string_view("formSequence"));

        if (kind == "formSequence" || has_form_seq)
        {
            // Contract formSequence doc. Two shapes are accepted: the
            // current wire's formSequence.items[] and the legacy
            // pre-cutover formSequence.sequence[] — items first, sequence
            // as the fallback, empty document when neither key exists.
            FormSeqDto seq;
            ReadStringField(
                *obj,
                std::string_view("sourceFingerprint"),
                seq.fingerprint);
            yyjson::value seq_src = doc;
            if (has_form_seq)
            {
                seq_src = (*obj)[std::string_view("formSequence")];
            }
            if (auto seq_obj = seq_src.as_object())
            {
                const std::string_view item_list_key =
                    seq_obj->contains(std::string_view("items"))
                        ? std::string_view("items")
                        : std::string_view("sequence");
                if (seq_obj->contains(item_list_key))
                {
                    if (auto arr = (*seq_obj)[item_list_key].as_array())
                    {
                        for (const auto& iv : *arr)
                        {
                            auto io = iv.as_object();
                            if (!io)
                            {
                                continue;
                            }
                            Das::Core::GraphRuntime::Dto::FormSequenceItemDto
                                item;
                            if (auto v =
                                    (*io)[std::string_view("id")].as_string())
                            {
                                item.item_id = std::string(*v);
                            }
                            std::string guid;
                            // DAS-81: componentGuid (renamed from type).
                            // Accept both for backward compat with persisted
                            // contract docs.
                            if (!ReadStringField(
                                    *io,
                                    std::string_view("componentGuid"),
                                    guid))
                            {
                                ReadStringField(
                                    *io,
                                    std::string_view("type"),
                                    guid);
                            }
                            if (!guid.empty())
                            {
                                item.target.target_kind = "componentRef";
                                item.target.component_ref = Das::Core::
                                    GraphRuntime::Dto::ComponentRefDto{
                                        "componentRef",
                                        guid,
                                        {}};
                            }
                            if (io->contains(std::string_view("settings")))
                            {
                                item.settings = Das::Utils::CloneYyjsonValue(
                                    (*io)[std::string_view("settings")]);
                            }
                            seq.items.push_back(std::move(item));
                        }
                    }
                }
            }
            s.document =
                Das::Core::GraphRuntime::FormSequenceProjector::Project(seq);
            s.document.tags.emplace_back(
                Das::Core::GraphRuntime::Contract::LINEAR_TAG);
            return;
        }

        // Raw formSequence ({items}) / raw graph ({nodes}) fallback.
        if (obj->contains(std::string_view("items")))
        {
            FormSeqDto seq;
            ReadStringField(
                *obj,
                std::string_view("documentId"),
                seq.document_id);
            if (obj->contains(std::string_view("version")))
            {
                if (auto v = (*obj)[std::string_view("version")].as_int())
                {
                    seq.version = static_cast<int32_t>(*v);
                }
            }
            ReadStringField(
                *obj,
                std::string_view("fingerprint"),
                seq.fingerprint);
            if (auto items = (*obj)[std::string_view("items")].as_array())
            {
                for (const auto& iv : *items)
                {
                    try
                    {
                        seq.items.push_back(
                            Das::Core::GraphRuntime::Dto::Detail::CastDtoObject<
                                Das::Core::GraphRuntime::Dto::
                                    FormSequenceItemDto>(iv));
                    }
                    catch (...)
                    {
                    }
                }
            }
            s.document =
                Das::Core::GraphRuntime::FormSequenceProjector::Project(seq);
            s.document.tags.emplace_back(
                Das::Core::GraphRuntime::Contract::LINEAR_TAG);
        }
        else if (obj->contains(std::string_view("nodes")))
        {
            ParseGraphDoc(*obj, s.document);
        }
    }

    // Forward declarations — the parse helpers' definitions live further
    // down, next to the CatalogPortShapeValid lock-shape rule they share.
    std::optional<
        std::vector<Das::Core::GraphRuntime::Contract::ActionCatalogEntry>>
    ParseActionCatalogEntries(const yyjson::value& root);
    std::optional<Das::Core::GraphRuntime::GraphCompiler::NodeFacesMap>
    ParseNodeFaces(const yyjson::value& root);
    std::optional<std::pair<
        Das::Core::GraphRuntime::GraphCompiler::EntryFacesMap,
        std::vector<Das::Core::GraphRuntime::GraphCompiler::DocumentFaceType>>>
    ParseEntryFaces(const yyjson::value& root);

    void SeedFromContext(GraphAuthoringSessionState& s, std::string_view json)
    {
        if (json.empty())
        {
            return;
        }
        yyjson::value root;
        try
        {
            root = yyjson::read(std::string(json));
        }
        catch (...)
        {
            return;
        }
        auto obj = root.as_object();
        if (!obj)
        {
            return;
        }

        // actionCatalog 键自解析（追加裁定二）：SeedFromContext 在此把
        // context 携带的 registry 快照播种进既有 action_entries 容器
        // （MakeCatalogFromState 读 state 路径零改动）——插件是纯管道
        // （完整 context 原文透传，零提取/转发跳）。畸形降级：零 catalog
        // + WARNING，会话继续加载文档。
        if (obj->contains(std::string_view("actionCatalog")))
        {
            auto entries = ParseActionCatalogEntries(
                (*obj)[std::string_view("actionCatalog")]);
            if (entries.has_value())
            {
                s.action_entries = std::move(*entries);
            }
            else
            {
                DAS_CORE_LOG_WARN(
                    "context actionCatalog key rejected (shape violation); "
                    "the session continues with an empty catalog");
            }
        }
        // nodeFaces 键（faces 注入通道）：文档动态口面以数据快照随
        // context 进入会话状态（编译宿主零活查询）。畸形→该键整体零
        // faces + WARNING（含键名与违规条目定位，ParseNodeFaces 在违规
        // 点发出）+ 会话继续——降级到「无口」而非「全口」；键缺席→零
        // faces 零告警（过渡态/standalone 合法）。
        if (obj->contains(std::string_view("nodeFaces")))
        {
            auto faces = ParseNodeFaces((*obj)[std::string_view("nodeFaces")]);
            if (faces.has_value())
            {
                s.node_faces = std::move(*faces);
            }
        }
        // entryFaces 键（绑条目调用节点的文档口面通道）：畸形降级语义与
        // nodeFaces 同款（该键整体零 entryFaces + WARNING + 会话继续）。
        if (obj->contains(std::string_view("entryFaces")))
        {
            auto parsed =
                ParseEntryFaces((*obj)[std::string_view("entryFaces")]);
            if (parsed.has_value())
            {
                s.entry_faces = std::move(parsed->first);
                s.entry_face_types = std::move(parsed->second);
            }
        }

        // Real scheduler context shape: {taskId, properties, revision,
        // authoring}. The live store round-trips through `properties` as a
        // contract authoring doc (what SerializeDocument emits and ApplyChange
        // returns as acceptedProperties). Prefer `properties`; fall back to a
        // raw/contract doc at top level for direct callers and tests.
        if (obj->contains(std::string_view("properties")))
        {
            SeedFromDocValue(s, (*obj)[std::string_view("properties")]);
            return;
        }
        if (obj->contains(std::string_view("kind"))
            || obj->contains(std::string_view("items"))
            || obj->contains(std::string_view("nodes")))
        {
            SeedFromDocValue(s, root);
        }
        // else: empty (graph-mode) store.
    }

    // -------------------------------------------------------------------
    // Contract change parsing
    // -------------------------------------------------------------------

    template <typename Obj>
    std::optional<Das::Core::GraphRuntime::Contract::GraphConnection>
    ParseGraphConnection(const Obj& obj)
    {
        Das::Core::GraphRuntime::Contract::GraphConnection c;
        if (!ReadStringField(
                obj,
                std::string_view("fromNodeId"),
                c.from_node_id))
        {
            return std::nullopt;
        }
        ReadStringField(obj, std::string_view("fromPortId"), c.from_port_id);
        if (!ReadStringField(obj, std::string_view("toNodeId"), c.to_node_id))
        {
            return std::nullopt;
        }
        ReadStringField(obj, std::string_view("toPortId"), c.to_port_id);
        return c;
    }

    // Strict Phase 84 AuthoringChange parser. The {baseRevision, kind, payload}
    // envelope is the only accepted shape; legacy flat {op, ...} is rejected
    // upstream as a malformed request. Per-kind payload fields populate the
    // optional slots of AuthoringChange; the dispatch reads them by kind.
    bool ParseAuthoringChange(
        const yyjson::value&                                json,
        Das::Core::GraphRuntime::Contract::AuthoringChange& out,
        int64_t&                                            out_base_revision)
    {
        out = Das::Core::GraphRuntime::Contract::AuthoringChange{};
        out_base_revision = 0;
        auto obj = json.as_object();
        if (!obj)
        {
            return false;
        }
        // baseRevision is required by the strict envelope but only used by the
        // caller for revision-invariant result shaping; this parser is lenient
        // (defaults to 0 when missing) so that test callers and the scheduler
        // both behave predictably.
        if (obj->contains(std::string_view("baseRevision")))
        {
            if (auto v = (*obj)[std::string_view("baseRevision")].as_int())
            {
                out_base_revision = static_cast<int64_t>(*v);
            }
        }
        if (!ReadStringField(*obj, std::string_view("kind"), out.kind))
        {
            return false;
        }
        if (!obj->contains(std::string_view("payload")))
        {
            return false;
        }
        auto payload = (*obj)[std::string_view("payload")].as_object();
        if (!payload)
        {
            return false;
        }

        // setValue payload: {target, fieldName, value}.
        if (out.kind == "setValue")
        {
            if (payload->contains(std::string_view("target")))
            {
                auto target_o =
                    (*payload)[std::string_view("target")].as_object();
                if (target_o)
                {
                    Das::Core::GraphRuntime::Contract::SetValueTarget t;
                    ReadStringField(
                        *target_o,
                        std::string_view("kind"),
                        t.kind);
                    std::string id;
                    if (ReadStringField(*target_o, std::string_view("id"), id)
                        && !id.empty())
                    {
                        t.id = std::move(id);
                    }
                    out.target = std::move(t);
                }
            }
            std::string field_name;
            if (ReadStringField(
                    *payload,
                    std::string_view("fieldName"),
                    field_name)
                && !field_name.empty())
            {
                out.field_name = std::move(field_name);
            }
            if (payload->contains(std::string_view("value")))
            {
                out.value = Das::Utils::CloneYyjsonValue(
                    (*payload)[std::string_view("value")]);
            }
            return true;
        }
        // invokeCommand payload: {commandId}.
        if (out.kind == "invokeCommand")
        {
            std::string command_id;
            if (ReadStringField(
                    *payload,
                    std::string_view("commandId"),
                    command_id)
                && !command_id.empty())
            {
                out.command_id = std::move(command_id);
            }
            return true;
        }
        // convertView payload: {targetViewKind}.
        if (out.kind == "convertView")
        {
            std::string tvk;
            if (ReadStringField(
                    *payload,
                    std::string_view("targetViewKind"),
                    tvk)
                && !tvk.empty())
            {
                out.target_view_kind = std::move(tvk);
            }
            return true;
        }
        // addGraphNode / addFormSequenceItem payload: {actionId,
        // insertionIndex?}.
        if (out.kind == "addGraphNode" || out.kind == "addFormSequenceItem")
        {
            std::string action_id;
            if (ReadStringField(
                    *payload,
                    std::string_view("actionId"),
                    action_id)
                && !action_id.empty())
            {
                out.action_id = std::move(action_id);
            }
            if (payload->contains(std::string_view("insertionIndex")))
            {
                if (auto v =
                        (*payload)[std::string_view("insertionIndex")].as_int())
                {
                    out.insertion_index = static_cast<int64_t>(*v);
                }
            }
            return true;
        }
        // moveFormSequenceItem payload: {formSequenceItemId, destinationIndex}.
        if (out.kind == "moveFormSequenceItem")
        {
            std::string id;
            if (ReadStringField(
                    *payload,
                    std::string_view("formSequenceItemId"),
                    id)
                && !id.empty())
            {
                out.form_sequence_item_id = std::move(id);
            }
            if (payload->contains(std::string_view("destinationIndex")))
            {
                if (auto v = (*payload)[std::string_view("destinationIndex")]
                                 .as_int())
                {
                    out.destination_index = static_cast<int64_t>(*v);
                }
            }
            return true;
        }
        // moveGraphNode payload: {nodeId, position:{x,y}}. The coordinates
        // ride the document-level viewData partition (not node.settings).
        // as_num accepts both "120" and "120.5" so integer-formatted JSON
        // coordinates deserialize identically.
        if (out.kind == "moveGraphNode")
        {
            std::string id;
            if (ReadStringField(*payload, std::string_view("nodeId"), id)
                && !id.empty())
            {
                out.item_or_node_id = std::move(id);
            }
            if (payload->contains(std::string_view("position")))
            {
                if (auto position =
                        (*payload)[std::string_view("position")].as_object())
                {
                    if (position->contains(std::string_view("x")))
                    {
                        if (auto x_value =
                                (*position)[std::string_view("x")].as_num())
                        {
                            out.position_x = *x_value;
                        }
                    }
                    if (position->contains(std::string_view("y")))
                    {
                        if (auto y_value =
                                (*position)[std::string_view("y")].as_num())
                        {
                            out.position_y = *y_value;
                        }
                    }
                }
            }
            return true;
        }
        // removeGraphNodes payload: {nodeIds[], connections?[]}. Batch node
        // removal — ApplyGraphChange validates all ids and resolves all
        // connections before mutating (atomic validate-all-then-mutate).
        if (out.kind == "removeGraphNodes")
        {
            if (payload->contains(std::string_view("nodeIds")))
            {
                if (auto ids =
                        (*payload)[std::string_view("nodeIds")].as_array())
                {
                    std::vector<std::string> node_ids;
                    for (const auto& item : *ids)
                    {
                        if (auto id = item.as_string())
                        {
                            node_ids.emplace_back(*id);
                        }
                    }
                    out.removed_node_ids = std::move(node_ids);
                }
            }
            if (payload->contains(std::string_view("connections")))
            {
                if (auto connections =
                        (*payload)[std::string_view("connections")].as_array())
                {
                    std::vector<
                        Das::Core::GraphRuntime::Contract::GraphConnection>
                        parsed_connections;
                    for (const auto& connection : *connections)
                    {
                        auto connection_obj = connection.as_object();
                        if (!connection_obj)
                        {
                            continue;
                        }
                        if (auto parsed = ParseGraphConnection(*connection_obj))
                        {
                            parsed_connections.push_back(std::move(*parsed));
                        }
                    }
                    out.removed_connections = std::move(parsed_connections);
                }
            }
            return true;
        }
        // moveGraphNodes payload: {moves[]} — each entry is
        // {nodeId, position:{x,y}}. as_num accepts both "120" and "120.5"
        // so integer-formatted JSON coordinates deserialize identically.
        if (out.kind == "moveGraphNodes")
        {
            if (payload->contains(std::string_view("moves")))
            {
                if (auto moves =
                        (*payload)[std::string_view("moves")].as_array())
                {
                    std::vector<
                        Das::Core::GraphRuntime::Contract::GraphNodeMove>
                        node_moves;
                    for (const auto& item : *moves)
                    {
                        auto move_obj = item.as_object();
                        if (!move_obj)
                        {
                            continue;
                        }
                        Das::Core::GraphRuntime::Contract::GraphNodeMove
                                    node_move;
                        std::string id;
                        if (ReadStringField(
                                *move_obj,
                                std::string_view("nodeId"),
                                id)
                            && !id.empty())
                        {
                            node_move.node_id = std::move(id);
                        }
                        if (move_obj->contains(std::string_view("position")))
                        {
                            if (auto position =
                                    (*move_obj)[std::string_view("position")]
                                        .as_object())
                            {
                                if (position->contains(std::string_view("x")))
                                {
                                    if (auto x_value =
                                            (*position)[std::string_view("x")]
                                                .as_num())
                                    {
                                        node_move.position_x = *x_value;
                                    }
                                }
                                if (position->contains(std::string_view("y")))
                                {
                                    if (auto y_value =
                                            (*position)[std::string_view("y")]
                                                .as_num())
                                    {
                                        node_move.position_y = *y_value;
                                    }
                                }
                            }
                        }
                        node_moves.push_back(std::move(node_move));
                    }
                    out.node_moves = std::move(node_moves);
                }
            }
            return true;
        }
        // renameGraphNode payload: {nodeId, label}. The label is pure
        // presentation data written onto the store node; addressing stays on
        // the node id (same slot convention as moveGraphNode).
        if (out.kind == "renameGraphNode")
        {
            std::string id;
            if (ReadStringField(*payload, std::string_view("nodeId"), id)
                && !id.empty())
            {
                out.item_or_node_id = std::move(id);
            }
            std::string label;
            if (ReadStringField(*payload, std::string_view("label"), label)
                && !label.empty())
            {
                out.rename_label = std::move(label);
            }
            return true;
        }
        // addNodePort payload: {nodeId, name, direction, valueType, portId?}.
        // The nodeId rides the shared item_or_node_id slot; the port fields
        // feed the begin/end dynamic port partition dispatch. portId is the
        // optional stable-id channel for call-node signature port
        // materialization (the runtime value-plane key — the child graph's
        // boundary port id), absent for the allocator-assigned p_N ids.
        if (out.kind == "addNodePort")
        {
            std::string node_id;
            if (ReadStringField(*payload, std::string_view("nodeId"), node_id)
                && !node_id.empty())
            {
                out.item_or_node_id = std::move(node_id);
            }
            std::string port_id;
            if (ReadStringField(*payload, std::string_view("portId"), port_id)
                && !port_id.empty())
            {
                out.node_port_id = std::move(port_id);
            }
            std::string port_name;
            if (ReadStringField(*payload, std::string_view("name"), port_name)
                && !port_name.empty())
            {
                out.node_port_name = std::move(port_name);
            }
            std::string direction;
            if (ReadStringField(
                    *payload,
                    std::string_view("direction"),
                    direction))
            {
                out.node_port_direction = std::move(direction);
            }
            std::string value_type;
            if (ReadStringField(
                    *payload,
                    std::string_view("valueType"),
                    value_type))
            {
                out.node_port_value_type = std::move(value_type);
            }
            return true;
        }
        // removeNodePort payload: {nodeId, portId} — the stable partition id
        // addresses the declaration.
        if (out.kind == "removeNodePort")
        {
            std::string node_id;
            if (ReadStringField(*payload, std::string_view("nodeId"), node_id)
                && !node_id.empty())
            {
                out.item_or_node_id = std::move(node_id);
            }
            std::string port_id;
            if (ReadStringField(*payload, std::string_view("portId"), port_id)
                && !port_id.empty())
            {
                out.node_port_id = std::move(port_id);
            }
            return true;
        }
        // renameNodePort payload: {nodeId, portId, name}.
        if (out.kind == "renameNodePort")
        {
            std::string node_id;
            if (ReadStringField(*payload, std::string_view("nodeId"), node_id)
                && !node_id.empty())
            {
                out.item_or_node_id = std::move(node_id);
            }
            std::string port_id;
            if (ReadStringField(*payload, std::string_view("portId"), port_id)
                && !port_id.empty())
            {
                out.node_port_id = std::move(port_id);
            }
            std::string port_name;
            if (ReadStringField(*payload, std::string_view("name"), port_name)
                && !port_name.empty())
            {
                out.node_port_name = std::move(port_name);
            }
            return true;
        }
        // removeFormSequenceItem / removeGraphNode payload:
        // {formSequenceItemId} or {nodeId}. Both are surfaced via
        // item_or_node_id.
        if (out.kind == "removeFormSequenceItem"
            || out.kind == "removeGraphNode")
        {
            std::string id;
            const auto  key = out.kind == "removeFormSequenceItem"
                                  ? std::string_view("formSequenceItemId")
                                  : std::string_view("nodeId");
            if (ReadStringField(*payload, key, id) && !id.empty())
            {
                out.item_or_node_id = std::move(id);
            }
            return true;
        }
        // connectGraphPorts / disconnectGraphPorts payload: connection fields.
        if (out.kind == "connectGraphPorts"
            || out.kind == "disconnectGraphPorts")
        {
            out.connection = ParseGraphConnection(*payload);
            return true;
        }
        // Unknown kinds are surfaced as parse failure; the dispatcher emits an
        // InvalidOp diagnostic. The strict upstream validator would have
        // rejected them earlier.
        return true;
    }

    /// Optional ActionCatalog projection from the session's stored registry
    /// snapshot. Both the Get and the Apply document serialisation paths must
    /// pass it so the node port projection (static registry ports merged by
    /// componentGuid) stays identical across GetDocument and every Apply
    /// round-trip.
    std::optional<Das::Core::GraphRuntime::Contract::ActionCatalog>
    MakeCatalogFromState(const GraphAuthoringSessionState& state)
    {
        if (state.action_entries.empty())
        {
            return std::nullopt;
        }
        Das::Core::GraphRuntime::Contract::ActionCatalog catalog;
        catalog.entries = state.action_entries;
        return catalog;
    }

    /// Build the apply-result JSON. Per D-84-08/D-84-10, the result envelope
    /// is {acceptedProperties, document, diagnostics[]}; per the Phase 84
    /// validator (plan 01), `document.revision` must equal `baseRevision + 1`
    /// on success. The post-apply store version is reflected directly inside
    /// the emitted document; `base_revision` is retained for documentation.
    yyjson::value BuildChangeResultJson(
        const Das::Core::GraphRuntime::Contract::AuthoringChangeResult& r,
        int32_t            base_revision,
        const GraphDocDto& document,
        const std::optional<Das::Core::GraphRuntime::Contract::ActionCatalog>&
            action_catalog,
        const Das::Core::GraphRuntime::GraphCompiler::NodeFacesMap* node_faces,
        const Das::Core::GraphRuntime::GraphCompiler::EntryFacesMap*
            entry_faces)
    {
        auto root = Das::Utils::MakeYyjsonObject();
        auto obj = root.as_object();

        // diagnostics[] — strict Phase 84 shape (D-84-11).
        auto diag_arr = Das::Utils::MakeYyjsonArray();

        if (r.Ok())
        {
            // D-41: acceptedProperties = source (persisted GraphDocumentDto);
            // document = regenerated UI view. Distinct blobs — no double-stuff.
            (*obj)[std::string_view("acceptedProperties")] =
                Das::Core::GraphRuntime::Contract::SerializeSource(document);
            (*obj)[std::string_view("document")] =
                Das::Core::GraphRuntime::Contract::SerializeDocument(
                    document,
                    action_catalog,
                    node_faces,
                    entry_faces);
        }
        else
        {
            // Strict Phase 84 ApplyResult envelope is {acceptedProperties,
            // document, diagnostics[]} — no top-level errorKind/message. The
            // error is surfaced as a diagnostic with severity:error so the
            // Scheduler can pass it through to the client. An optional integer
            // DasResult code is attached when the caller mapped the error kind
            // to a stable DasResult value; the strict validator allows code on
            // Diagnostic. Here we keep code absent and rely on the message
            // alone — GraphRuntime-local errors are not DasResult-coded.
            auto diag_obj = Das::Utils::MakeYyjsonObject();
            auto d_o = diag_obj.as_object();
            if (d_o)
            {
                (*d_o)[std::string_view("severity")] = std::make_pair(
                    std::string_view("error"),
                    yyjson::copy_string);
                (*d_o)[std::string_view("message")] = std::make_pair(
                    std::string_view(r.message),
                    yyjson::copy_string);
            }
            auto diag_arr_o = diag_arr.as_array();
            if (diag_arr_o)
            {
                diag_arr_o->emplace_back(std::move(diag_obj));
            }
            // On failure, the document is the unchanged pre-apply state — emit
            // it so the client sees a fresh authoritative snapshot. The
            // revision reflects the unchanged store.
            (*obj)[std::string_view("document")] =
                Das::Core::GraphRuntime::Contract::SerializeDocument(
                    document,
                    action_catalog,
                    node_faces,
                    entry_faces);
            // acceptedProperties remains the persisted source so the client
            // sees the unchanged authoritative state.
            (*obj)[std::string_view("acceptedProperties")] =
                Das::Core::GraphRuntime::Contract::SerializeSource(document);
            // baseRevision-based invariant: on failure, document.revision must
            // equal base (no change). The validator only rejects on Apply
            // success paths where document.revision != base+1; failure paths
            // surface diagnostics instead.
            std::ignore = base_revision;
        }
        (*obj)[std::string_view("diagnostics")] = std::move(diag_arr);

        // The allocated id (D-84-38) is NOT emitted as a separate envelope
        // field — the strict Phase 84 Apply result validator
        // (ValidateAuthoringApplyResult) rejects unknown fields. Callers read
        // the freshly-allocated id from the returned document's nodes[]/items[]
        // instead. Failure-path diagnostics surface errorKind/message only via
        // the diagnostics[] array (no top-level errorKind field on the
        // envelope).
        std::ignore = r.allocated_id;
        return root;
    }

    // Catalog 端口对象的锁形规则（单一来源）：必填 portId/portType/
    // direction，可选 displayLabel/loopCarried；键集外任何键、空 id/type、
    // direction 不在 {input,output}、非字符串 displayLabel、非布尔
    // loopCarried 均为形状违规。会话目录注入入口与端口面构建段共用本
    // 判定——两处曾各自手抄键清单，只改一份时带新键的目录被另一份
    // 整体拒收（添加节点面板全空）；改键集/值域只改这里。
    bool CatalogPortShapeValid(const yyjson::value& port_value)
    {
        auto port_obj = port_value.as_object();
        if (!port_obj.has_value()
            || !port_obj->contains(std::string_view("portId"))
            || !port_obj->contains(std::string_view("portType"))
            || !port_obj->contains(std::string_view("direction")))
        {
            return false;
        }
        for (const auto& [key, value] : *port_obj)
        {
            (void)value;
            if (key != std::string_view("portId")
                && key != std::string_view("portType")
                && key != std::string_view("direction")
                && key != std::string_view("displayLabel")
                && key != std::string_view("loopCarried"))
            {
                return false;
            }
        }
        const auto& port = *port_obj;
        const auto  id = port[std::string_view("portId")].as_string();
        if (!id.has_value() || id->empty())
        {
            return false;
        }
        const auto type = port[std::string_view("portType")].as_string();
        if (!type.has_value() || type->empty())
        {
            return false;
        }
        const auto direction = port[std::string_view("direction")].as_string();
        if (!direction.has_value()
            || (*direction != std::string_view("input")
                && *direction != std::string_view("output")))
        {
            return false;
        }
        if (port.contains(std::string_view("displayLabel"))
            && !port[std::string_view("displayLabel")].as_string().has_value())
        {
            return false;
        }
        if (port.contains(std::string_view("loopCarried"))
            && !port[std::string_view("loopCarried")].as_bool().has_value())
        {
            return false;
        }
        return true;
    }

    // faces 通道口条目的锁形规则（单一来源，nodeFaces/entryFaces 两键共
    // 用——照 CatalogPortShapeValid 先例收拢）：必含非空 id/type 与
    // direction ∈ {input,output}；label 可选（缺失即 id，非字符串即违
    // 规）；键集外任何键均为形状违规。与 CatalogPortShapeValid 分立——
    // 那是 catalog 静态口（portId/portType）的词表，两键不同源。
    bool FacePortShapeValid(const yyjson::value& port_value)
    {
        auto port_obj = port_value.as_object();
        if (!port_obj.has_value() || !port_obj->contains(std::string_view("id"))
            || !port_obj->contains(std::string_view("type"))
            || !port_obj->contains(std::string_view("direction")))
        {
            return false;
        }
        for (const auto& [key, value] : *port_obj)
        {
            (void)value;
            if (key != std::string_view("id") && key != std::string_view("type")
                && key != std::string_view("direction")
                && key != std::string_view("label"))
            {
                return false;
            }
        }
        const auto& port = *port_obj;
        const auto  id = port[std::string_view("id")].as_string();
        if (!id.has_value() || id->empty())
        {
            return false;
        }
        const auto type = port[std::string_view("type")].as_string();
        if (!type.has_value() || type->empty())
        {
            return false;
        }
        const auto direction = port[std::string_view("direction")].as_string();
        if (!direction.has_value()
            || (*direction != std::string_view("input")
                && *direction != std::string_view("output")))
        {
            return false;
        }
        if (port.contains(std::string_view("label"))
            && !port[std::string_view("label")].as_string().has_value())
        {
            return false;
        }
        return true;
    }

    // nodeFaces context 键解析（fail-closed 逐条；畸形→该键整体零 faces
    // + WARNING（含键名与违规条目定位）+ 会话继续——actionCatalog Wire
    // 降级同款，降级到「无口」而非「全口」）。条目形状：
    // {nodeId, ports:[{id,type,direction[,label]}]}；重复 nodeId 视为形
    // 状违规（wire 契约=每节点一条）。数据源是 Core 自产会话投影，但
    // 解析仍按不可信输入防御（threat T-111.1-01）。
    std::optional<Das::Core::GraphRuntime::GraphCompiler::NodeFacesMap>
    ParseNodeFaces(const yyjson::value& root)
    {
        auto arr = root.as_array();
        if (!arr.has_value())
        {
            DAS_CORE_LOG_WARN(
                "context nodeFaces key rejected: value is not an array; the "
                "session continues with no injected faces");
            return std::nullopt;
        }
        Das::Core::GraphRuntime::GraphCompiler::NodeFacesMap faces;
        std::size_t                                          entry_index = 0;
        for (const auto& entry_value : *arr)
        {
            const auto reject = [entry_index](std::string_view reason)
            {
                DAS_CORE_LOG_WARN(
                    "context nodeFaces key rejected at entry {}: {}; the "
                    "session continues with no injected faces",
                    entry_index,
                    reason);
            };
            auto entry_obj = entry_value.as_object();
            if (!entry_obj.has_value()
                || !entry_obj->contains(std::string_view("nodeId")))
            {
                reject("entry is not an object or carries no nodeId");
                return std::nullopt;
            }
            auto node_id = (*entry_obj)[std::string_view("nodeId")].as_string();
            if (!node_id.has_value() || node_id->empty())
            {
                reject("nodeId is empty or not a string");
                return std::nullopt;
            }
            if (faces.find(std::string(*node_id)) != faces.end())
            {
                reject(
                    "duplicate nodeId (the wire contract is one entry "
                    "per node)");
                return std::nullopt;
            }
            if (!entry_obj->contains(std::string_view("ports")))
            {
                reject("entry carries no ports array");
                return std::nullopt;
            }
            auto ports_arr = (*entry_obj)[std::string_view("ports")].as_array();
            if (!ports_arr.has_value())
            {
                reject("ports is not an array");
                return std::nullopt;
            }
            std::vector<
                Das::Core::GraphRuntime::GraphCompiler::DocumentFacePort>
                ports;
            ports.reserve(ports_arr->size());
            std::size_t port_index = 0;
            for (const auto& port_value : *ports_arr)
            {
                if (!FacePortShapeValid(port_value))
                {
                    reject(
                        "port " + std::to_string(port_index)
                        + " violates the {id,type,direction[,label]} "
                          "shape");
                    return std::nullopt;
                }
                auto        port_obj = port_value.as_object();
                const auto& port = *port_obj;
                Das::Core::GraphRuntime::GraphCompiler::DocumentFacePort
                    port_entry;
                port_entry.port_id =
                    std::string(*port[std::string_view("id")].as_string());
                port_entry.port_type =
                    std::string(*port[std::string_view("type")].as_string());
                port_entry.direction = std::string(
                    *port[std::string_view("direction")].as_string());
                // label 可选：缺失即 id。
                if (port.contains(std::string_view("label")))
                {
                    if (auto label =
                            port[std::string_view("label")].as_string();
                        label.has_value() && !label->empty())
                    {
                        port_entry.label = std::string(*label);
                    }
                    else
                    {
                        port_entry.label = port_entry.port_id;
                    }
                }
                else
                {
                    port_entry.label = port_entry.port_id;
                }
                ports.push_back(std::move(port_entry));
                ++port_index;
            }
            faces.emplace(std::string(*node_id), std::move(ports));
            ++entry_index;
        }
        return faces;
    }

    // entryFaces context 键解析（fail-closed 逐条；畸形→该键整体零
    // entryFaces + WARNING + 会话继续——nodeFaces 同款降级）。条目形状：
    // {entryId, ports:[...], types?:[{name[, compatibleWith]}]}——
    // entryId 是节点 settings 所持的正整数（现仓 taskRepositoryRef 判定
    // 的 wire 形态）；口条目锁形与 nodeFaces 共享 FacePortShapeValid 单
    // 一来源；重复 entryId 视为形状违规（每条目一条）。types[] 为可选注
    // 册类型透传容器（消费随分区退役的投影侧类型注册表落地）。
    std::optional<std::pair<
        Das::Core::GraphRuntime::GraphCompiler::EntryFacesMap,
        std::vector<Das::Core::GraphRuntime::GraphCompiler::DocumentFaceType>>>
    ParseEntryFaces(const yyjson::value& root)
    {
        auto arr = root.as_array();
        if (!arr.has_value())
        {
            DAS_CORE_LOG_WARN(
                "context entryFaces key rejected: value is not an array; "
                "the session continues with no injected entry faces");
            return std::nullopt;
        }
        Das::Core::GraphRuntime::GraphCompiler::EntryFacesMap faces;
        std::vector<Das::Core::GraphRuntime::GraphCompiler::DocumentFaceType>
                    types;
        std::size_t entry_index = 0;
        for (const auto& entry_value : *arr)
        {
            const auto reject = [entry_index](std::string_view reason)
            {
                DAS_CORE_LOG_WARN(
                    "context entryFaces key rejected at entry {}: {}; the "
                    "session continues with no injected entry faces",
                    entry_index,
                    reason);
            };
            auto entry_obj = entry_value.as_object();
            if (!entry_obj.has_value()
                || !entry_obj->contains(std::string_view("entryId")))
            {
                reject("entry is not an object or carries no entryId");
                return std::nullopt;
            }
            const auto entry_id =
                (*entry_obj)[std::string_view("entryId")].as_int();
            if (!entry_id.has_value() || *entry_id <= 0)
            {
                reject("entryId is not a positive integer");
                return std::nullopt;
            }
            const std::string entry_key = std::to_string(*entry_id);
            if (faces.find(entry_key) != faces.end())
            {
                reject(
                    "duplicate entryId (the wire contract is one entry "
                    "per repository entry)");
                return std::nullopt;
            }
            if (!entry_obj->contains(std::string_view("ports")))
            {
                reject("entry carries no ports array");
                return std::nullopt;
            }
            auto ports_arr = (*entry_obj)[std::string_view("ports")].as_array();
            if (!ports_arr.has_value())
            {
                reject("ports is not an array");
                return std::nullopt;
            }
            std::vector<
                Das::Core::GraphRuntime::GraphCompiler::DocumentFacePort>
                ports;
            ports.reserve(ports_arr->size());
            std::size_t port_index = 0;
            for (const auto& port_value : *ports_arr)
            {
                if (!FacePortShapeValid(port_value))
                {
                    reject(
                        "port " + std::to_string(port_index)
                        + " violates the {id,type,direction[,label]} "
                          "shape");
                    return std::nullopt;
                }
                auto        port_obj = port_value.as_object();
                const auto& port = *port_obj;
                Das::Core::GraphRuntime::GraphCompiler::DocumentFacePort
                    port_entry;
                port_entry.port_id =
                    std::string(*port[std::string_view("id")].as_string());
                port_entry.port_type =
                    std::string(*port[std::string_view("type")].as_string());
                port_entry.direction = std::string(
                    *port[std::string_view("direction")].as_string());
                if (port.contains(std::string_view("label")))
                {
                    if (auto label =
                            port[std::string_view("label")].as_string();
                        label.has_value() && !label->empty())
                    {
                        port_entry.label = std::string(*label);
                    }
                    else
                    {
                        port_entry.label = port_entry.port_id;
                    }
                }
                else
                {
                    port_entry.label = port_entry.port_id;
                }
                ports.push_back(std::move(port_entry));
                ++port_index;
            }
            // types?[]：可选注册类型透传（{name[, compatibleWith]}——
            // compatibleWith 元素限字符串；非字符串/空 name 均为形状违
            // 规）。
            if (entry_obj->contains(std::string_view("types")))
            {
                auto types_arr =
                    (*entry_obj)[std::string_view("types")].as_array();
                if (!types_arr.has_value())
                {
                    reject("types is not an array");
                    return std::nullopt;
                }
                for (const auto& type_value : *types_arr)
                {
                    auto type_obj = type_value.as_object();
                    if (!type_obj.has_value()
                        || !type_obj->contains(std::string_view("name")))
                    {
                        reject(
                            "types element violates the "
                            "{name[, compatibleWith]} shape");
                        return std::nullopt;
                    }
                    auto name =
                        (*type_obj)[std::string_view("name")].as_string();
                    if (!name.has_value() || name->empty())
                    {
                        reject(
                            "types element name is empty or not a "
                            "string");
                        return std::nullopt;
                    }
                    Das::Core::GraphRuntime::GraphCompiler::DocumentFaceType
                        type_entry;
                    type_entry.name = std::string(*name);
                    if (type_obj->contains(std::string_view("compatibleWith")))
                    {
                        auto compatible =
                            (*type_obj)[std::string_view("compatibleWith")]
                                .as_array();
                        if (!compatible.has_value())
                        {
                            reject("compatibleWith is not an array");
                            return std::nullopt;
                        }
                        for (const auto& compatible_name : *compatible)
                        {
                            auto compatible_text = compatible_name.as_string();
                            if (!compatible_text.has_value()
                                || compatible_text->empty())
                            {
                                reject(
                                    "compatibleWith element is not a "
                                    "non-empty string");
                                return std::nullopt;
                            }
                            type_entry.compatible_with.emplace_back(
                                *compatible_text);
                        }
                    }
                    types.push_back(std::move(type_entry));
                }
            }
            faces.emplace(std::move(entry_key), std::move(ports));
            ++entry_index;
        }
        return std::make_pair(std::move(faces), std::move(types));
    }

    // actionCatalog 数组的逐条 fail-closed 解析——SeedFromContext 的
    // context 键播种通道的唯一逻辑源。任何形状违规→nullopt（调用方降
    // 级为零 catalog+WARNING 且会话继续——fail-closed 不落半截结果）。
    std::optional<
        std::vector<Das::Core::GraphRuntime::Contract::ActionCatalogEntry>>
    ParseActionCatalogEntries(const yyjson::value& root)
    {
        auto arr = root.as_array();
        if (!arr)
        {
            return std::nullopt;
        }
        std::vector<Das::Core::GraphRuntime::Contract::ActionCatalogEntry>
            entries;
        entries.reserve(arr->size());
        for (const auto& v : *arr)
        {
            auto entry_obj = v.as_object();
            if (!entry_obj
                || !entry_obj->contains(std::string_view("actionId")))
            {
                return std::nullopt;
            }
            auto action_id =
                (*entry_obj)[std::string_view("actionId")].as_string();
            if (!action_id || action_id->empty())
            {
                return std::nullopt;
            }
            Das::Core::GraphRuntime::Contract::ActionCatalogEntry entry;
            entry.action_id = *action_id;
            // Missing/empty label falls back to the actionId; a present
            // non-string label is a shape violation.
            entry.label = *action_id;
            if (entry_obj->contains(std::string_view("label")))
            {
                auto label =
                    (*entry_obj)[std::string_view("label")].as_string();
                if (!label)
                {
                    return std::nullopt;
                }
                if (!label->empty())
                {
                    entry.label = *label;
                }
            }
            if (entry_obj->contains(std::string_view("ports")))
            {
                auto ports_val =
                    (*entry_obj)[std::string_view("ports")].as_array();
                if (!ports_val)
                {
                    return std::nullopt;
                }
                entry.ports.reserve(ports_val->size());
                for (const auto& port_val : *ports_val)
                {
                    auto port_obj = port_val.as_object();
                    // 端口锁形规则单一来源：CatalogPortShapeValid（与端口
                    // 面构建段共享——改键集/值域只改那里）。loopCarried 在
                    // 此只验不存：wire 投影不携带它，端口面通道从目录原文
                    // 重读。
                    if (!port_obj || !CatalogPortShapeValid(port_val))
                    {
                        return std::nullopt;
                    }
                    Das::Core::GraphRuntime::Contract::GraphNodePort port;
                    if (auto id =
                            (*port_obj)[std::string_view("portId")].as_string();
                        id && !id->empty())
                    {
                        port.port_id = *id;
                    }
                    else
                    {
                        return std::nullopt;
                    }
                    if (auto type = (*port_obj)[std::string_view("portType")]
                                        .as_string();
                        type && !type->empty())
                    {
                        port.port_type = *type;
                    }
                    else
                    {
                        return std::nullopt;
                    }
                    auto direction =
                        (*port_obj)[std::string_view("direction")].as_string();
                    if (direction
                        && (*direction == std::string_view("input")
                            || *direction == std::string_view("output")))
                    {
                        port.direction = *direction;
                    }
                    else
                    {
                        return std::nullopt;
                    }
                    if (port_obj->contains(std::string_view("displayLabel")))
                    {
                        auto display_label =
                            (*port_obj)[std::string_view("displayLabel")]
                                .as_string();
                        if (!display_label)
                        {
                            return std::nullopt;
                        }
                        port.display_label = *display_label;
                    }
                    entry.ports.push_back(std::move(port));
                }
            }
            // Optional config channel:
            // {name,type,default?,taskGuid?,label?,required?,options?}
            // elements with the element key set locked to exactly those keys
            // — the same whole-catalog rejection on malformed input as the
            // port channel.
            if (entry_obj->contains(std::string_view("config")))
            {
                auto config_val =
                    (*entry_obj)[std::string_view("config")].as_array();
                if (!config_val)
                {
                    return std::nullopt;
                }
                entry.config_fields.reserve(config_val->size());
                for (const auto& config_item : *config_val)
                {
                    auto config_obj = config_item.as_object();
                    if (!config_obj
                        || !config_obj->contains(std::string_view("name"))
                        || !config_obj->contains(std::string_view("type")))
                    {
                        return std::nullopt;
                    }
                    for (const auto& [key, value] : *config_obj)
                    {
                        (void)value;
                        if (key != std::string_view("name")
                            && key != std::string_view("type")
                            && key != std::string_view("default")
                            && key != std::string_view("taskGuid")
                            && key != std::string_view("label")
                            && key != std::string_view("description")
                            && key != std::string_view("required")
                            && key != std::string_view("options"))
                        {
                            return std::nullopt;
                        }
                    }
                    auto name =
                        (*config_obj)[std::string_view("name")].as_string();
                    if (!name || name->empty())
                    {
                        return std::nullopt;
                    }
                    auto type =
                        (*config_obj)[std::string_view("type")].as_string();
                    if (!type || type->empty())
                    {
                        return std::nullopt;
                    }
                    Das::Core::GraphRuntime::Contract::ConfigFieldDescriptor
                        field;
                    field.name = *name;
                    field.type = *type;
                    if (config_obj->contains(std::string_view("default")))
                    {
                        const auto& default_value =
                            (*config_obj)[std::string_view("default")];
                        if (!default_value.is_null())
                        {
                            field.default_value =
                                Das::Utils::CloneYyjsonValue(default_value);
                        }
                    }
                    // Optional declared target task type of a
                    // component-domain field: a non-string value is a shape
                    // violation; an empty string reads as "none declared"
                    // (the same engaged-only reading the producer applies).
                    if (config_obj->contains(std::string_view("taskGuid")))
                    {
                        auto task_guid =
                            (*config_obj)[std::string_view("taskGuid")]
                                .as_string();
                        if (!task_guid)
                        {
                            return std::nullopt;
                        }
                        if (!task_guid->empty())
                        {
                            field.task_guid = *task_guid;
                        }
                    }
                    // Optional producer-resolved display label of the field:
                    // a non-string value is a shape violation; an empty or
                    // absent label reads as "none declared" and the fields
                    // projection falls back to the field name (the same
                    // engaged-only reading the producer applies).
                    if (config_obj->contains(std::string_view("label")))
                    {
                        auto label = (*config_obj)[std::string_view("label")]
                                         .as_string();
                        if (!label)
                        {
                            return std::nullopt;
                        }
                        if (!label->empty())
                        {
                            field.label = *label;
                        }
                    }
                    // Optional producer-resolved help text of the field:
                    // same shape rules as label (a non-string value is a
                    // shape violation; empty or absent reads as "none
                    // declared" — the projection then omits the wire key).
                    if (config_obj->contains(std::string_view("description")))
                    {
                        auto description =
                            (*config_obj)[std::string_view("description")]
                                .as_string();
                        if (!description)
                        {
                            return std::nullopt;
                        }
                        if (!description->empty())
                        {
                            field.description = *description;
                        }
                    }
                    // Optional required declaration: a non-bool value is a
                    // shape violation rejecting the whole catalog; absent or
                    // false reads as not required (the projection always
                    // emits the boolean so consumers see the declaration).
                    if (config_obj->contains(std::string_view("required")))
                    {
                        auto required =
                            (*config_obj)[std::string_view("required")]
                                .as_bool();
                        if (!required)
                        {
                            return std::nullopt;
                        }
                        field.required = *required;
                    }
                    // Optional closed candidate-list declaration: a non-empty
                    // array of strings (the field's value domain, e.g. an
                    // operator vocabulary). A non-array value or any
                    // non-string/empty element is a shape violation rejecting
                    // the whole catalog (fail-closed); an empty array reads
                    // as not declared (engaged-only, the producer never
                    // emits one).
                    if (config_obj->contains(std::string_view("options")))
                    {
                        auto options =
                            (*config_obj)[std::string_view("options")]
                                .as_array();
                        if (!options)
                        {
                            return std::nullopt;
                        }
                        field.options.reserve(options->size());
                        for (const auto& option : *options)
                        {
                            auto option_text = option.as_string();
                            if (!option_text || option_text->empty())
                            {
                                return std::nullopt;
                            }
                            field.options.emplace_back(*option_text);
                        }
                    }
                    entry.config_fields.push_back(std::move(field));
                }
            }
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    // 新建空图预置 begin+end 两节点：仅当 seed 后文档仍空且非
    // formSequence 视图（LINEAR_TAG）——formSequence 上下文与非空图不
    // 预置。内部 DTO 直写绕过 addGraphNode 校验（时序上 catalog 尚未就
    // 位——节点只携带 componentGuid，合法）；节点 id 用 n_0/n_1，与
    // AllocateStableNodeId 在空文档上的产出逐字相同（计数器从
    // existing.size() 起扫），后续 addGraphNode 顺延到 n_2。
    void SeedBoundaryNodesForNewGraph(GraphAuthoringSessionState& s)
    {
        if (!s.document.nodes.empty()
            || Das::Core::GraphRuntime::Contract::IsLinear(s.document))
        {
            return;
        }
        const auto make_boundary_node =
            [](const std::string_view node_id,
               const std::string_view component_guid)
        {
            Das::Core::GraphRuntime::Dto::GraphNodeDto node;
            node.node_id = std::string(node_id);
            node.target.target_kind = "componentRef";
            node.target.component_ref =
                Das::Core::GraphRuntime::Dto::ComponentRefDto{
                    "componentRef",
                    std::string(component_guid),
                    std::string(
                        Das::Core::GraphRuntime::GRAPH_TASK_PLUGIN_GUID)};
            return node;
        };
        s.document.nodes.push_back(make_boundary_node(
            "n_0",
            Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID));
        s.document.nodes.push_back(make_boundary_node(
            "n_1",
            Das::Core::GraphRuntime::GRAPH_END_COMPONENT_GUID));
    }
} // anonymous namespace

DAS_C_API DasResult CreateGraphAuthoringSession(
    IDasReadOnlyString*          p_context_json,
    GraphAuthoringSessionState** pp_out)
{
    if (pp_out == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out = nullptr;

    try
    {
        auto*       s = new GraphAuthoringSessionState();
        std::string ctx;
        if (ReadUtf8(p_context_json, ctx))
        {
            SeedFromContext(*s, ctx);
        }
        SeedBoundaryNodesForNewGraph(*s);
        *pp_out = s;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DAS_C_API void DestroyGraphAuthoringSession(GraphAuthoringSessionState* p)
{
    delete p;
}

DAS_C_API DasResult GraphAuthoringSessionGetDocument(
    GraphAuthoringSessionState*      p,
    IDasReadOnlyString*              p_request_json,
    Das::ExportInterface::IDasJson** pp_out_document_json)
{
    std::ignore = p_request_json;
    if (p == nullptr || pp_out_document_json == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_document_json = nullptr;

    try
    {
        // Build the optional ActionCatalog projection from the seeded
        // entries. SeedFromContext stores the context actionCatalog array's
        // {actionId,label} objects as-is (label already carries the manifest
        // kind, with the actionId fallback applied at parse time), so the
        // projection is a direct copy of the stored catalog.
        const auto catalog = MakeCatalogFromState(*p);
        // D-84-08: 严格 Get envelope = {document, diagnostics}。
        // SchedulerService ValidateAuthoringGetResult 要求 top-level
        // diagnostics 数组（即使空）。Core authoring 没有 PiDiagnostic 来源，
        // diagnostics 永远为空数组（满足 validator required 约束）。
        auto document = Das::Core::GraphRuntime::Contract::SerializeDocument(
            p->document,
            catalog,
            &p->node_faces,
            &p->entry_faces);

        yyjson::value envelope(Das::Utils::MakeYyjsonObject());
        auto          env_obj = envelope.as_object();
        (*env_obj)[std::string_view("document")] = std::move(document);
        (*env_obj)[std::string_view("diagnostics")] =
            Das::Utils::MakeYyjsonArray();

        auto str = Das::Utils::SerializeYyjsonValue(envelope);
        if (!str)
        {
            return DAS_E_FAIL;
        }
        return ParseDasJsonFromString(str->c_str(), pp_out_document_json);
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (...)
    {
        return DAS_E_FAIL;
    }
}

DAS_C_API DasResult GraphAuthoringSessionApplyChange(
    GraphAuthoringSessionState*      p,
    IDasReadOnlyString*              p_change_json,
    Das::ExportInterface::IDasJson** pp_out_result_json)
{
    if (p == nullptr || pp_out_result_json == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_result_json = nullptr;

    try
    {
        std::string change_utf8;
        if (!ReadUtf8(p_change_json, change_utf8))
        {
            return DAS_E_INVALID_POINTER;
        }

        Das::Core::GraphRuntime::Contract::AuthoringChange change;
        int64_t                                            base_revision = 0;
        if (change_utf8.empty())
        {
            auto r = WrapValue(
                BuildChangeResultJson(
                    {Das::Core::GraphRuntime::Contract::ChangeErrorKind::
                         InvalidOp,
                     "empty change request",
                     {},
                     std::nullopt},
                    p->document.version,
                    p->document,
                    MakeCatalogFromState(*p),
                    &p->node_faces,
                    &p->entry_faces),
                pp_out_result_json);
            return r;
        }

        auto doc = yyjson::read(change_utf8);
        if (!ParseAuthoringChange(doc, change, base_revision))
        {
            return WrapValue(
                BuildChangeResultJson(
                    {Das::Core::GraphRuntime::Contract::ChangeErrorKind::
                         InvalidOp,
                     "malformed change request",
                     {},
                     std::nullopt},
                    p->document.version,
                    p->document,
                    MakeCatalogFromState(*p),
                    &p->node_faces,
                    &p->entry_faces),
                pp_out_result_json);
        }

        std::vector<std::string> valid_action_ids;
        valid_action_ids.reserve(p->action_entries.size());
        for (const auto& entry : p->action_entries)
        {
            valid_action_ids.push_back(entry.action_id);
        }
        // The declared port surface rides the same session catalog the
        // compile path injects via SetPortSurfaceProvider — connectGraphPorts
        // derives the stored edge_type from the endpoint port categories
        // through it (canonical signal port names resolve with no
        // declaration; undeclared endpoints reject).
        auto r = Das::Core::GraphRuntime::Contract::ApplyAuthoringChange(
            p->document,
            change,
            valid_action_ids,
            MakeCatalogFromState(*p));
        return WrapValue(
            BuildChangeResultJson(
                r,
                static_cast<int32_t>(base_revision),
                p->document,
                MakeCatalogFromState(*p),
                &p->node_faces,
                &p->entry_faces),
            pp_out_result_json);
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (...)
    {
        return DAS_E_FAIL;
    }
}

DAS_C_API DasResult GraphAuthoringSessionCompile(
    GraphAuthoringSessionState*      p,
    IDasReadOnlyString*              p_request_json,
    Das::ExportInterface::IDasJson** pp_out_compiled_plan_json)
{
    std::ignore = p_request_json;
    if (p == nullptr || pp_out_compiled_plan_json == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_compiled_plan_json = nullptr;

    try
    {
        // No factory manager bound at the authoring boundary, so manifest port
        // validation is skipped (acceptable for authoring preview).
        Das::Core::GraphRuntime::GraphCompiler compiler;
        // faces 通道消费：nodeFaces 快照随 context 进入会话状态，此处借
        // 引用注入编译器（编译同步完成，p 在编译期内有效——与 provider
        // 闭包的 action_entries 引用捕获同生命周期前提）；编译期零活查
        // 询（不回调任何外部存储/PluginManager）。
        compiler.BindNodeFaces(&p->node_faces);
        // entryFaces 通道消费 + 绑条目判据的数据快照（与 required 清单
        // 同范式）：taskRepositoryRef 字段名按 guid 自会话 catalog 推导注
        // 入（编译器不回调 catalog）——ResolveNodePorts 据此从节点
        // settings 解析 entryId 并并入 entryFaces[该 entryId]。
        {
            Das::Core::GraphRuntime::GraphCompiler::RepositoryFieldMap
                repository_fields;
            for (const auto& entry : p->action_entries)
            {
                for (const auto& config_field : entry.config_fields)
                {
                    if (config_field.type
                        == std::string_view("taskRepositoryRef"))
                    {
                        repository_fields[entry.action_id].insert(
                            config_field.name);
                    }
                }
            }
            compiler.BindEntryFaces(
                &p->entry_faces,
                std::move(repository_fields));
        }
        // 端口面注入（与 required 清单同范式）：catalog 声明的静态端口面
        // （direction 由 inputs/outputs 声明携带）喂给编译器，使无 factory
        // manager 的 authoring 编译路径也能做端口校验与边界端口推导——
        // 生产执行输入正是这条编译路径的产物。闭包形态=guid 级纯声明面：
        // 动态口不在此展开——begin/end 文档口面经 nodeFaces 通道按节点并
        // 入（ResolveNodePorts 合并、DeriveBoundaryPorts 直读注入面），
        // 绑条目调用节点口面经 entryFaces（同按节点），guid 维分区聚合
        // （同 guid 节点口串面）随之根除。闭包捕获会话指针 p：编译同步
        // 完成，p 在编译期内有效（与既有 action_entries 引用捕获同生命
        // 周期前提）。
        compiler.SetPortSurfaceProvider(
            [p](const std::string& component_guid)
                -> std::optional<Das::Core::GraphRuntime::GraphCompiler::
                                     ComponentPortSurface>
            {
                for (const auto& entry : p->action_entries)
                {
                    if (entry.action_id != component_guid)
                    {
                        continue;
                    }
                    Das::Core::GraphRuntime::GraphCompiler::ComponentPortSurface
                        surface;
                    for (const auto& port : entry.ports)
                    {
                        Das::Core::GraphRuntime::Dto::GraphPortDefinitionDto
                            definition;
                        definition.port_id = port.port_id;
                        definition.display_label = port.display_label;
                        definition.port_type = port.port_type;
                        if (port.direction == std::string_view("input"))
                        {
                            surface.inputs.push_back(std::move(definition));
                        }
                        else
                        {
                            surface.outputs.push_back(std::move(definition));
                        }
                    }
                    return surface;
                }
                return std::nullopt;
            });
        // required 清单由会话 catalog 推导注入（compiler 无 catalog 访问）：
        // action_id 即 GraphTask 域的 componentGuid，config 字段的 required
        // 声明聚合成 component_guid → 字段名集，编译期对空值产出
        // RequiredFieldMissing diagnostic。
        std::unordered_map<std::string, std::unordered_set<std::string>>
            required_fields;
        for (const auto& entry : p->action_entries)
        {
            for (const auto& config_field : entry.config_fields)
            {
                if (config_field.required)
                {
                    required_fields[entry.action_id].insert(config_field.name);
                }
            }
        }
        auto plan = compiler.Compile(p->document, required_fields);

        // begin/end 数量约束是 session graph 视图契约——Core 编译器本体不
        // 检查（formSequence/maapi/缓存 plan 三场景 begin 永远缺席），门
        // 按 !IsLinear 分派。违例 fatal：诊断进 plan.diagnostics、
        // execution_order 清空（UnbackedDataCycle 先例），编译仍返回
        // envelope（诊断经同一出口供 UI ErrorLens 消费）。
        if (!Das::Core::GraphRuntime::Contract::IsLinear(p->document))
        {
            std::vector<std::string> begin_node_ids;
            std::vector<std::string> end_node_ids;
            for (const auto& node : p->document.nodes)
            {
                if (!node.target.component_ref.has_value())
                {
                    continue;
                }
                const std::string& node_component_guid =
                    node.target.component_ref->component_guid;
                if (node_component_guid
                    == Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID)
                {
                    begin_node_ids.push_back(node.node_id);
                }
                else if (
                    node_component_guid
                    == Das::Core::GraphRuntime::GRAPH_END_COMPONENT_GUID)
                {
                    end_node_ids.push_back(node.node_id);
                }
            }

            const auto join_node_ids =
                [](const std::vector<std::string>& node_ids)
            {
                std::string joined;
                for (std::size_t index = 0; index < node_ids.size(); ++index)
                {
                    if (index != 0)
                    {
                        joined += ", ";
                    }
                    joined += node_ids[index];
                }
                return joined;
            };
            const auto append_diagnostic =
                [&plan](
                    Das::Core::GraphRuntime::CompileDiagnosticKind kind,
                    const std::string&                             node_id,
                    std::string                                    message)
            {
                Das::Core::GraphRuntime::CompileEdgeDiagnostic diagnostic;
                diagnostic.kind = kind;
                diagnostic.node_id = node_id;
                diagnostic.message = std::move(message);
                plan.diagnostics.push_back(
                    Das::Core::GraphRuntime::GraphCompiler::DiagnosticToJson(
                        diagnostic));
            };

            bool has_boundary_violation = false;
            if (begin_node_ids.empty())
            {
                append_diagnostic(
                    Das::Core::GraphRuntime::CompileDiagnosticKind::
                        BeginNodeMissing,
                    {},
                    "Graph view requires exactly one Begin node; add a Begin "
                    "node to compile");
                has_boundary_violation = true;
            }
            else if (begin_node_ids.size() > 1)
            {
                append_diagnostic(
                    Das::Core::GraphRuntime::CompileDiagnosticKind::
                        BeginNodeDuplicate,
                    begin_node_ids.front(),
                    "Graph view requires exactly one Begin node; duplicate "
                    "Begin nodes: "
                        + join_node_ids(begin_node_ids));
                has_boundary_violation = true;
            }
            if (end_node_ids.size() > 1)
            {
                append_diagnostic(
                    Das::Core::GraphRuntime::CompileDiagnosticKind::
                        EndNodeDuplicate,
                    end_node_ids.front(),
                    "Graph view allows at most one End node; duplicate End "
                    "nodes: "
                        + join_node_ids(end_node_ids));
                has_boundary_violation = true;
            }
            if (has_boundary_violation)
            {
                plan.execution_order.clear();
            }
        }

        // Strict authoring compile envelope: the scheduler validates Compile
        // results with OnlyKeys {compile, diagnostics} and extracts the task
        // input from compile.executionInput. The wire artifact traveling
        // inside executionInput is the {"fingerprint","plan"} shell: the
        // whole CompiledGraphPlanDto rides under "plan" and its FNV-1a 64
        // content fingerprint under "fingerprint" (GraphRuntime::Execute
        // re-computes and compares before executing; DasGraphTaskAdapter::Do
        // is a pure pass-through).
        yyjson::value envelope(Das::Utils::MakeYyjsonObject());
        auto          envelope_obj = envelope.as_object();

        yyjson::value compile_section(Das::Utils::MakeYyjsonObject());
        (*compile_section.as_object())[std::string_view("executionInput")] =
            Das::Core::GraphRuntime::BuildCompiledPlanShell(
                yyjson::object(plan));
        (*envelope_obj)[std::string_view("compile")] =
            std::move(compile_section);

        // The validator requires diagnostics to be an array even when empty.
        // 顶层 diagnostics 是调度器边界的严格诊断契约（severity/
        // message/code，拒绝其它键）；plan 内部的 kind/node_id 私有形状
        // 留在 executionInput 透传域（GraphRuntime 测试固化），出口投影
        // 而非克隆——否则编译器诊断缺 severity 必被调度器拒收。
        yyjson::value diagnostics(Das::Utils::MakeYyjsonArray());
        auto          diagnostics_arr = diagnostics.as_array();
        for (const auto& diagnostic : plan.diagnostics)
        {
            diagnostics_arr->emplace_back(
                Das::Core::GraphRuntime::GraphCompiler::
                    DiagnosticToWireContract(diagnostic));
        }
        (*envelope_obj)[std::string_view("diagnostics")] =
            std::move(diagnostics);

        return WrapValue(std::move(envelope), pp_out_compiled_plan_json);
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (...)
    {
        return DAS_E_FAIL;
    }
}

DAS_C_API DasResult
GraphAuthoringSessionUpgradeToGraph(GraphAuthoringSessionState* p)
{
    if (p == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    return Das::Core::GraphRuntime::Contract::UpgradeToGraph(p->document)
               ? DAS_S_OK
               : DAS_E_FAIL;
}

// 图文档边界口推导 C ABI：声明口面通道改读会话状态（action_entries——
// 与编译路径的端口面注入同款形态：catalog 声明的静态口面按
// actionId==component_guid 解析，direction 由 ports 声明携带）。会话与
// 推导同步完成，p 在推导期内有效。
DAS_C_API DasResult GraphDeriveBoundaryPorts(
    GraphAuthoringSessionState* p,
    IDasReadOnlyString*         p_document_json,
    IDasReadOnlyString**        pp_out_boundary_json)
{
    if (pp_out_boundary_json == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_boundary_json = nullptr;
    if (p == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    std::string document_text;
    if (!ReadUtf8(p_document_json, document_text) || document_text.empty())
    {
        return DAS_E_INVALID_POINTER;
    }

    try
    {
        auto document_root = yyjson::read(document_text);
        auto document =
            yyjson::cast<Das::Core::GraphRuntime::Dto::GraphDocumentDto>(
                document_root);

        Das::Core::GraphRuntime::GraphCompiler::ComponentPortSurfaceProvider
            provider = [p](const std::string& component_guid)
            -> std::optional<
                Das::Core::GraphRuntime::GraphCompiler::ComponentPortSurface>
        {
            for (const auto& entry : p->action_entries)
            {
                if (entry.action_id != component_guid)
                {
                    continue;
                }
                Das::Core::GraphRuntime::GraphCompiler::ComponentPortSurface
                    surface;
                for (const auto& port : entry.ports)
                {
                    Das::Core::GraphRuntime::Dto::GraphPortDefinitionDto
                        definition;
                    definition.port_id = port.port_id;
                    definition.display_label = port.display_label;
                    definition.port_type = port.port_type;
                    if (port.direction == std::string_view("input"))
                    {
                        surface.inputs.push_back(std::move(definition));
                    }
                    else
                    {
                        surface.outputs.push_back(std::move(definition));
                    }
                }
                return surface;
            }
            return std::nullopt;
        };

        auto boundary =
            Das::Core::GraphRuntime::GraphCompiler::DeriveBoundaryPorts(
                document,
                provider);
        auto boundary_value = yyjson::object(boundary);
        auto serialized = Das::Utils::SerializeYyjsonValue(boundary_value);
        if (!serialized.has_value())
        {
            return DAS_E_FAIL;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            serialized->c_str(),
            pp_out_boundary_json);
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (...)
    {
        return DAS_E_FAIL;
    }
}

// begin/end 边界组件 authoring 会话的 create 出口。component_guid 参数
// 决定会话角色（begin/end 之一；方向固化的信任基础）——其它 guid 拒绝
// （fail-closed）。context_json 携带该节点 settings（含 boundaryPorts）
// 与顶层 revision；null/空串 = 空 settings 起步（engaged-only）。
DAS_C_API DasResult CreateGraphBoundaryAuthoringSession(
    const DasGuid*                                   p_component_guid,
    IDasReadOnlyString*                              p_context_json,
    Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session)
{
    if (pp_out_session == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_session = nullptr;
    if (p_component_guid == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    const auto role =
        Das::Core::GraphRuntime::GraphBoundaryAuthoringSession::RoleOfGuid(
            *p_component_guid);
    if (role
        == Das::Core::GraphRuntime::GraphBoundaryAuthoringSession::
            BoundaryRole::kNone)
    {
        DAS_CORE_LOG_ERROR(
            "CreateGraphBoundaryAuthoringSession: component guid is not a "
            "boundary component (begin/end)");
        return DAS_E_INVALID_ARGUMENT;
    }

    std::string context;
    if (!ReadUtf8(p_context_json, context))
    {
        return DAS_E_INVALID_POINTER;
    }

    try
    {
        auto* session =
            new Das::Core::GraphRuntime::GraphBoundaryAuthoringSession(
                role,
                context);
        session->AddRef();
        *pp_out_session = session;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

// ===========================================================================
// 组件绑定扫描 / 节点写回（调度编排支持）——图文档形状判据知识本体。
// ===========================================================================
namespace
{
    // 节点的 componentRef 组件 guid（targetKind 非 componentRef 或形状
    // 不全时返回空串）。
    std::string ComponentGuidOfNode(const yyjson::value& node_value)
    {
        auto node_obj = node_value.as_object();
        if (!node_obj || !node_obj->contains(std::string_view("target")))
        {
            return {};
        }
        auto target = (*node_obj)[std::string_view("target")].as_object();
        if (!target)
        {
            return {};
        }
        auto target_kind =
            (*target)[std::string_view("targetKind")].as_string();
        if (!target_kind || *target_kind != std::string_view("componentRef"))
        {
            return {};
        }
        if (!target->contains(std::string_view("componentRef")))
        {
            return {};
        }
        const auto& ref = (*target)[std::string_view("componentRef")];
        if (ref.is_null())
        {
            return {};
        }
        auto ref_obj = ref.as_object();
        if (!ref_obj)
        {
            return {};
        }
        auto guid = (*ref_obj)[std::string_view("componentGuid")].as_string();
        if (!guid)
        {
            return {};
        }
        return std::string(*guid);
    }

    // 组件 guid → taskRepositoryRef 配置字段名列表（数据源=actionCatalog
    // 数组的 config 声明——与编译侧绑条目判据同一目录语义，业务中立不
    // 特判组件）。catalog 缺席/非数组/畸形 → 空表（无绑定判据可用）。
    std::unordered_map<std::string, std::vector<std::string>>
    CollectRepositoryRefFields(const yyjson::value& catalog_root)
    {
        std::unordered_map<std::string, std::vector<std::string>> fields;
        auto catalog = catalog_root.as_array();
        if (!catalog)
        {
            return fields;
        }
        for (const auto& entry_value : *catalog)
        {
            auto entry_obj = entry_value.as_object();
            if (!entry_obj
                || !entry_obj->contains(std::string_view("actionId")))
            {
                continue;
            }
            auto action_id =
                (*entry_obj)[std::string_view("actionId")].as_string();
            if (!action_id || action_id->empty())
            {
                continue;
            }
            if (!entry_obj->contains(std::string_view("config")))
            {
                continue;
            }
            auto config = (*entry_obj)[std::string_view("config")].as_array();
            if (!config)
            {
                continue;
            }
            for (const auto& field_value : *config)
            {
                auto field_obj = field_value.as_object();
                if (!field_obj
                    || !field_obj->contains(std::string_view("type")))
                {
                    continue;
                }
                auto type = (*field_obj)[std::string_view("type")].as_string();
                if (!type || *type != std::string_view("taskRepositoryRef"))
                {
                    continue;
                }
                if (!field_obj->contains(std::string_view("name")))
                {
                    continue;
                }
                auto name = (*field_obj)[std::string_view("name")].as_string();
                if (!name || name->empty())
                {
                    continue;
                }
                fields[std::string(*action_id)].emplace_back(*name);
            }
        }
        return fields;
    }

    // 绑条目判定（判据=编译侧 BoundRepositoryEntryIdOf 同款语义：声明的
    // taskRepositoryRef 字段持正整数值）——返回该正整数，nullopt=未绑。
    std::optional<int64_t> BoundEntryIdOfNode(
        const yyjson::value&            node_value,
        const std::vector<std::string>& repository_field_names)
    {
        auto node_obj = node_value.as_object();
        if (!node_obj || !node_obj->contains(std::string_view("settings")))
        {
            return std::nullopt;
        }
        auto settings_obj =
            (*node_obj)[std::string_view("settings")].as_object();
        if (!settings_obj)
        {
            return std::nullopt;
        }
        for (const auto& field_name : repository_field_names)
        {
            if (!settings_obj->contains(std::string_view(field_name)))
            {
                continue;
            }
            const auto& value = (*settings_obj)[std::string_view(field_name)];
            if (value.is_int())
            {
                const auto entry_id = value.as_int();
                if (entry_id.has_value() && *entry_id > 0)
                {
                    return *entry_id;
                }
            }
        }
        return std::nullopt;
    }

    // 节点 settings 的克隆读取（绑定清单的 settings 段）。
    yyjson::value NodeSettingsOf(const yyjson::value& node_value)
    {
        auto node_obj = node_value.as_object();
        if (node_obj && node_obj->contains(std::string_view("settings")))
        {
            const auto& settings_ref =
                (*node_obj)[std::string_view("settings")];
            return Das::Utils::CloneYyjsonValue(settings_ref);
        }
        return Das::Utils::MakeYyjsonObject();
    }

    // 单条边的级联命中判定：任一端点落在（node_id, removed_port_ids）内
    // 即删（removeNodePort apply 级联 erase 的双侧删语义）。
    bool EdgeReferencesRemovedPort(
        const yyjson::value&            edge,
        std::string_view                node_id,
        const std::vector<std::string>& removed_port_ids)
    {
        auto edge_obj = edge.as_object();
        if (!edge_obj)
        {
            return false;
        }
        auto source_node =
            (*edge_obj)[std::string_view("sourceNodeId")].as_string();
        auto source_port =
            (*edge_obj)[std::string_view("sourcePortId")].as_string();
        auto target_node =
            (*edge_obj)[std::string_view("targetNodeId")].as_string();
        auto target_port =
            (*edge_obj)[std::string_view("targetPortId")].as_string();
        const bool source_hit = source_node.has_value()
                                && *source_node == node_id
                                && source_port.has_value()
                                && std::find(
                                       removed_port_ids.begin(),
                                       removed_port_ids.end(),
                                       *source_port)
                                       != removed_port_ids.end();
        const bool target_hit = target_node.has_value()
                                && *target_node == node_id
                                && target_port.has_value()
                                && std::find(
                                       removed_port_ids.begin(),
                                       removed_port_ids.end(),
                                       *target_port)
                                       != removed_port_ids.end();
        return source_hit || target_hit;
    }

    // 序列化 + 经 ParseDasJsonFromString 装配 IDasJson 出参（本文件 C ABI
    // 的既有出参形态）。
    DasResult EmitJsonOutput(
        const yyjson::value&             value,
        Das::ExportInterface::IDasJson** pp_out)
    {
        auto serialized = Das::Utils::SerializeYyjsonValue(value);
        if (!serialized.has_value())
        {
            return DAS_E_FAIL;
        }
        return ParseDasJsonFromString(serialized->c_str(), pp_out);
    }
} // namespace

// 图文档组件绑定扫描：非图文档（无 nodes 数组）输出空数组（调用方零
// faces 零动作）；畸形文档 JSON 返回 DAS_E_INVALID_JSON（fail-closed）。
// catalog 缺席（null/空串）= 无绑定判据（全部按未绑输出）。
DAS_C_API DasResult GraphScanComponentBindings(
    IDasReadOnlyString*              p_graph_document_json,
    IDasReadOnlyString*              p_action_catalog_json,
    Das::ExportInterface::IDasJson** pp_out_bindings)
{
    if (pp_out_bindings == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_bindings = nullptr;

    std::string document_text;
    if (p_graph_document_json == nullptr
        || !ReadUtf8(p_graph_document_json, document_text)
        || document_text.empty())
    {
        return DAS_E_INVALID_POINTER;
    }

    yyjson::value bindings(Das::Utils::MakeYyjsonArray());
    try
    {
        yyjson::value catalog_root(Das::Utils::MakeYyjsonArray());
        if (p_action_catalog_json != nullptr)
        {
            std::string catalog_text;
            if (ReadUtf8(p_action_catalog_json, catalog_text)
                && !catalog_text.empty())
            {
                try
                {
                    catalog_root = yyjson::read(catalog_text);
                }
                catch (...)
                {
                    // catalog 畸形：无绑定判据可用（空表），扫描继续——
                    // 绑定判据缺席不阻断清单产出。
                    catalog_root = Das::Utils::MakeYyjsonArray();
                }
            }
        }
        const auto repository_ref_fields =
            CollectRepositoryRefFields(catalog_root);

        auto document_root = yyjson::read(document_text);
        auto document_obj = document_root.as_object();
        if (document_obj && document_obj->contains(std::string_view("nodes")))
        {
            const auto& nodes_ref = (*document_obj)[std::string_view("nodes")];
            auto        nodes_arr = nodes_ref.as_array();
            if (nodes_arr)
            {
                for (const auto& node_value : *nodes_arr)
                {
                    auto node_obj = node_value.as_object();
                    if (!node_obj
                        || !node_obj->contains(std::string_view("nodeId")))
                    {
                        continue;
                    }
                    auto node_id =
                        (*node_obj)[std::string_view("nodeId")].as_string();
                    if (!node_id || node_id->empty())
                    {
                        continue;
                    }
                    const std::string component_guid =
                        ComponentGuidOfNode(node_value);
                    if (component_guid.empty())
                    {
                        continue;
                    }

                    yyjson::value binding(Das::Utils::MakeYyjsonObject());
                    auto          binding_obj = binding.as_object();
                    (*binding_obj)[std::string_view("nodeId")] = std::make_pair(
                        std::string_view(*node_id),
                        yyjson::copy_string);
                    (*binding_obj)[std::string_view("componentGuid")] =
                        std::make_pair(
                            std::string_view(component_guid),
                            yyjson::copy_string);
                    const auto field_it =
                        repository_ref_fields.find(component_guid);
                    if (field_it != repository_ref_fields.end())
                    {
                        auto bound_entry =
                            BoundEntryIdOfNode(node_value, field_it->second);
                        if (bound_entry.has_value())
                        {
                            (*binding_obj)[std::string_view("boundEntryId")] =
                                *bound_entry;
                        }
                    }
                    (*binding_obj)[std::string_view("settings")] =
                        NodeSettingsOf(node_value);
                    bindings.as_array()->emplace_back(std::move(binding));
                }
            }
        }
        return EmitJsonOutput(bindings, pp_out_bindings);
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (...)
    {
        return DAS_E_INVALID_JSON;
    }
}

// 图文档节点写回：settings 整体替换 + 被删口边级联（重建式——nodes 逐
// 条克隆、edges 过滤、其余顶层键原样克隆）。节点不存在时输出原文档的
// 副本（调用方按需持久化）。
DAS_C_API DasResult GraphWriteComponentNodeDocument(
    IDasReadOnlyString*              p_graph_document_json,
    IDasReadOnlyString*              p_node_id,
    IDasReadOnlyString*              p_node_settings_json,
    IDasReadOnlyString*              p_removed_port_ids_json,
    Das::ExportInterface::IDasJson** pp_out_document)
{
    if (pp_out_document == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_document = nullptr;

    std::string document_text;
    std::string node_id_text;
    std::string settings_text;
    std::string removed_text;
    if (p_graph_document_json == nullptr
        || !ReadUtf8(p_graph_document_json, document_text)
        || document_text.empty() || p_node_id == nullptr
        || !ReadUtf8(p_node_id, node_id_text) || node_id_text.empty()
        || p_node_settings_json == nullptr
        || !ReadUtf8(p_node_settings_json, settings_text))
    {
        return DAS_E_INVALID_POINTER;
    }

    try
    {
        auto                     document_root = yyjson::read(document_text);
        yyjson::value            new_settings = yyjson::read(settings_text);
        std::vector<std::string> removed_port_ids;
        if (p_removed_port_ids_json != nullptr
            && ReadUtf8(p_removed_port_ids_json, removed_text)
            && !removed_text.empty())
        {
            auto removed_root = yyjson::read(removed_text);
            auto removed_arr = removed_root.as_array();
            if (removed_arr)
            {
                for (const auto& removed_value : *removed_arr)
                {
                    if (auto removed_id = removed_value.as_string();
                        removed_id && !removed_id->empty())
                    {
                        removed_port_ids.emplace_back(*removed_id);
                    }
                }
            }
        }

        bool          node_found = false;
        yyjson::value nodes_out(Das::Utils::MakeYyjsonArray());
        {
            auto document_obj = document_root.as_object();
            if (document_obj
                && document_obj->contains(std::string_view("nodes")))
            {
                const auto& nodes_ref =
                    (*document_obj)[std::string_view("nodes")];
                auto nodes_arr = nodes_ref.as_array();
                if (nodes_arr)
                {
                    for (const auto& node_value : *nodes_arr)
                    {
                        auto node_obj = node_value.as_object();
                        if (node_obj
                            && node_obj->contains(std::string_view("nodeId")))
                        {
                            auto current_id =
                                (*node_obj)[std::string_view("nodeId")]
                                    .as_string();
                            if (current_id
                                && *current_id
                                       == std::string_view(node_id_text))
                            {
                                node_found = true;
                                yyjson::value updated =
                                    Das::Utils::CloneYyjsonValue(node_value);
                                auto updated_obj = updated.as_object();
                                if (updated_obj)
                                {
                                    (*updated_obj)[std::string_view(
                                        "settings")] =
                                        Das::Utils::CloneYyjsonValue(
                                            new_settings);
                                }
                                nodes_out.as_array()->emplace_back(
                                    std::move(updated));
                                continue;
                            }
                        }
                        nodes_out.as_array()->emplace_back(
                            Das::Utils::CloneYyjsonValue(node_value));
                    }
                }
            }
        }
        if (!node_found)
        {
            return EmitJsonOutput(document_root, pp_out_document);
        }

        yyjson::value updated_properties(Das::Utils::MakeYyjsonObject());
        auto          out_obj = updated_properties.as_object();
        {
            auto document_obj = document_root.as_object();
            if (document_obj)
            {
                for (const auto& [key, value] : *document_obj)
                {
                    if (key == std::string_view("nodes"))
                    {
                        (*out_obj)[std::string_view(key)] =
                            std::move(nodes_out);
                    }
                    else if (key == std::string_view("edges"))
                    {
                        yyjson::value edges_out(Das::Utils::MakeYyjsonArray());
                        auto          edges_arr = value.as_array();
                        if (edges_arr)
                        {
                            for (const auto& edge_value : *edges_arr)
                            {
                                if (EdgeReferencesRemovedPort(
                                        edge_value,
                                        std::string_view(node_id_text),
                                        removed_port_ids))
                                {
                                    continue;
                                }
                                edges_out.as_array()->emplace_back(
                                    Das::Utils::CloneYyjsonValue(edge_value));
                            }
                        }
                        (*out_obj)[std::string_view(key)] =
                            std::move(edges_out);
                    }
                    else
                    {
                        (*out_obj)[std::string_view(key)] =
                            Das::Utils::CloneYyjsonValue(value);
                    }
                }
            }
        }
        return EmitJsonOutput(updated_properties, pp_out_document);
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (...)
    {
        return DAS_E_INVALID_JSON;
    }
}
