#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>
#include <das/Core/GraphRuntime/FormSequenceProjector.h>
#include <das/Core/GraphRuntime/GraphBoundaryComponents.h>
#include <das/Core/GraphRuntime/GraphCompiler.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>

#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <functional>
#include <optional>
#include <string>
#include <utility>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// ---------------------------------------------------------------------------
// Sentinel source_node_id marking a broadcast (graph-level input) binding in
// the port binding plan, as opposed to an ordinary point-to-point data edge.
// The binding targets are the derived dangling input ports — the graph's
// boundary inputs.
// ---------------------------------------------------------------------------
namespace
{
    constexpr std::string_view GRAPH_INPUT_BROADCAST_SOURCE = "$graph_input";
}

// ---------------------------------------------------------------------------
// Known port types for the DAS type system
// ---------------------------------------------------------------------------

static const std::unordered_set<std::string>& GetKnownTypes()
{
    static const std::unordered_set<std::string> known_types = {
        "int",
        "int64",
        "float",
        "double",
        "string",
        "bool",
        "image",
        "base",
        "component",
        "signal",
        "json",
        "object",
        "null"};
    return known_types;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

GraphCompiler::GraphCompiler() = default;

// ---------------------------------------------------------------------------
// SetFactoryManager
// ---------------------------------------------------------------------------

void GraphCompiler::SetFactoryManager(
    TaskComponentFactoryManager* factory_manager)
{
    factory_manager_ = factory_manager;
}

// ---------------------------------------------------------------------------
// SetPortSurfaceProvider
// ---------------------------------------------------------------------------

void GraphCompiler::SetPortSurfaceProvider(
    ComponentPortSurfaceProvider provider)
{
    port_surface_provider_ = std::move(provider);
}

// ---------------------------------------------------------------------------
// BindNodeFaces / BindEntryFaces / MergeFacesOntoSurface
// ---------------------------------------------------------------------------

void GraphCompiler::BindNodeFaces(const NodeFacesMap* node_faces)
{
    node_faces_ = node_faces;
}

void GraphCompiler::BindEntryFaces(
    const EntryFacesMap* entry_faces,
    RepositoryFieldMap   repository_fields)
{
    entry_faces_ = entry_faces;
    repository_fields_ = std::move(repository_fields);
}

void GraphCompiler::BindFaceTypes(
    const std::vector<DocumentFaceType>* face_types)
{
    face_types_ = face_types;
}

void GraphCompiler::BindCreator(
    Das::PluginInterface::IDasTaskComponentHost* p_creator)
{
    creator_ = p_creator;
}

void GraphCompiler::MergeFacesOntoSurface(
    ManifestPorts&                       ports,
    const std::string&                   node_id,
    const std::vector<DocumentFacePort>& faces,
    bool                                 expand_pair) const
{
    // faces 本身是一路声明源：带 faces 的节点口面已知（并集语义），
    // 即便 manifest/catalog 双双未解析，边校验也继续按并出的面走口检
    // 查而非 UnresolvableComponentGuid 早退。
    ports.was_resolved = true;
    for (const auto& face : faces)
    {
        // Dedupe by port id — the document face wins over a same-name
        // surviving port and the surface never carries a duplicate id (edge
        // addressing uniqueness). The direction rides the face entry
        // single-sided: a face re-homing a declared input to the output
        // side is a legal document-side override. Boundary (begin/end)
        // nodes merge as port pairs instead — both sides carry the face so
        // in-graph edges off the value anchor resolve.
        bool collided = false;
        for (auto* side : {&ports.inputs, &ports.outputs})
        {
            const auto removed = std::remove_if(
                side->begin(),
                side->end(),
                [&face](const PortEntry& entry)
                { return entry.port_id == face.port_id; });
            if (removed != side->end())
            {
                collided = true;
                side->erase(removed, side->end());
            }
        }
        if (collided)
        {
            DAS_CORE_LOG_WARN(
                "node {} port {} is redeclared by the injected document "
                "face; the face wins (type={})",
                node_id,
                face.port_id,
                face.port_type);
        }
        PortEntry entry;
        entry.port_id = face.port_id;
        entry.port_type = face.port_type;
        PortEntry mirror = entry;
        if (face.direction == std::string_view("input"))
        {
            ports.inputs.push_back(std::move(entry));
            if (expand_pair)
            {
                ports.outputs.push_back(std::move(mirror));
            }
        }
        else
        {
            ports.outputs.push_back(std::move(entry));
            if (expand_pair)
            {
                ports.inputs.push_back(std::move(mirror));
            }
        }
    }
}

std::optional<std::string> GraphCompiler::ResolveBoundEntryId(
    const Dto::GraphNodeDto& node) const
{
    if (repository_fields_.empty() || entry_faces_ == nullptr)
    {
        return std::nullopt;
    }
    if (!node.target.component_ref.has_value())
    {
        return std::nullopt;
    }
    const auto fields =
        repository_fields_.find(node.target.component_ref->component_guid);
    if (fields == repository_fields_.end() || fields->second.empty())
    {
        return std::nullopt;
    }
    auto settings_obj = node.settings.as_object();
    if (!settings_obj.has_value())
    {
        return std::nullopt;
    }
    for (const auto& field_name : fields->second)
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
                return std::to_string(*entry_id);
            }
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// 引用闭包注入 pass（编译期现算新链）
// ---------------------------------------------------------------------------

namespace
{
    /// IDasJson → yyjson（失败/空返回 nullopt）：经 ToString 序列化再解析，
    /// 跨 ABI 边界的通用读法（ReadJsonInterface 同款）。
    std::optional<yyjson::value> JsonToYyjson(
        Das::ExportInterface::IDasJson* json)
    {
        if (json == nullptr)
        {
            return std::nullopt;
        }
        Das::DasPtr<IDasReadOnlyString> text;
        if (DAS::IsFailed(json->ToString(0, text.Put())) || !text)
        {
            return std::nullopt;
        }
        const char* utf8 = nullptr;
        if (DAS::IsFailed(text->GetUtf8(&utf8)) || utf8 == nullptr)
        {
            return std::nullopt;
        }
        auto parsed = Das::Utils::ParseYyjsonFromString(utf8);
        if (!parsed.has_value())
        {
            return std::nullopt;
        }
        return std::move(*parsed);
    }

    /// 会话 GetDocument 响应 → 文档根 ports 自述口面。响应形状取两者之
    /// 一（严格 authoring 信封或裸文档），ports 条目键 id/portId 双兼容。
    std::optional<std::vector<GraphCompiler::DocumentFacePort>>
    ParseDocumentRootPorts(const yyjson::value& response)
    {
        auto root_obj = response.as_object();
        if (!root_obj.has_value())
        {
            return std::nullopt;
        }
        const yyjson::value* document = &response;
        yyjson::value        unwrapped;
        if (root_obj->contains(std::string_view("document")))
        {
            unwrapped = Das::Utils::CloneYyjsonValue(
                (*root_obj)[std::string_view("document")]);
            document = &unwrapped;
        }
        auto doc_obj = document->as_object();
        if (!doc_obj.has_value()
            || !doc_obj->contains(std::string_view("ports")))
        {
            return std::nullopt;
        }
        const auto& ports_value = (*doc_obj)[std::string_view("ports")];
        auto        ports_arr = ports_value.as_array();
        if (!ports_arr.has_value())
        {
            return std::nullopt;
        }

        std::vector<GraphCompiler::DocumentFacePort> faces;
        for (const auto& port : *ports_arr)
        {
            auto port_obj = port.as_object();
            if (!port_obj.has_value())
            {
                continue;
            }
            GraphCompiler::DocumentFacePort face;
            auto id = (*port_obj)[std::string_view("id")].as_string();
            if (!id.has_value()
                && port_obj->contains(std::string_view("portId")))
            {
                id = (*port_obj)[std::string_view("portId")].as_string();
            }
            if (!id.has_value() || id->empty())
            {
                continue;
            }
            face.port_id = std::string(*id);
            auto type = (*port_obj)[std::string_view("type")].as_string();
            if (!type.has_value()
                && port_obj->contains(std::string_view("portType")))
            {
                type = (*port_obj)[std::string_view("portType")].as_string();
            }
            face.port_type = type.has_value() ? std::string(*type) : "string";
            auto direction =
                (*port_obj)[std::string_view("direction")].as_string();
            face.direction =
                direction.has_value() ? std::string(*direction) : "output";
            if (port_obj->contains(std::string_view("label")))
            {
                auto label = (*port_obj)[std::string_view("label")].as_string();
                face.label =
                    label.has_value() ? std::string(*label) : face.port_id;
            }
            else
            {
                face.label = face.port_id;
            }
            faces.push_back(std::move(face));
        }
        return faces;
    }

    /// 会话 Compile 响应 → compile 段 executionInput 原文（shell 串）。
    std::optional<std::string> ParseCompileExecutionInput(
        const yyjson::value& response)
    {
        auto obj = response.as_object();
        if (!obj.has_value() || !obj->contains(std::string_view("compile")))
        {
            return std::nullopt;
        }
        auto compile_obj = (*obj)[std::string_view("compile")].as_object();
        if (!compile_obj.has_value()
            || !compile_obj->contains(std::string_view("executionInput")))
        {
            return std::nullopt;
        }
        const auto& execution_input =
            (*compile_obj)[std::string_view("executionInput")];
        if (const auto text = execution_input.as_string())
        {
            return std::string(*text);
        }
        // 非 string 形态（对象/序列化文本）按原文序列化消费——与
        // DasGraphTaskImpl 的 GetString 读法对齐面为准（string 为正形态）。
        auto serialized = Das::Utils::SerializeYyjsonValue(
            Das::Utils::CloneYyjsonValue(execution_input));
        if (!serialized.has_value())
        {
            return std::nullopt;
        }
        return std::move(*serialized);
    }
} // namespace

void GraphCompiler::RunReferenceClosurePass(
    const Dto::GraphDocumentDto& document)
{
    reference_faces_.clear();
    reference_artifacts_.clear();
    reference_pass_failed_ = false;

    // creator 解析次序：显式绑定 → factory manager host。
    Das::PluginInterface::IDasTaskComponentHost* creator = creator_;
    if (creator == nullptr && factory_manager_ != nullptr)
    {
        creator = factory_manager_->GetTaskComponentHost();
    }

    std::vector<int64_t> entry_stack;
    for (const auto& node : document.nodes)
    {
        if (node.target.target_kind != "componentRef"
            || !node.target.component_ref.has_value())
        {
            continue;
        }
        const auto& component_guid = node.target.component_ref->component_guid;
        const auto  fields = repository_fields_.find(component_guid);
        if (fields == repository_fields_.end() || fields->second.empty())
        {
            continue;
        }

        // 绑条目判定（ResolveBoundEntryId 同款语义，但保留值形态用于
        // 「声明了但值非正整数」的近根因诊断）。
        auto                   settings_obj = node.settings.as_object();
        std::optional<int64_t> bound_entry_id;
        bool                   field_present = false;
        if (settings_obj.has_value())
        {
            for (const auto& field_name : fields->second)
            {
                if (!settings_obj->contains(std::string_view(field_name)))
                {
                    continue;
                }
                field_present = true;
                const auto& value =
                    (*settings_obj)[std::string_view(field_name)];
                if (value.is_int())
                {
                    const auto entry_id = value.as_int();
                    if (entry_id.has_value() && *entry_id > 0)
                    {
                        bound_entry_id = *entry_id;
                        break;
                    }
                }
            }
        }
        if (!bound_entry_id.has_value())
        {
            if (field_present)
            {
                reference_pass_failed_ = true;
                CompileEdgeDiagnostic diagnostic;
                diagnostic.kind =
                    CompileDiagnosticKind::ReferenceEntryValueEmpty;
                diagnostic.node_id = node.node_id;
                diagnostic.message = DAS_FMT_NS::format(
                    "node {} declares a taskRepositoryRef field but its "
                    "value is not a positive entry id",
                    node.node_id);
                DAS_CORE_LOG_WARN(
                    "reference closure pass: {}",
                    diagnostic.message);
                reference_diagnostics_.push_back(DiagnosticToJson(diagnostic));
            }
            continue;
        }

        // entryId 栈环检测（单层栈深 1；递归栈随后续扩展）。
        if (std::find(entry_stack.begin(), entry_stack.end(), *bound_entry_id)
            != entry_stack.end())
        {
            reference_pass_failed_ = true;
            CompileEdgeDiagnostic diagnostic;
            diagnostic.kind = CompileDiagnosticKind::ReferenceCycleDetected;
            diagnostic.node_id = node.node_id;
            diagnostic.message = DAS_FMT_NS::format(
                "repository entry {} appears again on the reference "
                "closure stack (cycle)",
                *bound_entry_id);
            DAS_CORE_LOG_WARN("reference closure pass: {}", diagnostic.message);
            reference_diagnostics_.push_back(DiagnosticToJson(diagnostic));
            continue;
        }
        entry_stack.push_back(*bound_entry_id);

        if (creator == nullptr)
        {
            reference_pass_failed_ = true;
            CompileEdgeDiagnostic diagnostic;
            diagnostic.kind = CompileDiagnosticKind::ReferenceCreatorMissing;
            diagnostic.node_id = node.node_id;
            diagnostic.message =
                "reference closure pass has no creator available (neither "
                "bound creator nor factory manager host)";
            DAS_CORE_LOG_WARN("reference closure pass: {}", diagnostic.message);
            reference_diagnostics_.push_back(DiagnosticToJson(diagnostic));
            continue;
        }

        // entryId→条目最小字段集（manager 路径；无 manager 的会话路径回
        // 节点组件 guid——GraphTask 节点组件 guid 与任务类型 guid 同值，
        // 本 plan 的真值域）。
        std::string   task_type_guid = component_guid;
        yyjson::value properties;
        if (factory_manager_ != nullptr)
        {
            ForeignInterfaceHost::RepositoryEntrySummary summary;
            const auto                                   resolve_result =
                factory_manager_->ResolveRepositoryEntry(
                    *bound_entry_id,
                    &summary);
            if (DAS::IsFailed(resolve_result))
            {
                reference_pass_failed_ = true;
                CompileEdgeDiagnostic diagnostic;
                diagnostic.kind =
                    CompileDiagnosticKind::ReferenceEntryUnresolvable;
                diagnostic.node_id = node.node_id;
                diagnostic.message = DAS_FMT_NS::format(
                    "repository entry {} could not be resolved: result={}",
                    *bound_entry_id,
                    resolve_result);
                DAS_CORE_LOG_WARN(
                    "reference closure pass: {}",
                    diagnostic.message);
                reference_diagnostics_.push_back(DiagnosticToJson(diagnostic));
                continue;
            }
            task_type_guid = summary.task_type_guid;
            properties = std::move(summary.accepted_properties);
        }

        DasGuid resolved_task_guid{};
        try
        {
            resolved_task_guid =
                Das::Core::ForeignInterfaceHost::MakeDasGuid(task_type_guid);
        }
        catch (const std::exception&)
        {
            reference_pass_failed_ = true;
            CompileEdgeDiagnostic diagnostic;
            diagnostic.kind = CompileDiagnosticKind::ReferenceSessionFailed;
            diagnostic.node_id = node.node_id;
            diagnostic.message = DAS_FMT_NS::format(
                "repository entry {} task type guid '{}' is invalid",
                *bound_entry_id,
                task_type_guid);
            DAS_CORE_LOG_WARN("reference closure pass: {}", diagnostic.message);
            reference_diagnostics_.push_back(DiagnosticToJson(diagnostic));
            continue;
        }

        // context：条目绑定键由条目解析后端自行预载（Discretion 裁定）。
        yyjson::value context(Das::Utils::MakeYyjsonObject());
        {
            auto ctx_obj = context.as_object();
            (*ctx_obj)[std::string_view("repositoryEntryId")] = *bound_entry_id;
            if (!properties.is_null())
            {
                (*ctx_obj)[std::string_view("properties")] =
                    Das::Utils::CloneYyjsonValue(properties);
            }
        }
        auto context_serialized = Das::Utils::SerializeYyjsonValue(
            Das::Utils::CloneYyjsonValue(context));
        if (!context_serialized.has_value())
        {
            reference_pass_failed_ = true;
            continue;
        }

        // IDasJson 包装（会话工厂入参形态）。
        Das::DasPtr<Das::ExportInterface::IDasJson> context_json;
        try
        {
            context_json = Das::MakeDasPtr<Das::Core::Utils::IDasJsonImpl>(
                Das::Utils::CloneYyjsonValue(context));
        }
        catch (const std::exception&)
        {
            reference_pass_failed_ = true;
            continue;
        }

        // 早段：开被引条目会话。
        Das::DasPtr<Das::PluginInterface::IDasTaskAuthoringSession> session;
        const auto session_result = creator->CreateAuthoringSession(
            resolved_task_guid,
            context_json.Get(),
            session.Put());
        if (DAS::IsFailed(session_result) || !session)
        {
            reference_pass_failed_ = true;
            CompileEdgeDiagnostic diagnostic;
            diagnostic.kind = CompileDiagnosticKind::ReferenceSessionFailed;
            diagnostic.node_id = node.node_id;
            diagnostic.message = DAS_FMT_NS::format(
                "CreateAuthoringSession failed for entry {} "
                "(task type {}): result={}",
                *bound_entry_id,
                task_type_guid,
                session_result);
            DAS_CORE_LOG_WARN("reference closure pass: {}", diagnostic.message);
            reference_diagnostics_.push_back(DiagnosticToJson(diagnostic));
            continue;
        }

        // 早段：GetDocument 现算节点口面（文档根 ports 自述——消费文档
        // 自述，禁第二份推导）。
        Das::DasPtr<Das::ExportInterface::IDasJson> document_json;
        const auto                                  document_result =
            session->GetDocument(nullptr, document_json.Put());
        auto document_value = JsonToYyjson(document_json.Get());
        if (DAS::IsFailed(document_result) || !document_value.has_value())
        {
            reference_pass_failed_ = true;
            CompileEdgeDiagnostic diagnostic;
            diagnostic.kind = CompileDiagnosticKind::ReferenceDocumentMissing;
            diagnostic.node_id = node.node_id;
            diagnostic.message = DAS_FMT_NS::format(
                "GetDocument failed for referenced entry {}: result={}",
                *bound_entry_id,
                document_result);
            DAS_CORE_LOG_WARN("reference closure pass: {}", diagnostic.message);
            reference_diagnostics_.push_back(DiagnosticToJson(diagnostic));
            continue;
        }
        auto faces = ParseDocumentRootPorts(*document_value);
        if (!faces.has_value())
        {
            reference_pass_failed_ = true;
            CompileEdgeDiagnostic diagnostic;
            diagnostic.kind = CompileDiagnosticKind::ReferenceDocumentMissing;
            diagnostic.node_id = node.node_id;
            diagnostic.message = DAS_FMT_NS::format(
                "referenced entry {} document carries no root ports "
                "section",
                *bound_entry_id);
            DAS_CORE_LOG_WARN("reference closure pass: {}", diagnostic.message);
            reference_diagnostics_.push_back(DiagnosticToJson(diagnostic));
            continue;
        }
        reference_faces_.emplace(node.node_id, std::move(*faces));

        // 晚段：同一会话 Compile → 产物直写广播绑定（compile 段
        // executionInput 原文；产物永不落盘、不写仓库）。
        Das::DasPtr<Das::ExportInterface::IDasJson> compile_json;
        const auto                                  compile_result =
            session->Compile(nullptr, compile_json.Put());
        auto compile_value = JsonToYyjson(compile_json.Get());
        if (DAS::IsFailed(compile_result) || !compile_value.has_value())
        {
            reference_pass_failed_ = true;
            CompileEdgeDiagnostic diagnostic;
            diagnostic.kind = CompileDiagnosticKind::ReferenceCompileFailed;
            diagnostic.node_id = node.node_id;
            diagnostic.message = DAS_FMT_NS::format(
                "Compile failed for referenced entry {}: result={}",
                *bound_entry_id,
                compile_result);
            DAS_CORE_LOG_WARN("reference closure pass: {}", diagnostic.message);
            reference_diagnostics_.push_back(DiagnosticToJson(diagnostic));
            continue;
        }
        auto artifact_text = ParseCompileExecutionInput(*compile_value);
        if (!artifact_text.has_value())
        {
            reference_pass_failed_ = true;
            CompileEdgeDiagnostic diagnostic;
            diagnostic.kind = CompileDiagnosticKind::ReferenceCompileFailed;
            diagnostic.node_id = node.node_id;
            diagnostic.message = DAS_FMT_NS::format(
                "compile result of referenced entry {} carries no "
                "executionInput segment",
                *bound_entry_id);
            DAS_CORE_LOG_WARN("reference closure pass: {}", diagnostic.message);
            reference_diagnostics_.push_back(DiagnosticToJson(diagnostic));
            continue;
        }
        reference_artifacts_.emplace_back(
            node.node_id,
            std::move(*artifact_text));
    }
}

// ---------------------------------------------------------------------------
// ResolveDeclaredPorts / ResolveNodePorts
// ---------------------------------------------------------------------------

GraphCompiler::ManifestPorts GraphCompiler::ResolveDeclaredPorts(
    const std::string&                              component_guid,
    std::unordered_map<std::string, ManifestPorts>& cache) const
{
    auto cached = cache.find(component_guid);
    if (cached != cache.end())
    {
        return cached->second;
    }

    ManifestPorts ports;
    if (factory_manager_)
    {
        ports = ReadManifest(component_guid);
    }
    if (!ports.was_resolved && port_surface_provider_)
    {
        auto surface = port_surface_provider_(component_guid);
        if (surface.has_value())
        {
            ports.inputs.clear();
            ports.outputs.clear();
            for (const auto& port : surface->inputs)
            {
                ports.inputs.push_back(
                    {port.port_id, port.port_type, port.loop_carried});
            }
            for (const auto& port : surface->outputs)
            {
                ports.outputs.push_back(
                    {port.port_id, port.port_type, port.loop_carried});
            }
            ports.was_resolved = true;
        }
    }

    cache[component_guid] = ports;
    return ports;
}

GraphCompiler::ManifestPorts GraphCompiler::ResolveNodePorts(
    const Dto::GraphNodeDto&                        node,
    std::unordered_map<std::string, ManifestPorts>& cache) const
{
    if (node.target.target_kind != "componentRef"
        || !node.target.component_ref.has_value())
    {
        return {};
    }

    // The guid-keyed cache holds the declared-only surface (faces are
    // per-node — caching the merged result would leak node A's faces onto
    // node B); the merge below mutates only this node's local copy.
    ManifestPorts ports =
        ResolveDeclaredPorts(node.target.component_ref->component_guid, cache);
    // begin/end 文档口面按口对并入（双侧同 id——入侧收调用方图输入、出侧
    // 供图内连线，角色镜像侧由推导层 D-14 排除）；其余节点按 face 方向单
    // 侧并入。
    const bool node_is_boundary =
        node.target.component_ref->component_guid == GRAPH_BEGIN_COMPONENT_GUID
        || node.target.component_ref->component_guid
               == GRAPH_END_COMPONENT_GUID;
    if (node_faces_ != nullptr)
    {
        const auto found = node_faces_->find(node.node_id);
        if (found != node_faces_->end() && !found->second.empty())
        {
            MergeFacesOntoSurface(
                ports,
                node.node_id,
                found->second,
                node_is_boundary);
        }
    }
    // 绑条目调用节点口面：注入 pass 已供面的节点由 pass 接管（文档根
    // ports 现算，新链）；未接管者并入 entryFaces[该 entryId]（去重
    // 规则同上，face 胜出+WARN）——签名物化存储副本（112-06 通道）的
    // 替代数据源，存储退役前的并行通道（后续删除）。
    const auto pass_face = reference_faces_.find(node.node_id);
    if (pass_face != reference_faces_.end())
    {
        if (!pass_face->second.empty())
        {
            MergeFacesOntoSurface(ports, node.node_id, pass_face->second);
        }
        return ports;
    }
    const auto bound_entry_id = ResolveBoundEntryId(node);
    if (bound_entry_id.has_value() && entry_faces_ != nullptr)
    {
        const auto found = entry_faces_->find(*bound_entry_id);
        if (found != entry_faces_->end() && !found->second.empty())
        {
            MergeFacesOntoSurface(ports, node.node_id, found->second);
        }
    }
    return ports;
}

// ---------------------------------------------------------------------------
// ReadManifest
// ---------------------------------------------------------------------------

GraphCompiler::ManifestPorts GraphCompiler::ReadManifest(
    const std::string& component_guid) const
{
    ManifestPorts result;

    if (!factory_manager_)
    {
        DAS_CORE_LOG_WARN("Factory manager not set");
        return result;
    }

    DasGuid target_guid;
    try
    {
        target_guid =
            Das::Core::ForeignInterfaceHost::MakeDasGuid(component_guid);
    }
    catch (const std::exception&)
    {
        DAS_CORE_LOG_WARN("Invalid GUID string: {}", component_guid);
        return result;
    }

    auto definitions = factory_manager_->EnumerateDefinitions();

    for (const auto& def : definitions)
    {
        if (def.component_guid == target_guid)
        {
            result.inputs = PortsFromDefinitionList(def.definition, "inputs");
            result.outputs = PortsFromDefinitionList(def.definition, "outputs");
            result.was_resolved = true;
            return result;
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// PortsFromDefinitionList
// ---------------------------------------------------------------------------

std::vector<GraphCompiler::PortEntry> GraphCompiler::PortsFromDefinitionList(
    const yyjson::value& definition,
    const std::string&   list_key)
{
    std::vector<PortEntry> ports;

    auto def_obj = definition.as_object();
    if (!def_obj.has_value())
    {
        return ports;
    }

    if (!def_obj->contains(std::string_view(list_key)))
    {
        return ports;
    }

    auto list_val = (*def_obj)[std::string_view(list_key)];
    if (list_val.is_null())
    {
        return ports;
    }

    auto arr = list_val.as_array();
    if (!arr.has_value())
    {
        return ports;
    }

    for (const auto& entry : *arr)
    {
        auto entry_obj = entry.as_object();
        if (!entry_obj.has_value())
        {
            DAS_CORE_LOG_WARN(
                "Non-object entry in {} array, skipping",
                list_key.c_str());
            continue;
        }

        if (!entry_obj->contains("id") || !entry_obj->contains("type"))
        {
            DAS_CORE_LOG_WARN(
                "Missing id or type in {} entry, skipping",
                list_key.c_str());
            continue;
        }

        auto id_val = (*entry_obj)[std::string_view("id")];
        auto type_val = (*entry_obj)[std::string_view("type")];

        if (id_val.is_null() || type_val.is_null())
        {
            DAS_CORE_LOG_WARN(
                "Null id or type in {} entry, skipping",
                list_key.c_str());
            continue;
        }

        PortEntry port;

        auto id_str = id_val.as_string();
        if (!id_str.has_value())
        {
            DAS_CORE_LOG_WARN(
                "Port id is not a string in {} entry, skipping",
                list_key.c_str());
            continue;
        }
        port.port_id = std::string(*id_str);

        auto type_str = type_val.as_string();
        if (!type_str.has_value())
        {
            DAS_CORE_LOG_WARN(
                "Port type is not a string in {} entry, skipping",
                list_key.c_str());
            continue;
        }
        port.port_type = std::string(*type_str);

        // loopCarried 声明 engaged-only 提取：仅布尔真值置位，缺失/非布
        // 尔一律视为普通口（与 required 的 miss 容忍语义一致）。
        if (entry_obj->contains(std::string_view("loopCarried")))
        {
            auto carried =
                (*entry_obj)[std::string_view("loopCarried")].as_bool();
            if (carried.has_value() && *carried)
            {
                port.loop_carried = true;
            }
        }

        ports.push_back(std::move(port));
    }

    return ports;
}

// ---------------------------------------------------------------------------
// IsTypeCompatible
// ---------------------------------------------------------------------------

bool GraphCompiler::IsTypeCompatible(
    const std::string& source_type,
    const std::string& target_type)
{
    // Same type — always compatible
    if (source_type == target_type)
    {
        return true;
    }

    // Number equivalence classes: the integer family {int, int64} and the
    // float family {float, double} interconnect freely inside a family and
    // integer widens to float (loss-free). double -> float is allowed as a
    // family-internal link because representation precision narrowing is
    // not a semantic truncation — the {int, int64, double} ruling extends
    // consistently over the pre-existing float word; the hard line is
    // float -> integer, where precision truncation is never done silently.
    // Mirrors the UI-side GraphConnectionRules equivalence table
    // (DasNativeUI/Services/GraphConnectionRules.cs CanConnect) — the two
    // sides are one table: changing one side requires changing the other,
    // and the matrix tests pin the Core half so drift goes red.
    const auto in_integer_family = [](const std::string& type)
    { return type == "int" || type == "int64"; };
    const auto in_float_family = [](const std::string& type)
    { return type == "float" || type == "double"; };
    if (in_integer_family(source_type) && in_integer_family(target_type))
    {
        return true;
    }
    if (in_float_family(source_type) && in_float_family(target_type))
    {
        return true;
    }
    if (in_integer_family(source_type) && in_float_family(target_type))
    {
        return true;
    }

    // Unknown types stay permissive here BY DESIGN: Core carries custom
    // plugin-declared port types its vocabulary cannot enumerate, so it
    // warns (UnknownPortType) instead of rejecting, while the UI side has
    // no manifest context and must fail closed (GraphConnectionRules).
    // Equivalence classes must never ride on this permissive path — they
    // are explicit rules over known words only.
    const auto& known = GetKnownTypes();
    if (!known.contains(source_type) || !known.contains(target_type))
    {
        return true;
    }

    // All other known-type mismatches are incompatible
    return false;
}

// ---------------------------------------------------------------------------
// ValidateEdgePorts
// ---------------------------------------------------------------------------

std::vector<CompileEdgeDiagnostic> GraphCompiler::ValidateEdgePorts(
    const Dto::GraphDocumentDto&    document,
    std::set<SignalEdgeCoordinate>* canonical_signal_edges)
{
    std::vector<CompileEdgeDiagnostic> diagnostics;

    // 边校验的口面 = 声明面 ∪ 投影面 ∪ faces 注入面：工厂 manifest、会话
    // 注入的 catalog 端口面、nodeFaces/entryFaces 文档口面快照任一可用
    // 即可出诊断（authoring 会话编译路径只绑定 provider，faces-only 文档
    // 只绑定 faces，提前返回会静默吞掉全部边诊断）。
    const bool faces_channel_bound =
        (node_faces_ != nullptr && !node_faces_->empty())
        || (entry_faces_ != nullptr && !entry_faces_->empty());
    if (!factory_manager_ && !port_surface_provider_ && !faces_channel_bound)
    {
        DAS_CORE_LOG_WARN("Factory manager not set");
        return diagnostics;
    }

    // Build node index for O(1) lookup by node_id
    std::unordered_map<std::string, const Dto::GraphNodeDto*> node_index;
    for (const auto& node : document.nodes)
    {
        node_index[node.node_id] = &node;
    }

    // Cache manifest lookups to avoid redundant EnumerateDefinitions calls
    std::unordered_map<std::string, ManifestPorts> manifest_cache;

    for (const auto& edge : document.edges)
    {
        // --- Node existence checks ---
        auto src_it = node_index.find(edge.source_node_id);
        if (src_it == node_index.end())
        {
            CompileEdgeDiagnostic d;
            d.kind = CompileDiagnosticKind::NodeNotFound;
            d.node_id = edge.source_node_id;
            d.edge_id = edge.edge_id;
            d.message = DAS_FMT_NS::format(
                "Source node '{}' not found in graph",
                edge.source_node_id);
            diagnostics.push_back(std::move(d));
            continue; // Can't validate ports without the node
        }

        auto tgt_it = node_index.find(edge.target_node_id);
        if (tgt_it == node_index.end())
        {
            CompileEdgeDiagnostic d;
            d.kind = CompileDiagnosticKind::NodeNotFound;
            d.node_id = edge.target_node_id;
            d.edge_id = edge.edge_id;
            d.message = DAS_FMT_NS::format(
                "Target node '{}' not found in graph",
                edge.target_node_id);
            diagnostics.push_back(std::move(d));
            continue;
        }

        const auto& src_node = *src_it->second;
        const auto& tgt_node = *tgt_it->second;

        // --- Source manifest resolution ---
        ManifestPorts src_manifest;
        if (src_node.target.target_kind == "componentRef")
        {
            if (!src_node.target.component_ref.has_value())
            {
                CompileEdgeDiagnostic d;
                d.kind = CompileDiagnosticKind::UnresolvableComponentGuid;
                d.node_id = edge.source_node_id;
                d.edge_id = edge.edge_id;
                d.message = DAS_FMT_NS::format(
                    "Source node '{}' has componentRef but missing "
                    "component_ref field",
                    edge.source_node_id);
                diagnostics.push_back(std::move(d));
                continue;
            }

            src_manifest = ResolveNodePorts(src_node, manifest_cache);

            // Check if manifest was resolved.  A component may legitimately
            // declare zero ports, so we use an explicit flag instead of
            // relying on empty port vectors.
            if (!src_manifest.was_resolved)
            {
                CompileEdgeDiagnostic d;
                d.kind = CompileDiagnosticKind::UnresolvableComponentGuid;
                d.node_id = edge.source_node_id;
                d.edge_id = edge.edge_id;
                d.message = DAS_FMT_NS::format(
                    "Cannot resolve manifest for source node '{}' "
                    "component_guid='{}'",
                    edge.source_node_id,
                    src_node.target.component_ref->component_guid);
                diagnostics.push_back(std::move(d));
                continue;
            }
        }

        // --- Target manifest resolution ---
        ManifestPorts tgt_manifest;
        if (tgt_node.target.target_kind == "componentRef")
        {
            if (!tgt_node.target.component_ref.has_value())
            {
                CompileEdgeDiagnostic d;
                d.kind = CompileDiagnosticKind::UnresolvableComponentGuid;
                d.node_id = edge.target_node_id;
                d.edge_id = edge.edge_id;
                d.message = DAS_FMT_NS::format(
                    "Target node '{}' has componentRef but missing "
                    "component_ref field",
                    edge.target_node_id);
                diagnostics.push_back(std::move(d));
                continue;
            }

            tgt_manifest = ResolveNodePorts(tgt_node, manifest_cache);

            if (!tgt_manifest.was_resolved)
            {
                CompileEdgeDiagnostic d;
                d.kind = CompileDiagnosticKind::UnresolvableComponentGuid;
                d.node_id = edge.target_node_id;
                d.edge_id = edge.edge_id;
                d.message = DAS_FMT_NS::format(
                    "Cannot resolve manifest for target node '{}' "
                    "component_guid='{}'",
                    edge.target_node_id,
                    tgt_node.target.component_ref->component_guid);
                diagnostics.push_back(std::move(d));
                continue;
            }
        }

        // --- Source port existence check ---
        const PortEntry* src_port = nullptr;
        for (const auto& p : src_manifest.outputs)
        {
            if (p.port_id == edge.source_port_id)
            {
                src_port = &p;
                break;
            }
        }

        // Canonical implicit sequencing endpoint: a node-level signal wire
        // leaves on the canonical "signal_out" without any manifest
        // declaration (the canonical port names are the wire-layer
        // coordinate system — per-component signal port declarations were
        // explicitly rejected). Only UNDECLARED canonical endpoints are
        // exempt; declared signal ports validate normally.
        const bool src_canonical_implicit =
            edge.edge_type == "signal"
            && edge.source_port_id
                   == std::string(FormSequenceProjector::SignalOutPort)
            && src_port == nullptr;

        if (!src_port && !src_canonical_implicit)
        {
            CompileEdgeDiagnostic d;
            d.kind = CompileDiagnosticKind::PortNotFound;
            d.node_id = edge.source_node_id;
            d.port_id = edge.source_port_id;
            d.edge_id = edge.edge_id;
            d.direction = CompileEdgeDiagnostic::PortDirection::Output;
            d.message = DAS_FMT_NS::format(
                "Port '{}' not found in node '{}' manifest outputs",
                edge.source_port_id,
                edge.source_node_id);
            diagnostics.push_back(std::move(d));
            continue;
        }

        // 规范口 signal 源边收集：signal 边从规范 signal_out 离开且该口
        // 未声明（隐式豁免形态）或声明为 signal 类型（launch 形态）——两
        // 种形态的成功发射都是运行时兜底的对象，具名口（continue/break/
        // true/false/next）不在其列、永不代发。
        if (canonical_signal_edges != nullptr && edge.edge_type == "signal"
            && edge.source_port_id
                   == std::string(FormSequenceProjector::SignalOutPort)
            && (src_port == nullptr || src_port->port_type == "signal"))
        {
            canonical_signal_edges->insert(
                SignalEdgeCoordinate{
                    edge.source_node_id,
                    edge.source_port_id,
                    edge.target_node_id,
                    edge.target_port_id});
        }

        // --- Target port existence check ---
        const PortEntry* tgt_port = nullptr;
        for (const auto& p : tgt_manifest.inputs)
        {
            if (p.port_id == edge.target_port_id)
            {
                tgt_port = &p;
                break;
            }
        }

        // Target-side mirror of the canonical sequencing exemption: the
        // implicit "enter node" landing point is the canonical "signal_in".
        const bool tgt_canonical_implicit =
            edge.edge_type == "signal"
            && edge.target_port_id
                   == std::string(FormSequenceProjector::SignalInPort)
            && tgt_port == nullptr;

        if (!tgt_port && !tgt_canonical_implicit)
        {
            CompileEdgeDiagnostic d;
            d.kind = CompileDiagnosticKind::PortNotFound;
            d.node_id = edge.target_node_id;
            d.port_id = edge.target_port_id;
            d.edge_id = edge.edge_id;
            d.direction = CompileEdgeDiagnostic::PortDirection::Input;
            d.message = DAS_FMT_NS::format(
                "Port '{}' not found in node '{}' manifest inputs",
                edge.target_port_id,
                edge.target_node_id);
            diagnostics.push_back(std::move(d));
            continue;
        }

        // Implicit canonical endpoints carry no declared type — type checks
        // run only when both endpoints resolved against a manifest.
        if (src_port != nullptr && tgt_port != nullptr)
        {
            // --- Unknown type warning ---
            const auto& known = GetKnownTypes();
            if (!known.contains(src_port->port_type)
                || !known.contains(tgt_port->port_type))
            {
                CompileEdgeDiagnostic d;
                d.kind = CompileDiagnosticKind::UnknownPortType;
                d.node_id = edge.source_node_id;
                d.port_id = edge.source_port_id;
                d.edge_id = edge.edge_id;
                d.direction = CompileEdgeDiagnostic::PortDirection::Output;
                d.message = DAS_FMT_NS::format(
                    "Unknown port type: source='{}' target='{}'",
                    src_port->port_type,
                    tgt_port->port_type);
                d.actual_type = src_port->port_type;
                d.expected_type = tgt_port->port_type;
                diagnostics.push_back(std::move(d));
                // Unknown types are warnings — don't block, but continue to
                // type check
            }

            // --- Type compatibility check ---
            if (!IsTypeCompatible(src_port->port_type, tgt_port->port_type))
            {
                CompileEdgeDiagnostic d;
                d.kind = CompileDiagnosticKind::TypeMismatch;
                d.node_id = edge.target_node_id;
                d.port_id = edge.target_port_id;
                d.edge_id = edge.edge_id;
                d.direction = CompileEdgeDiagnostic::PortDirection::Input;
                d.expected_type = tgt_port->port_type;
                d.actual_type = src_port->port_type;
                d.message = DAS_FMT_NS::format(
                    "Type mismatch on edge '{}': source port '{}' has type "
                    "'{}', target port '{}' expects type '{}'",
                    edge.edge_id,
                    edge.source_port_id,
                    src_port->port_type,
                    edge.target_port_id,
                    tgt_port->port_type);
                diagnostics.push_back(std::move(d));
            }
        }
    }

    return diagnostics;
}

// ---------------------------------------------------------------------------
// Stored-edge stale review (compile-time auto-disconnect)
// ---------------------------------------------------------------------------

bool GraphCompiler::NodeCarriesInjectedFaces(
    const Dto::GraphNodeDto& node) const
{
    if (node_faces_ != nullptr
        && node_faces_->find(node.node_id) != node_faces_->end())
    {
        return true;
    }
    const auto bound_entry_id = ResolveBoundEntryId(node);
    return bound_entry_id.has_value() && entry_faces_ != nullptr
           && entry_faces_->find(*bound_entry_id) != entry_faces_->end();
}

bool GraphCompiler::IsTypeCompatibleWithRegistry(
    const std::string& source_type,
    const std::string& target_type) const
{
    if (source_type == target_type)
    {
        return true;
    }
    // 注册类型闭世界判定（文档 types[] 注册表——图视图根 types section
    // 的同一数据源）：两侧均可判（内建九词，或经 BindFaceTypes 注册的
    // 自定义类型）时按声明链接判定——任一侧 compatibleWith 列出对方即
    // 兼容，未声明的可判配对不兼容。判不了的一侧（未注册且非内建——
    // Core 无法枚举插件类型）回落 IsTypeCompatible 的 permissive 语义，
    // 复核不误伤。内建九词撞名的注册条目被内建遮蔽（投影侧「内建胜
    // 出」的镜像）。
    const auto& known = GetKnownTypes();
    const auto  find_registered =
        [this](const std::string& name) -> const DocumentFaceType*
    {
        if (face_types_ == nullptr)
        {
            return nullptr;
        }
        for (const auto& type : *face_types_)
        {
            if (type.name == name)
            {
                return &type;
            }
        }
        return nullptr;
    };
    const bool source_builtin = known.contains(source_type);
    const bool target_builtin = known.contains(target_type);
    // 内建×内建：数值等价族等既有表义直接走 IsTypeCompatible 单一权威
    // （注册表闭世界不覆盖内建对的判定）。
    if (source_builtin && target_builtin)
    {
        return IsTypeCompatible(source_type, target_type);
    }
    const DocumentFaceType* source_registered =
        source_builtin ? nullptr : find_registered(source_type);
    const DocumentFaceType* target_registered =
        target_builtin ? nullptr : find_registered(target_type);
    const bool source_judgeable =
        source_builtin || source_registered != nullptr;
    const bool target_judgeable =
        target_builtin || target_registered != nullptr;
    if (!source_judgeable || !target_judgeable)
    {
        return IsTypeCompatible(source_type, target_type);
    }
    const auto declares_link =
        [](const DocumentFaceType* entry, const std::string& other)
    {
        return entry != nullptr
               && std::find(
                      entry->compatible_with.begin(),
                      entry->compatible_with.end(),
                      other)
                      != entry->compatible_with.end();
    };
    return declares_link(source_registered, target_type)
           || declares_link(target_registered, source_type);
}

GraphCompiler::StaleEdgeReview GraphCompiler::ReviewStaleEdges(
    const Dto::GraphDocumentDto& document) const
{
    StaleEdgeReview review;

    auto node_index = BuildNodeIndex(document);
    // 口面解析缓存（guid 键，声明面复用；faces 合并在 per-node 本地副本）。
    std::unordered_map<std::string, ManifestPorts> manifest_cache;

    for (const auto& edge : document.edges)
    {
        // 复核域=数据边：signal 边走规范时序坐标系（signal_out/signal_in
        // 隐式端点+声明执行口），其存亡不随数据口面演化（111.1 现仓边域
        // 实读定界——数据口复核不触 signal 边）。
        if (edge.edge_type == "signal")
        {
            continue;
        }
        auto src_it = node_index.find(edge.source_node_id);
        auto tgt_it = node_index.find(edge.target_node_id);
        if (src_it == node_index.end() || tgt_it == node_index.end())
        {
            // 缺节点边是 ValidateEdgePorts 的 NodeNotFound 诊断域（图文档
            // 删节点在 apply 期已有级联），复核不重复处理。
            continue;
        }
        const auto& src_node = *src_it->second;
        const auto& tgt_node = *tgt_it->second;
        if (src_node.target.target_kind != "componentRef"
            || !src_node.target.component_ref.has_value()
            || tgt_node.target.target_kind != "componentRef"
            || !tgt_node.target.component_ref.has_value())
        {
            continue;
        }
        const ManifestPorts src_ports =
            ResolveNodePorts(src_node, manifest_cache);
        const ManifestPorts tgt_ports =
            ResolveNodePorts(tgt_node, manifest_cache);
        // 口面未解析（无 manifest/catalog/faces 任一来源）= Unresolvable
        // ComponentGuid 诊断域——组件面未知时判不了口存亡，复核跳过不
        // 误删。
        if (!src_ports.was_resolved || !tgt_ports.was_resolved)
        {
            continue;
        }
        const PortEntry* src_port = nullptr;
        for (const auto& p : src_ports.outputs)
        {
            if (p.port_id == edge.source_port_id)
            {
                src_port = &p;
                break;
            }
        }
        const PortEntry* tgt_port = nullptr;
        for (const auto& p : tgt_ports.inputs)
        {
            if (p.port_id == edge.target_port_id)
            {
                tgt_port = &p;
                break;
            }
        }

        StaleEdgeRecord record;
        record.edge_id = edge.edge_id;
        record.source_node_id = edge.source_node_id;
        record.source_port_id = edge.source_port_id;
        record.target_node_id = edge.target_node_id;
        record.target_port_id = edge.target_port_id;

        // 复核资格按原因侧锚定（「口面演化后复核存量边」的精确域）：
        // 口消失的原因侧节点必须携带注入 faces（演化证据）；类型不配须
        // 至少一侧携带。faces 条目缺席=编译宿主未跑预备 pass，不是口面
        // 演化为空——缺席侧判不了演化，跳过防误删。
        bool stale = false;
        if (src_port == nullptr)
        {
            record.reason = StaleEdgeRecord::Reason::SourcePortMissing;
            stale = NodeCarriesInjectedFaces(src_node);
        }
        else if (tgt_port == nullptr)
        {
            record.reason = StaleEdgeRecord::Reason::TargetPortMissing;
            stale = NodeCarriesInjectedFaces(tgt_node);
        }
        else if (!IsTypeCompatibleWithRegistry(
                     src_port->port_type,
                     tgt_port->port_type))
        {
            record.reason = StaleEdgeRecord::Reason::TypeIncompatible;
            record.source_type = src_port->port_type;
            record.target_type = tgt_port->port_type;
            stale = NodeCarriesInjectedFaces(src_node)
                    || NodeCarriesInjectedFaces(tgt_node);
        }
        if (!stale)
        {
            continue;
        }

        CompileEdgeDiagnostic diagnostic;
        diagnostic.kind = CompileDiagnosticKind::StaleEdgeDisconnect;
        diagnostic.edge_id = edge.edge_id;
        diagnostic.expected_type = record.target_type;
        diagnostic.actual_type = record.source_type;
        switch (record.reason)
        {
        case StaleEdgeRecord::Reason::SourcePortMissing:
            diagnostic.node_id = edge.source_node_id;
            diagnostic.port_id = edge.source_port_id;
            diagnostic.direction = CompileEdgeDiagnostic::PortDirection::Output;
            diagnostic.message = DAS_FMT_NS::format(
                "stored edge '{}' disconnects at compile review: source "
                "port '{}.{}' no longer exists on the node's current port "
                "face",
                edge.edge_id,
                edge.source_node_id,
                edge.source_port_id);
            DAS_CORE_LOG_WARN(
                "stored edge {} disconnects at compile review: source port "
                "{}.{} no longer exists on the node's current port face",
                edge.edge_id,
                edge.source_node_id,
                edge.source_port_id);
            break;
        case StaleEdgeRecord::Reason::TargetPortMissing:
            diagnostic.node_id = edge.target_node_id;
            diagnostic.port_id = edge.target_port_id;
            diagnostic.direction = CompileEdgeDiagnostic::PortDirection::Input;
            diagnostic.message = DAS_FMT_NS::format(
                "stored edge '{}' disconnects at compile review: target "
                "port '{}.{}' no longer exists on the node's current port "
                "face",
                edge.edge_id,
                edge.target_node_id,
                edge.target_port_id);
            DAS_CORE_LOG_WARN(
                "stored edge {} disconnects at compile review: target port "
                "{}.{} no longer exists on the node's current port face",
                edge.edge_id,
                edge.target_node_id,
                edge.target_port_id);
            break;
        case StaleEdgeRecord::Reason::TypeIncompatible:
            diagnostic.node_id = edge.target_node_id;
            diagnostic.port_id = edge.target_port_id;
            diagnostic.direction = CompileEdgeDiagnostic::PortDirection::Input;
            diagnostic.message = DAS_FMT_NS::format(
                "stored edge '{}' disconnects at compile review: source port "
                "'{}.{}' type '{}' is incompatible with target port '{}.{}' "
                "type '{}'",
                edge.edge_id,
                edge.source_node_id,
                edge.source_port_id,
                record.source_type,
                edge.target_node_id,
                edge.target_port_id,
                record.target_type);
            DAS_CORE_LOG_WARN(
                "stored edge {} disconnects at compile review: source port "
                "{}.{} type {} is incompatible with target port {}.{} type "
                "{}",
                edge.edge_id,
                edge.source_node_id,
                edge.source_port_id,
                record.source_type,
                edge.target_node_id,
                edge.target_port_id,
                record.target_type);
            break;
        }
        review.diagnostics.push_back(std::move(diagnostic));
        review.stale_edges.push_back(std::move(record));
    }
    return review;
}

// ---------------------------------------------------------------------------
// BuildNodeIndex
// ---------------------------------------------------------------------------

std::unordered_map<std::string, const Dto::GraphNodeDto*>
GraphCompiler::BuildNodeIndex(const Dto::GraphDocumentDto& document)
{
    std::unordered_map<std::string, const Dto::GraphNodeDto*> index;
    for (const auto& node : document.nodes)
    {
        index[node.node_id] = &node;
    }
    return index;
}

// ---------------------------------------------------------------------------
// ComputeExecutionOrder (signal-aware, deterministic Kahn)
// ---------------------------------------------------------------------------

std::vector<std::string> GraphCompiler::ComputeExecutionOrder(
    const Dto::GraphDocumentDto& document)
{
    return ComputeTopology(document).execution_order;
}

// ---------------------------------------------------------------------------
// ComputeTopology — DFS back-edge classification + deterministic Kahn
// ---------------------------------------------------------------------------

GraphCompiler::TopologyResult GraphCompiler::ComputeTopology(
    const Dto::GraphDocumentDto& document)
{
    TopologyResult result;

    if (document.nodes.empty())
    {
        return result;
    }

    // Stable per-node index → deterministic Kahn tie-breaking (signals the
    // unordered_map nondeterminism called out in DAS-60 Stage 2).
    std::unordered_map<std::string, std::size_t> node_index;
    for (std::size_t i = 0; i < document.nodes.size(); ++i)
    {
        node_index[document.nodes[i].node_id] = i;
    }

    // Adjacency in document edge order (deterministic DFS traversal).
    std::unordered_map<std::string, std::vector<const Dto::GraphEdgeDto*>>
        adjacency;
    for (const auto& edge : document.edges)
    {
        adjacency[edge.source_node_id].push_back(&edge);
    }

    // DFS back-edge classification. An edge to a node still on the recursion
    // stack (an ancestor) is a back-edge. Removing every DFS back-edge always
    // yields a DAG, so the Kahn run below is guaranteed to order every node.
    enum class Color
    {
        White,
        Gray,
        Black
    };
    std::unordered_map<std::string, Color> color;
    for (const auto& node : document.nodes)
    {
        color[node.node_id] = Color::White;
    }

    std::unordered_set<std::string>       back_edge_ids;
    std::vector<const Dto::GraphEdgeDto*> back_edges_raw;

    std::function<void(const std::string&)> dfs = [&](const std::string& u)
    {
        color[u] = Color::Gray;

        auto it = adjacency.find(u);
        if (it != adjacency.end())
        {
            for (const Dto::GraphEdgeDto* edge : it->second)
            {
                const std::string& v = edge->target_node_id;
                auto               cit = color.find(v);
                if (cit == color.end())
                {
                    // Edge to a node outside the graph — ValidateEdgePorts
                    // reports it; topology ignores it.
                    continue;
                }

                if (cit->second == Color::Gray)
                {
                    // Back-edge: u -> ancestor v closes a cycle. Signal and
                    // data back-edges are recorded symmetrically — the
                    // former carries the iteration boundary, the latter is
                    // loop-carried state and no longer rejects the graph.
                    back_edge_ids.insert(edge->edge_id);
                    back_edges_raw.push_back(edge);
                }
                else if (cit->second == Color::White)
                {
                    dfs(v);
                }
                // Black: forward/cross edge — not a back-edge.
            }
        }

        color[u] = Color::Black;
    };

    for (const auto& node : document.nodes)
    {
        if (color[node.node_id] == Color::White)
        {
            dfs(node.node_id);
        }
    }

    // --- Tarjan SCC over the full edge set: the cycle layers ---------------
    // Layer membership comes from SCC + edge direction, not from the DFS
    // back-edge set: on nested/crossing graphs the two differ (an in-SCC
    // cross edge closes a cycle DFS never classified as a back-edge), and
    // the runtime consumes these layers as its iteration reset sets.
    std::unordered_map<std::string, int> tarjan_index;
    std::unordered_map<std::string, int> tarjan_lowlink;
    std::unordered_set<std::string>      on_tarjan_stack;
    std::vector<std::string>             tarjan_stack;
    int                                  next_tarjan_index = 0;
    std::vector<std::set<std::string>>   scc_sets;

    std::function<void(const std::string&)> strong_connect =
        [&](const std::string& v)
    {
        tarjan_index[v] = next_tarjan_index;
        tarjan_lowlink[v] = next_tarjan_index;
        ++next_tarjan_index;
        tarjan_stack.push_back(v);
        on_tarjan_stack.insert(v);

        auto adj_it = adjacency.find(v);
        if (adj_it != adjacency.end())
        {
            for (const Dto::GraphEdgeDto* edge : adj_it->second)
            {
                const std::string& w = edge->target_node_id;
                if (node_index.count(w) == 0)
                {
                    continue;
                }
                if (tarjan_index.find(w) == tarjan_index.end())
                {
                    strong_connect(w);
                    tarjan_lowlink[v] =
                        std::min(tarjan_lowlink[v], tarjan_lowlink[w]);
                }
                else if (on_tarjan_stack.count(w) > 0)
                {
                    tarjan_lowlink[v] =
                        std::min(tarjan_lowlink[v], tarjan_index[w]);
                }
            }
        }

        if (tarjan_lowlink[v] == tarjan_index[v])
        {
            std::set<std::string> scc;
            while (true)
            {
                const std::string w = tarjan_stack.back();
                tarjan_stack.pop_back();
                on_tarjan_stack.erase(w);
                scc.insert(w);
                if (w == v)
                {
                    break;
                }
            }
            scc_sets.push_back(std::move(scc));
        }
    };

    for (const auto& node : document.nodes)
    {
        if (tarjan_index.find(node.node_id) == tarjan_index.end())
        {
            strong_connect(node.node_id);
        }
    }

    // Only cyclic SCCs are loop layers: size > 1, or a single node with a
    // self-edge (a self-feeding data back edge closes a one-node cycle).
    std::unordered_set<std::string> self_loop_nodes;
    for (const auto& edge : document.edges)
    {
        if (edge.source_node_id == edge.target_node_id
            && node_index.count(edge.source_node_id) > 0)
        {
            self_loop_nodes.insert(edge.source_node_id);
        }
    }

    // Layer seeds. A cyclic SCC may hold several nested cycles: an inner loop
    // re-entered from an outer body collapses into one SCC over the full edge
    // set (everything is mutually reachable), so plain Tarjan cannot see the
    // nesting. Each signal back edge inside the SCC defines a natural loop
    // and back edges sharing a head merge into one layer. Membership is the
    // textbook two-half rule: nodes that reach a back-edge source without
    // passing the head, intersected with nodes the head reaches without
    // using any back edge. The second half keeps outer-level machinery (only
    // reachable through an outer back edge) out of inner loops, while bodies
    // chained by data edges stay in — the loop-carried flow may ride data
    // lines, not just signal lines.
    const auto edge_is_signal = [](const Dto::GraphEdgeDto* edge)
    { return edge->edge_type == "signal"; };

    std::unordered_map<std::string, std::vector<std::string>> all_preds;
    std::unordered_map<std::string, std::vector<const Dto::GraphEdgeDto*>>
        all_succ;
    for (const auto& edge : document.edges)
    {
        if (node_index.count(edge.source_node_id) == 0
            || node_index.count(edge.target_node_id) == 0)
        {
            continue;
        }
        all_preds[edge.target_node_id].push_back(edge.source_node_id);
        all_succ[edge.source_node_id].push_back(&edge);
    }

    // Layer under construction: natural-loop member set for tier/nesting,
    // expanded member set (natural loop ∪ producer closure) for the recorded
    // DTO.
    struct LayerDraft
    {
        std::set<std::string> scc_members;
        std::set<std::string> expanded_members;
        std::string           head_node_id;
        bool                  has_signal_back_edge = false;
    };
    std::vector<LayerDraft> layer_drafts;

    const auto natural_loop_of =
        [&](const std::set<std::string>&                 scc,
            const std::string&                           head,
            const std::vector<const Dto::GraphEdgeDto*>& sources)
    {
        std::unordered_set<std::string> reaches_source;
        std::vector<std::string>        backward_pending;
        for (const auto* edge : sources)
        {
            if (scc.count(edge->source_node_id) > 0
                && reaches_source.insert(edge->source_node_id).second)
            {
                backward_pending.push_back(edge->source_node_id);
            }
        }
        while (!backward_pending.empty())
        {
            const std::string node = backward_pending.back();
            backward_pending.pop_back();
            auto pred_it = all_preds.find(node);
            if (pred_it == all_preds.end())
            {
                continue;
            }
            for (const auto& pred : pred_it->second)
            {
                if (pred != head && scc.count(pred) > 0
                    && reaches_source.insert(pred).second)
                {
                    backward_pending.push_back(pred);
                }
            }
        }

        std::unordered_set<std::string> forward_from_head{head};
        std::vector<std::string>        forward_pending{head};
        while (!forward_pending.empty())
        {
            const std::string node = forward_pending.back();
            forward_pending.pop_back();
            auto succ_it = all_succ.find(node);
            if (succ_it == all_succ.end())
            {
                continue;
            }
            for (const auto* edge : succ_it->second)
            {
                if (back_edge_ids.count(edge->edge_id) > 0
                    || scc.count(edge->target_node_id) == 0
                    || !forward_from_head.insert(edge->target_node_id).second)
                {
                    continue;
                }
                forward_pending.push_back(edge->target_node_id);
            }
        }

        std::set<std::string> members{head};
        for (const auto& node : reaches_source)
        {
            if (forward_from_head.count(node) > 0)
            {
                members.insert(node);
            }
        }
        return members;
    };

    for (auto& scc : scc_sets)
    {
        const bool is_cycle =
            scc.size() > 1 || self_loop_nodes.count(*scc.begin()) > 0;
        if (!is_cycle)
        {
            continue;
        }

        // Group this SCC's signal back edges by their target (the head each
        // returns to); heads sorted by document index keep drafts
        // deterministic.
        std::vector<const Dto::GraphEdgeDto*> scc_signal_backs;
        for (const auto* edge : back_edges_raw)
        {
            if (edge_is_signal(edge) && scc.count(edge->source_node_id) > 0)
            {
                scc_signal_backs.push_back(edge);
            }
        }

        if (scc_signal_backs.empty())
        {
            // No signal circuit in this SCC: keep it whole so the carrying
            // check one level up can reject it with guidance.
            LayerDraft draft;
            draft.scc_members = scc;
            draft.expanded_members = scc;
            layer_drafts.push_back(std::move(draft));
            continue;
        }

        std::vector<std::string> heads;
        for (const auto* edge : scc_signal_backs)
        {
            if (std::find(heads.begin(), heads.end(), edge->target_node_id)
                == heads.end())
            {
                heads.push_back(edge->target_node_id);
            }
        }
        std::sort(
            heads.begin(),
            heads.end(),
            [&](const std::string& a, const std::string& b)
            { return node_index[a] < node_index[b]; });
        for (const auto& head : heads)
        {
            std::vector<const Dto::GraphEdgeDto*> sources;
            for (const auto* edge : scc_signal_backs)
            {
                if (edge->target_node_id == head)
                {
                    sources.push_back(edge);
                }
            }
            LayerDraft draft;
            draft.scc_members = natural_loop_of(scc, head, sources);
            draft.expanded_members = draft.scc_members;
            draft.head_node_id = head;
            draft.has_signal_back_edge = true;
            layer_drafts.push_back(std::move(draft));
        }
    }

    // Whole-SCC drafts (no signal back edge) fall back to their first member
    // in document order as head; split drafts already carry their back-edge
    // target (the carrying check one layer up in Compile() rejects the
    // former).
    for (std::size_t i = 0; i < layer_drafts.size(); ++i)
    {
        if (!layer_drafts[i].head_node_id.empty())
        {
            continue;
        }
        for (const auto& node : document.nodes)
        {
            if (layer_drafts[i].scc_members.count(node.node_id) > 0)
            {
                layer_drafts[i].head_node_id = node.node_id;
                break;
            }
        }
    }

    // tier = nesting depth: how many other layers' member sets contain this
    // layer's head (L' nested in L iff head(L') ∈ members(L), L' ≠ L).
    // Natural loops of one SCC nest; disjoint SCCs cannot contain each
    // other's heads, so isolated rings stay tier 0.
    std::vector<int> layer_tiers(layer_drafts.size(), 0);
    for (std::size_t i = 0; i < layer_drafts.size(); ++i)
    {
        for (std::size_t j = 0; j < layer_drafts.size(); ++j)
        {
            if (i != j
                && layer_drafts[j].scc_members.count(
                       layer_drafts[i].head_node_id)
                       > 0)
            {
                ++layer_tiers[i];
            }
        }
    }

    // Producer closure: a data producer with no incoming signal edge has no
    // execution timing of its own — its evaluation follows its consumers.
    // A consumer inside a loop pulls the producer into that (innermost)
    // layer so it re-evaluates every iteration; consumers all outside any
    // loop leave the producer out (evaluated once at startup).
    std::unordered_set<std::string> nodes_with_signal_in_edge;
    std::unordered_map<std::string, std::vector<std::string>> data_forward;
    for (const auto& edge : document.edges)
    {
        if (node_index.count(edge.source_node_id) == 0
            || node_index.count(edge.target_node_id) == 0)
        {
            continue;
        }
        if (edge.edge_type == "signal")
        {
            nodes_with_signal_in_edge.insert(edge.target_node_id);
        }
        else
        {
            data_forward[edge.source_node_id].push_back(edge.target_node_id);
        }
    }

    // Nested layers overlap (an inner layer's members belong to the outer
    // layer too), so membership is multi-valued; every consumer-side lookup
    // then picks the innermost candidate.
    std::unordered_map<std::string, std::vector<std::size_t>> layers_of_member;
    for (std::size_t i = 0; i < layer_drafts.size(); ++i)
    {
        for (const auto& member : layer_drafts[i].expanded_members)
        {
            layers_of_member[member].push_back(i);
        }
    }

    // "Innermost" among candidate layers = highest tier; ties break on the
    // head's document index so closure joins are deterministic.
    const auto is_innermost_choice =
        [&](std::size_t candidate, std::size_t incumbent)
    {
        if (layer_tiers[candidate] != layer_tiers[incumbent])
        {
            return layer_tiers[candidate] > layer_tiers[incumbent];
        }
        return node_index[layer_drafts[candidate].head_node_id]
               < node_index[layer_drafts[incumbent].head_node_id];
    };
    const auto innermost_of = [&](const std::vector<std::size_t>& candidates)
    {
        std::size_t best = candidates.front();
        for (std::size_t k = 1; k < candidates.size(); ++k)
        {
            if (is_innermost_choice(candidates[k], best))
            {
                best = candidates[k];
            }
        }
        return best;
    };

    bool closure_grew = true;
    while (closure_grew)
    {
        closure_grew = false;
        for (const auto& node : document.nodes)
        {
            if (layers_of_member.count(node.node_id) > 0
                || nodes_with_signal_in_edge.count(node.node_id) > 0)
            {
                continue;
            }
            std::size_t best_layer = layer_drafts.size();
            for (const auto& consumer : data_forward[node.node_id])
            {
                auto found = layers_of_member.find(consumer);
                if (found == layers_of_member.end())
                {
                    continue;
                }
                for (const auto candidate : found->second)
                {
                    if (best_layer == layer_drafts.size()
                        || is_innermost_choice(candidate, best_layer))
                    {
                        best_layer = candidate;
                    }
                }
            }
            if (best_layer == layer_drafts.size())
            {
                continue;
            }
            layer_drafts[best_layer].expanded_members.insert(node.node_id);
            layers_of_member[node.node_id].push_back(best_layer);
            closure_grew = true;
        }
    }

    // Back-edge records: every loop-closing edge lands in the plan with its
    // kind and owning layer (head + loop id bridge the runtime's firing and
    // reset bookkeeping). A signal back edge belongs to the layer whose head
    // it returns to; a data back edge belongs to the innermost layer holding
    // its source, so nested loops keep their own loop-carried channels apart.
    std::unordered_map<std::string, std::size_t> layer_of_head;
    for (std::size_t i = 0; i < layer_drafts.size(); ++i)
    {
        layer_of_head[layer_drafts[i].head_node_id] = i;
    }
    std::vector<std::set<std::pair<std::string, std::string>>>
        layer_data_sources(layer_drafts.size());
    for (const auto* edge : back_edges_raw)
    {
        Dto::BackEdgeDto back;
        back.edge_id = edge->edge_id;
        back.source_node_id = edge->source_node_id;
        back.source_port_id = edge->source_port_id;
        back.target_node_id = edge->target_node_id;
        back.target_port_id = edge->target_port_id;
        back.edge_kind = edge_is_signal(edge) ? "signal" : "data";
        std::size_t owner = layer_drafts.size();
        if (edge_is_signal(edge))
        {
            auto head_it = layer_of_head.find(edge->target_node_id);
            if (head_it != layer_of_head.end()
                && layer_drafts[head_it->second].scc_members.count(
                       edge->source_node_id)
                       > 0)
            {
                owner = head_it->second;
            }
        }
        else
        {
            // 数据回边直连某层头（如内环节点把计数值带回外层 while 的条件
            // 口）携带的是该层自己的循环状态：按源最内层归属会把它记进
            // 内层，外层发火清严格内层源口时就把外层自己的携带值清掉了，
            // 外层头重启后读不到上圈回边值。目标头归属优先；层内通道维持
            // 按源最内层。
            auto head_it = layer_of_head.find(edge->target_node_id);
            if (head_it != layer_of_head.end()
                && layer_drafts[head_it->second].has_signal_back_edge)
            {
                owner = head_it->second;
            }
            else
            {
                auto member_it = layers_of_member.find(edge->source_node_id);
                if (member_it != layers_of_member.end()
                    && !member_it->second.empty())
                {
                    owner = innermost_of(member_it->second);
                }
            }
        }
        if (owner != layer_drafts.size())
        {
            back.loop_head_node_id = layer_drafts[owner].head_node_id;
            if (!edge_is_signal(edge))
            {
                layer_data_sources[owner].insert(
                    {edge->source_node_id, edge->source_port_id});
            }
        }
        else
        {
            // Unreachable for real cycles (a back edge's source is always
            // inside its SCC); the naive target keeps the record well-formed.
            back.loop_head_node_id = edge->target_node_id;
        }
        result.back_edges.push_back(std::move(back));
    }

    // Loop-layer records: members in document order, data back-edge source
    // sites deduplicated, ready for the runtime to consume as reset sets.
    // Carrying check rides the same pass: a layer with loop-carried data
    // edges but no signal circuit has no iteration boundary at all — a pure
    // data cycle never starts (no first-pass origin) and a half loop runs
    // once and silently stops; both are rejected with guidance instead.
    for (std::size_t i = 0; i < layer_drafts.size(); ++i)
    {
        bool has_data_back_edge = false;
        for (const auto* edge : back_edges_raw)
        {
            if (!edge_is_signal(edge)
                && layer_drafts[i].expanded_members.count(edge->source_node_id)
                       > 0)
            {
                has_data_back_edge = true;
                break;
            }
        }
        if (has_data_back_edge && !layer_drafts[i].has_signal_back_edge)
        {
            CompileEdgeDiagnostic diagnostic;
            diagnostic.kind = CompileDiagnosticKind::UnbackedDataCycle;
            diagnostic.node_id = layer_drafts[i].head_node_id;
            diagnostic.message = DAS_FMT_NS::format(
                "A data edge cycle has no signal circuit carrying it; carry "
                "the loop with a while/for component, or wire the loop "
                "body's completion signal back to the loop head (head = '{}')",
                layer_drafts[i].head_node_id);
            result.diagnostics.push_back(std::move(diagnostic));
        }
    }

    // Recorded members: core ∪ closure, plus every strictly-nested layer's
    // recorded members — the outer reset set must cover the inner loop's
    // entirety. Nesting is decided core-side (head containment); closure
    // producers never carry a signal in-edge, so they can never masquerade
    // as heads and the recursion stays acyclic.
    std::vector<std::set<std::string>> recorded_members(layer_drafts.size());
    std::function<void(std::size_t)>   collect_recorded = [&](std::size_t i)
    {
        if (!recorded_members[i].empty())
        {
            return;
        }
        recorded_members[i] = layer_drafts[i].expanded_members;
        for (std::size_t j = 0; j < layer_drafts.size(); ++j)
        {
            if (j == i
                || layer_drafts[i].scc_members.count(
                       layer_drafts[j].head_node_id)
                       == 0)
            {
                continue;
            }
            collect_recorded(j);
            recorded_members[i].insert(
                recorded_members[j].begin(),
                recorded_members[j].end());
        }
    };
    for (std::size_t i = 0; i < layer_drafts.size(); ++i)
    {
        collect_recorded(i);
    }

    for (std::size_t i = 0; i < layer_drafts.size(); ++i)
    {
        Dto::LoopLayerDto layer;
        layer.head_node_id = layer_drafts[i].head_node_id;
        layer.tier = layer_tiers[i];
        for (const auto& node : document.nodes)
        {
            if (recorded_members[i].count(node.node_id) > 0)
            {
                layer.members.push_back(node.node_id);
            }
        }
        for (const auto& [node_id, port_id] : layer_data_sources[i])
        {
            Dto::PortRefDto ref;
            ref.node_id = node_id;
            ref.port_id = port_id;
            layer.data_back_edge_sources.push_back(std::move(ref));
        }
        result.loop_layers.push_back(std::move(layer));
    }

    // Leak lint: a member's data edge leaving every loop layer (and not a
    // back edge) is a stale-value risk, not an error — the port does not
    // refresh on skipped iterations, so outside readers observe the last
    // active iteration's value. Edges between two layers' members (an inner
    // body feeding an outer body) are fine: the outer restart re-runs the
    // inner loop from scratch, so the value never goes stale across a
    // boundary it actually crosses.
    std::unordered_set<std::string> any_layer_members;
    for (std::size_t i = 0; i < layer_drafts.size(); ++i)
    {
        any_layer_members.insert(
            recorded_members[i].begin(),
            recorded_members[i].end());
    }
    for (const auto& edge : document.edges)
    {
        if (edge.edge_type == "signal" || back_edge_ids.count(edge.edge_id) > 0
            || any_layer_members.count(edge.source_node_id) == 0
            || any_layer_members.count(edge.target_node_id) > 0)
        {
            continue;
        }
        CompileEdgeDiagnostic diagnostic;
        diagnostic.kind = CompileDiagnosticKind::LoopBodyDataLeak;
        diagnostic.node_id = edge.source_node_id;
        diagnostic.port_id = edge.source_port_id;
        diagnostic.edge_id = edge.edge_id;
        diagnostic.message = DAS_FMT_NS::format(
            "Loop-body data edge leaves every loop layer: port '{}.{}' does "
            "not refresh on skipped iterations, so outside readers observe "
            "the last active iteration's value; carry the state with a data "
            "back edge instead",
            edge.source_node_id,
            edge.source_port_id);
        result.diagnostics.push_back(std::move(diagnostic));
    }

    // Signal routes: every signal edge becomes a route (forward + back).
    // activation_condition names the source port whose emission fires the
    // route; Stage 4 may refine it to a concrete signal value via manifest
    // tags. Back-edges appear here too — they carry the loop-back signal.
    for (const auto& edge : document.edges)
    {
        if (edge.edge_type != "signal")
        {
            continue;
        }

        Dto::SignalRouteDto route;
        route.source_node_id = edge.source_node_id;
        route.source_port_id = edge.source_port_id;
        route.target_node_id = edge.target_node_id;
        route.target_port_id = edge.target_port_id;

        auto cond = Das::Utils::MakeYyjsonObject();
        auto cond_obj = *cond.as_object();
        cond_obj[std::string_view("signal")] = std::make_pair(
            std::string_view(edge.source_port_id),
            yyjson::copy_string);
        route.activation_condition = std::move(cond);

        result.signal_routes.push_back(std::move(route));
    }

    // Kahn over (data edges ∪ forward signal edges) — every back-edge removed
    // so this subgraph is a DAG and all nodes get ordered.
    std::unordered_map<std::string, std::vector<std::string>> forward_adj;
    std::unordered_map<std::string, int>                      in_degree;
    for (const auto& node : document.nodes)
    {
        in_degree[node.node_id] = 0;
    }

    for (const auto& edge : document.edges)
    {
        if (back_edge_ids.count(edge.edge_id) > 0)
        {
            continue;
        }
        if (node_index.count(edge.source_node_id) == 0
            || node_index.count(edge.target_node_id) == 0)
        {
            continue;
        }
        forward_adj[edge.source_node_id].push_back(edge.target_node_id);
        in_degree[edge.target_node_id]++;
    }

    auto cmp = [&](const std::string& a, const std::string& b)
    { return node_index[a] > node_index[b]; };
    std::priority_queue<std::string, std::vector<std::string>, decltype(cmp)>
        heap(cmp);
    for (const auto& node : document.nodes)
    {
        if (in_degree[node.node_id] == 0)
        {
            heap.push(node.node_id);
        }
    }

    while (!heap.empty())
    {
        const std::string current = heap.top();
        heap.pop();
        result.execution_order.push_back(current);

        auto it = forward_adj.find(current);
        if (it != forward_adj.end())
        {
            for (const auto& neighbor : it->second)
            {
                if (--in_degree[neighbor] == 0)
                {
                    heap.push(neighbor);
                }
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// GeneratePortBindingPlan
// ---------------------------------------------------------------------------

Dto::PortBindingPlanDto GraphCompiler::GeneratePortBindingPlan(
    const Dto::GraphDocumentDto& document)
{
    Dto::PortBindingPlanDto plan;

    if (!factory_manager_ && !port_surface_provider_)
    {
        return plan;
    }

    auto node_index = BuildNodeIndex(document);

    // Cache manifest lookups
    std::unordered_map<std::string, ManifestPorts> manifest_cache;

    for (const auto& edge : document.edges)
    {
        Dto::PortBindingDto binding;
        binding.source_node_id = edge.source_node_id;
        binding.source_port_id = edge.source_port_id;
        binding.target_node_id = edge.target_node_id;
        binding.target_port_id = edge.target_port_id;

        // Resolve expected_type from source manifest
        auto src_it = node_index.find(edge.source_node_id);
        if (src_it != node_index.end())
        {
            const auto& src_node = *src_it->second;
            if (src_node.target.target_kind == "componentRef")
            {
                auto src_manifest = ResolveNodePorts(src_node, manifest_cache);
                for (const auto& p : src_manifest.outputs)
                {
                    if (p.port_id == edge.source_port_id)
                    {
                        binding.expected_type = p.port_type;
                        break;
                    }
                }
            }
            // Non-componentRef sources keep an empty expected_type — the
            // binding plan never invents a type it cannot resolve.
        }

        // Resolve target_type from the target manifest inputs — the
        // read-side materialisation knowledge. Mirrors the source-side
        // block: only a resolved declaration fills the field, otherwise
        // it stays empty.
        auto tgt_it = node_index.find(edge.target_node_id);
        if (tgt_it != node_index.end())
        {
            const auto& tgt_node = *tgt_it->second;
            if (tgt_node.target.target_kind == "componentRef")
            {
                auto tgt_manifest = ResolveNodePorts(tgt_node, manifest_cache);
                for (const auto& p : tgt_manifest.inputs)
                {
                    if (p.port_id == edge.target_port_id)
                    {
                        binding.target_type = p.port_type;
                        break;
                    }
                }
            }
        }

        plan.bindings.push_back(std::move(binding));
    }

    // loopCarried 自环值路由：声明面 loopCarried 输出口与同名输入口成对
    // 时合成节点内 binding（循环状态入帧的值通道）。合成边不进
    // document.edges、不进边校验；同名输入口缺失的声明在 Compile 校验段
    // 报诊断，这里跳过不合成。
    for (const auto& node : document.nodes)
    {
        if (node.target.target_kind != "componentRef"
            || !node.target.component_ref.has_value())
        {
            continue;
        }
        const ManifestPorts node_ports = ResolveNodePorts(node, manifest_cache);
        for (const auto& output_port : node_ports.outputs)
        {
            if (!output_port.loop_carried)
            {
                continue;
            }
            const auto matched_input = std::find_if(
                node_ports.inputs.begin(),
                node_ports.inputs.end(),
                [&](const PortEntry& candidate)
                { return candidate.port_id == output_port.port_id; });
            if (matched_input == node_ports.inputs.end())
            {
                continue;
            }
            Dto::PortBindingDto binding;
            binding.source_node_id = node.node_id;
            binding.source_port_id = output_port.port_id;
            binding.target_node_id = node.node_id;
            binding.target_port_id = matched_input->port_id;
            binding.expected_type = output_port.port_type;
            binding.target_type = matched_input->port_type;
            plan.bindings.push_back(std::move(binding));
        }
    }

    // Derived broadcast bindings: every dangling input port (no incoming
    // edge) is a boundary input of this graph — it receives the same
    // $graph_input sentinel binding carrying the port's declared default (the
    // manifest channel carries no default, so the value is null there and the
    // runtime null branch leaves the port to the component default), so the
    // runtime materialises it and the caller-side value forwarding has a
    // patch target.
    Dto::BoundaryPortsDto boundary = DeriveBoundaryPorts(document);
    for (const auto& boundary_input : boundary.inputs)
    {
        for (const auto& node_id : boundary_input.node_ids)
        {
            Dto::PortBindingDto binding;
            binding.source_node_id = std::string(GRAPH_INPUT_BROADCAST_SOURCE);
            binding.source_port_id = boundary_input.port_id;
            binding.target_node_id = node_id;
            binding.target_port_id = boundary_input.port_id;
            binding.expected_type = boundary_input.port_type;
            // The boundary input IS the target port — its derived declared
            // type is the target-side knowledge, same source of truth as
            // expected_type here.
            binding.target_type = boundary_input.port_type;
            binding.default_value =
                Das::Utils::CloneYyjsonValue(boundary_input.default_value);
            plan.bindings.push_back(std::move(binding));
        }
    }

    return plan;
}

// ---------------------------------------------------------------------------
// ValidateRequiredFields
// ---------------------------------------------------------------------------

std::vector<CompileEdgeDiagnostic> GraphCompiler::ValidateRequiredFields(
    const Dto::GraphDocumentDto& document,
    const std::unordered_map<std::string, std::unordered_set<std::string>>&
        required_fields)
{
    std::vector<CompileEdgeDiagnostic> diagnostics;
    if (required_fields.empty())
    {
        return diagnostics;
    }
    for (const auto& node : document.nodes)
    {
        if (node.target.target_kind != "componentRef"
            || !node.target.component_ref.has_value())
        {
            continue;
        }
        const auto required_it =
            required_fields.find(node.target.component_ref->component_guid);
        if (required_it == required_fields.end())
        {
            continue;
        }
        auto settings_obj = node.settings.as_object();
        for (const auto& field_name : required_it->second)
        {
            bool satisfied = false;
            if (settings_obj.has_value()
                && settings_obj->contains(std::string_view(field_name)))
            {
                const auto& value =
                    (*settings_obj)[std::string_view(field_name)];
                if (!value.is_null())
                {
                    // 空值判定与运行时 launch exe_path.empty() 对齐：字符串
                    // 额外要求非纯空白；数值/布尔/结构类型键存在即满足。
                    if (auto text = value.as_string(); text.has_value())
                    {
                        satisfied = std::any_of(
                            text->begin(),
                            text->end(),
                            [](char ch)
                            {
                                return std::isspace(
                                           static_cast<unsigned char>(ch))
                                       == 0;
                            });
                    }
                    else
                    {
                        satisfied = true;
                    }
                }
            }
            if (!satisfied)
            {
                CompileEdgeDiagnostic diagnostic;
                diagnostic.kind = CompileDiagnosticKind::RequiredFieldMissing;
                diagnostic.node_id = node.node_id;
                diagnostic.message =
                    "Required field '" + field_name + "' has no value";
                diagnostics.push_back(std::move(diagnostic));
            }
        }
    }
    return diagnostics;
}

// ---------------------------------------------------------------------------
// DeriveBoundaryPorts
// ---------------------------------------------------------------------------

Dto::BoundaryPortsDto GraphCompiler::DeriveBoundaryPorts(
    const Dto::GraphDocumentDto& document) const
{
    ComponentPortSurfaceProvider provider;
    if (factory_manager_ || port_surface_provider_)
    {
        // 声明端口面解析次序与 ResolveDeclaredPorts 一致（manifest 优先、
        // 注入 provider 兜底），但注入通道直接透传完整端口定义——
        // ComponentPortSurface 携带默认值等元数据，经 ManifestPorts 归一
        // 会丢字段（broadcast 绑定的 default 物化依赖它）。
        provider = [this](const std::string& component_guid)
            -> std::optional<ComponentPortSurface>
        {
            if (factory_manager_)
            {
                const ManifestPorts ports = ReadManifest(component_guid);
                if (ports.was_resolved)
                {
                    ComponentPortSurface surface;
                    for (const auto& port : ports.inputs)
                    {
                        Dto::GraphPortDefinitionDto definition;
                        definition.port_id = port.port_id;
                        definition.port_type = port.port_type;
                        definition.loop_carried = port.loop_carried;
                        surface.inputs.push_back(std::move(definition));
                    }
                    for (const auto& port : ports.outputs)
                    {
                        Dto::GraphPortDefinitionDto definition;
                        definition.port_id = port.port_id;
                        definition.port_type = port.port_type;
                        definition.loop_carried = port.loop_carried;
                        surface.outputs.push_back(std::move(definition));
                    }
                    return surface;
                }
            }
            if (port_surface_provider_)
            {
                return port_surface_provider_(component_guid);
            }
            return std::nullopt;
        };
    }
    // begin/end 文档口面直读（动态口不经 provider 中介）：faces 容器按节
    // 点注入，begin/end 节点在图内结构唯一（session 契约 exactly-one
    // begin / at-most-one end），guid 定位无聚合歧义。动态口面按口对双
    // 侧展开——begin 输出侧/end 输入侧由推导规则排除（D-14 双机制），
    // 边界寻址的口对语义保留在推导层。faces 条目缺席（编译宿主未跑预
    // 备 pass）=动态口面为空 + WARN——不回退任何分区/manifest 通道
    // （begin/end 动态口无静态声明，文档口面是其唯一来源）；条目在场
    // 而数组为空是合法文档态（未声明端口），不告警。声明面（如 end 的
    // manifest 声明口「值」）照常经 provider 解析并与动态口面并集（同
    // 名口 face 胜出+WARN——与 ResolveNodePorts 的合并规则一致）。
    ComponentPortSurfaceProvider face_resolving_provider =
        [this, &document, &provider](const std::string& component_guid)
        -> std::optional<ComponentPortSurface>
    {
        const bool is_boundary_guid =
            component_guid == GRAPH_BEGIN_COMPONENT_GUID
            || component_guid == GRAPH_END_COMPONENT_GUID;
        if (!is_boundary_guid)
        {
            return provider ? provider(component_guid) : std::nullopt;
        }
        for (const auto& node : document.nodes)
        {
            if (node.target.target_kind != "componentRef"
                || !node.target.component_ref.has_value()
                || node.target.component_ref->component_guid != component_guid)
            {
                continue;
            }
            std::optional<ComponentPortSurface> surface;
            if (provider)
            {
                surface = provider(component_guid);
            }
            // faces 条目定位（faces 容器缺席=同条目缺席：动态口面为空——
            // 原三目判据反写，null 分支解引用 node_faces_->end() 属未定
            // 义行为，正向化为指针判定）。
            const std::vector<DocumentFacePort>* injected_faces = nullptr;
            if (node_faces_ != nullptr)
            {
                const auto found = node_faces_->find(node.node_id);
                if (found != node_faces_->end())
                {
                    injected_faces = &found->second;
                }
            }
            if (injected_faces == nullptr)
            {
                DAS_CORE_LOG_WARN(
                    "boundary node {} has no injected document faces; its "
                    "dynamic port face derives as empty",
                    node.node_id);
                return surface;
            }
            if (!surface.has_value())
            {
                surface = ComponentPortSurface{};
            }
            for (const auto& face : *injected_faces)
            {
                // 同名声明口：face 胜出（双侧重投影）+ WARN。
                for (auto* side : {&surface->inputs, &surface->outputs})
                {
                    const auto removed = std::remove_if(
                        side->begin(),
                        side->end(),
                        [&face](const Dto::GraphPortDefinitionDto& entry)
                        { return entry.port_id == face.port_id; });
                    if (removed != side->end())
                    {
                        DAS_CORE_LOG_WARN(
                            "boundary node {} port {} is redeclared by the "
                            "injected document face; the face wins (type={})",
                            node.node_id,
                            face.port_id,
                            face.port_type);
                        side->erase(removed, side->end());
                    }
                }
                Dto::GraphPortDefinitionDto definition;
                definition.port_id = face.port_id;
                definition.display_label = face.label;
                definition.port_type = face.port_type;
                surface->inputs.push_back(definition);
                surface->outputs.push_back(definition);
            }
            return surface;
        }
        return std::nullopt;
    };
    return DeriveBoundaryPorts(document, face_resolving_provider);
}

Dto::BoundaryPortsDto GraphCompiler::DeriveBoundaryPorts(
    const Dto::GraphDocumentDto&        document,
    const ComponentPortSurfaceProvider& provider)
{
    // 边角色证据：文档内对输入口「值是否有来源」的唯一证据是入边——
    // target 侧被引用即有来源。悬空判定不需要仓库访问。
    std::unordered_map<std::string, std::unordered_set<std::string>>
        input_roles;
    for (const auto& edge : document.edges)
    {
        input_roles[edge.target_node_id].insert(edge.target_port_id);
    }

    Dto::BoundaryPortsDto                        boundary;
    std::unordered_map<std::string, std::size_t> merged_input_index;
    std::unordered_map<std::string, std::size_t> merged_output_index;

    auto referenced_as_input = [&input_roles](
                                   const std::string& node_id,
                                   const std::string& port_id) -> bool
    {
        auto it = input_roles.find(node_id);
        return it != input_roles.end() && it->second.count(port_id) > 0;
    };

    for (const auto& node : document.nodes)
    {
        std::optional<ComponentPortSurface> surface;
        if (provider && node.target.target_kind == "componentRef"
            && node.target.component_ref.has_value())
        {
            surface = provider(node.target.component_ref->component_guid);
        }

        // loopCarried 声明落在输出口上，其同名输入口的值来源是编译器合成
        // 的节点内自环通道（非 document 边，悬空判定对它不可见）——收集
        // 携带口名集合，输入侧据此排除，内部状态通道不出现在图边界面上。
        std::unordered_set<std::string> carried_port_ids;
        if (surface.has_value())
        {
            for (const auto& output_port : surface->outputs)
            {
                if (output_port.loop_carried)
                {
                    carried_port_ids.insert(output_port.port_id);
                }
            }
        }

        auto add_boundary_input = [&](const Dto::GraphPortDefinitionDto& port)
        {
            if (referenced_as_input(node.node_id, port.port_id))
            {
                return; // 有入边，值已有来源，非边界入参。
            }
            if (carried_port_ids.count(port.port_id) > 0)
            {
                return;
            }
            auto merged = merged_input_index.find(port.port_id);
            if (merged == merged_input_index.end())
            {
                Dto::BoundaryPortDto entry;
                entry.port_id = port.port_id;
                entry.display_label = port.display_label;
                entry.port_type = port.port_type;
                entry.is_required = port.is_required;
                entry.default_value =
                    Das::Utils::CloneYyjsonValue(port.default_value);
                entry.node_ids.push_back(node.node_id);
                merged_input_index[port.port_id] = boundary.inputs.size();
                boundary.inputs.push_back(std::move(entry));
                return;
            }
            // by-name 合并：同名悬空输入口共用一个边界口（广播语义），
            // 先出现者贡献元数据。
            auto& existing = boundary.inputs[merged->second];
            if (std::find(
                    existing.node_ids.begin(),
                    existing.node_ids.end(),
                    node.node_id)
                == existing.node_ids.end())
            {
                existing.node_ids.push_back(node.node_id);
            }
        };

        auto add_boundary_output = [&](const Dto::GraphPortDefinitionDto& port)
        {
            // 输出侧全量导出：内部消费（有出边）与对外导出不互斥，扇出
            // 合法——半跨界口（框内外均有边的扇出口）因此自动暴露，提取
            // 后父图重连成立。
            auto merged = merged_output_index.find(port.port_id);
            if (merged == merged_output_index.end())
            {
                Dto::BoundaryPortDto entry;
                entry.port_id = port.port_id;
                entry.display_label = port.display_label;
                entry.port_type = port.port_type;
                entry.is_required = port.is_required;
                entry.default_value =
                    Das::Utils::CloneYyjsonValue(port.default_value);
                entry.node_ids.push_back(node.node_id);
                merged_output_index[port.port_id] = boundary.outputs.size();
                boundary.outputs.push_back(std::move(entry));
                return;
            }
            auto& existing = boundary.outputs[merged->second];
            if (std::find(
                    existing.node_ids.begin(),
                    existing.node_ids.end(),
                    node.node_id)
                == existing.node_ids.end())
            {
                existing.node_ids.push_back(node.node_id);
            }
        };

        // begin 的输出口导出排除（D-14）：begin 的同名 out 口是图内值边
        // 源，导出会造成入参回显污染出参面——输出侧跳过 begin 节点；end
        // 不排除（其口就是出参面）。输入侧对称排除 end（D-14 镜像）：end
        // 的口语义是「从图内收值」，悬空也不构成图级入参——广播绑定会把
        // 调用方图输入写进 end 转写输出，原值回声进出参面（含任务返回码
        // 注入）。begin/end 动态口 id 是节点内计数器（首口恒 p_1），排除
        // 同时消掉 by-name 合并的系统性撞名面（动态口分区仅 begin/end
        // 可持）。
        const bool node_is_boundary_component =
            node.target.target_kind == "componentRef"
            && node.target.component_ref.has_value();
        const bool node_is_begin = node_is_boundary_component
                                   && node.target.component_ref->component_guid
                                          == GRAPH_BEGIN_COMPONENT_GUID;
        const bool node_is_end = node_is_boundary_component
                                 && node.target.component_ref->component_guid
                                        == GRAPH_END_COMPONENT_GUID;

        if (surface.has_value())
        {
            if (!node_is_end)
            {
                for (const auto& port : surface->inputs)
                {
                    add_boundary_input(port);
                }
            }
            if (!node_is_begin)
            {
                for (const auto& port : surface->outputs)
                {
                    add_boundary_output(port);
                }
            }
        }
    }

    return boundary;
}

// ---------------------------------------------------------------------------
// DiagnosticToJson
// ---------------------------------------------------------------------------

yyjson::value GraphCompiler::DiagnosticToJson(const CompileEdgeDiagnostic& diag)
{
    auto payload = Das::Utils::MakeYyjsonObject();
    auto obj = *payload.as_object();

    auto kind_str = std::to_string(static_cast<int>(diag.kind));
    obj[std::string_view("kind")] =
        std::make_pair(std::string_view(kind_str), yyjson::copy_string);
    obj[std::string_view("node_id")] =
        std::make_pair(std::string_view(diag.node_id), yyjson::copy_string);
    obj[std::string_view("port_id")] =
        std::make_pair(std::string_view(diag.port_id), yyjson::copy_string);
    obj[std::string_view("edge_id")] =
        std::make_pair(std::string_view(diag.edge_id), yyjson::copy_string);
    obj[std::string_view("message")] =
        std::make_pair(std::string_view(diag.message), yyjson::copy_string);
    obj[std::string_view("expected_type")] = std::make_pair(
        std::string_view(diag.expected_type),
        yyjson::copy_string);
    obj[std::string_view("actual_type")] =
        std::make_pair(std::string_view(diag.actual_type), yyjson::copy_string);

    return payload;
}

yyjson::value GraphCompiler::DiagnosticToWireContract(
    const yyjson::value& diagnostic)
{
    auto payload = Das::Utils::MakeYyjsonObject();
    auto obj_o = payload.as_object();
    if (!obj_o)
    {
        return payload;
    }
    auto obj = *obj_o;

    // kind 在 plan 内部形状里是枚举整数的十进制文本（E2E 契约固化），
    // 投影侧解析回整数以映射 severity 并填可选 code。
    int  kind_value = -1;
    auto source_o = diagnostic.as_object();
    if (source_o && source_o->contains(std::string_view("kind")))
    {
        if (const auto kind_text =
                (*source_o)[std::string_view("kind")].as_string())
        {
            std::from_chars(
                kind_text->data(),
                kind_text->data() + kind_text->size(),
                kind_value);
        }
    }
    const bool is_non_blocking_lint =
        kind_value == static_cast<int>(CompileDiagnosticKind::UnknownPortType)
        || kind_value
               == static_cast<int>(CompileDiagnosticKind::LoopBodyDataLeak)
        || kind_value
               == static_cast<int>(CompileDiagnosticKind::StaleEdgeDisconnect);
    obj[std::string_view("severity")] = std::make_pair(
        std::string_view{is_non_blocking_lint ? "warning" : "error"},
        yyjson::copy_string);

    std::string message_text;
    if (source_o && source_o->contains(std::string_view("message")))
    {
        if (const auto message =
                (*source_o)[std::string_view("message")].as_string())
        {
            message_text = std::string(*message);
        }
    }
    obj[std::string_view("message")] =
        std::make_pair(std::string_view{message_text}, yyjson::copy_string);

    if (kind_value >= 0)
    {
        obj[std::string_view("code")] = static_cast<std::int64_t>(kind_value);
    }
    return payload;
}

// ---------------------------------------------------------------------------
// Compile
// ---------------------------------------------------------------------------

Dto::CompiledGraphPlanDto GraphCompiler::Compile(
    const Dto::GraphDocumentDto& document)
{
    static const std::
        unordered_map<std::string, std::unordered_set<std::string>>
            no_required_fields;
    return Compile(document, no_required_fields);
}

Dto::CompiledGraphPlanDto GraphCompiler::Compile(
    const Dto::GraphDocumentDto& document,
    const std::unordered_map<std::string, std::unordered_set<std::string>>&
        required_fields)
{
    Dto::CompiledGraphPlanDto plan;

    // Copy source metadata
    plan.document_id = document.document_id;
    plan.source_revision = document.version;
    plan.source_fingerprint = document.fingerprint;

    // 引用闭包注入 pass（编译期早段）：绑条目节点经
    // creator 开被引条目会话——GetDocument 现算口面（喂边校验）+ Compile
    // 产物（晚段广播绑定）。具名诊断零兜底：任一失败 → execution_order
    // 出口清空。
    RunReferenceClosurePass(document);
    for (auto& diagnostic : reference_diagnostics_)
    {
        plan.diagnostics.push_back(Das::Utils::CloneYyjsonValue(diagnostic));
    }

    // Edge port validation（顺带收集规范口 signal 源边坐标——manifest 知
    // 识只在 ValidateEdgePorts 的成员路径可达，拓扑阶段无此访问面）
    std::set<SignalEdgeCoordinate> canonical_signal_edges;
    auto                           edge_diagnostics =
        ValidateEdgePorts(document, &canonical_signal_edges);
    for (const auto& d : edge_diagnostics)
    {
        plan.diagnostics.push_back(DiagnosticToJson(d));
    }

    // Required-field validation (the session layer injects the
    // catalog-derived map — the compiler has no catalog access).
    auto required_diagnostics =
        ValidateRequiredFields(document, required_fields);
    for (const auto& d : required_diagnostics)
    {
        plan.diagnostics.push_back(DiagnosticToJson(d));
    }

    // 存储边复核（口面解析完成后、产物冻结前）：按节点当前口面复核数
    // 据边，失效边产 WARN 诊断随编译输出返回。写回是 Apply 语境的行为
    // （编译器本体只产诊断零变异——纯 Get/仓条目/OnTick 编译路径读无副
    // 作用）；会话 Apply 路径消费 stale_edges 集合作文档变更。
    auto stale_review = ReviewStaleEdges(document);
    for (const auto& d : stale_review.diagnostics)
    {
        plan.diagnostics.push_back(DiagnosticToJson(d));
    }

    // Signal-aware topological sort (single pass — also yields
    // signal_routes, back_edges and loop_layers for the compiled plan).
    auto topology = ComputeTopology(document);
    plan.execution_order = std::move(topology.execution_order);
    plan.signal_routes = std::move(topology.signal_routes);
    // 规范口打标：命中 ValidateEdgePorts 收集坐标的 route 标记源端为规
    // 范口，运行时据此在组件未自发发射时兜底补发（成功信号是运行时保证
    // 而非各组件的自觉行为）。
    for (auto& route : plan.signal_routes)
    {
        if (canonical_signal_edges.count(
                SignalEdgeCoordinate{
                    route.source_node_id,
                    route.source_port_id,
                    route.target_node_id,
                    route.target_port_id})
            > 0)
        {
            route.source_port_canonical = true;
        }
    }
    plan.back_edges = std::move(topology.back_edges);
    plan.loop_layers = std::move(topology.loop_layers);
    // Topology diagnostics ride along, split by severity: unbacked data
    // cycles fail closed with guidance (the plan carries no executable
    // order), while the loop-body leak lint is a stale-value risk hint
    // that must not block compilation.
    bool has_fatal_topology_diagnostic = false;
    for (const auto& diagnostic : topology.diagnostics)
    {
        plan.diagnostics.push_back(DiagnosticToJson(diagnostic));
        if (diagnostic.kind == CompileDiagnosticKind::UnbackedDataCycle)
        {
            has_fatal_topology_diagnostic = true;
        }
    }
    if (has_fatal_topology_diagnostic)
    {
        plan.execution_order.clear();
    }
    else if (plan.execution_order.empty() && !document.nodes.empty())
    {
        auto payload = Das::Utils::MakeYyjsonObject();
        auto obj = *payload.as_object();
        obj[std::string_view("kind")] = std::make_pair(
            std::string_view(
                std::to_string(
                    static_cast<int>(CompileDiagnosticKind::CyclicEdgeGraph))),
            yyjson::copy_string);
        obj[std::string_view("message")] = std::make_pair(
            std::string_view(
                "Graph contains a cycle - topological sort failed"),
            yyjson::copy_string);
        plan.diagnostics.push_back(std::move(payload));
    }

    // Port binding plan
    plan.binding_plan = GeneratePortBindingPlan(document);

    // 注入 pass 晚段：产物直写广播绑定（$graph_input → executionInput，
    // default_value=compile 段原文）——运行期物化半边（DoAdapter 现成
    // 路径）零新机制。产物永不落盘、不写仓库。派生的悬
    // 空输入广播（无缺省值）让位——pass 产物绑定是该口唯一供值源。
    for (auto& [node_id, artifact_text] : reference_artifacts_)
    {
        auto& bindings = plan.binding_plan.bindings;
        bindings.erase(
            std::remove_if(
                bindings.begin(),
                bindings.end(),
                [&node_id](const Dto::PortBindingDto& candidate)
                {
                    return candidate.source_node_id
                               == std::string{GRAPH_INPUT_BROADCAST_SOURCE}
                           && candidate.target_node_id == node_id
                           && candidate.target_port_id == "executionInput";
                }),
            bindings.end());

        Dto::PortBindingDto reference_broadcast;
        reference_broadcast.source_node_id =
            std::string{GRAPH_INPUT_BROADCAST_SOURCE};
        reference_broadcast.source_port_id = "referenceArtifact";
        reference_broadcast.target_node_id = node_id;
        reference_broadcast.target_port_id = "executionInput";
        reference_broadcast.expected_type = "object";
        reference_broadcast.default_value = yyjson::value(artifact_text);
        plan.binding_plan.bindings.push_back(std::move(reference_broadcast));
    }
    if (reference_pass_failed_)
    {
        plan.execution_order.clear();
    }

    // loopCarried 通道记档与校验：成对口合成 data 型回边入 plan.back_edges
    // （运行期据此把自环排除出 data_preds，节点不会等自己；外层清场经层
    // 清单清到该口）。归属沿用回边归属规则：节点自身为某层头（for 组件的
    // 自然形态）归该层，否则归最内含该节点的层，无层则仅记边不入清单。
    {
        std::unordered_map<std::string, ManifestPorts> manifest_cache;
        for (const auto& node : document.nodes)
        {
            if (node.target.target_kind != "componentRef"
                || !node.target.component_ref.has_value())
            {
                continue;
            }
            const ManifestPorts node_ports =
                ResolveNodePorts(node, manifest_cache);
            for (const auto& output_port : node_ports.outputs)
            {
                if (!output_port.loop_carried)
                {
                    continue;
                }
                const auto matched_input = std::find_if(
                    node_ports.inputs.begin(),
                    node_ports.inputs.end(),
                    [&](const PortEntry& candidate)
                    { return candidate.port_id == output_port.port_id; });
                if (matched_input == node_ports.inputs.end())
                {
                    CompileEdgeDiagnostic diagnostic;
                    diagnostic.kind =
                        CompileDiagnosticKind::LoopCarriedPortInvalid;
                    diagnostic.node_id = node.node_id;
                    diagnostic.port_id = output_port.port_id;
                    diagnostic.message = DAS_FMT_NS::format(
                        "loopCarried output port '{}.{}' has no same-name "
                        "input port to pair with; the loop state channel "
                        "cannot be synthesized",
                        node.node_id,
                        output_port.port_id);
                    plan.diagnostics.push_back(DiagnosticToJson(diagnostic));
                    continue;
                }

                Dto::BackEdgeDto back;
                back.edge_id =
                    "loopcarried::" + node.node_id + "::" + output_port.port_id;
                back.source_node_id = node.node_id;
                back.source_port_id = output_port.port_id;
                back.target_node_id = node.node_id;
                back.target_port_id = matched_input->port_id;
                back.edge_kind = "data";

                std::size_t owner = plan.loop_layers.size();
                for (std::size_t layer_index = 0;
                     layer_index < plan.loop_layers.size();
                     ++layer_index)
                {
                    const auto& layer = plan.loop_layers[layer_index];
                    if (layer.head_node_id == node.node_id)
                    {
                        owner = layer_index;
                        break;
                    }
                }
                if (owner == plan.loop_layers.size())
                {
                    for (std::size_t layer_index = 0;
                         layer_index < plan.loop_layers.size();
                         ++layer_index)
                    {
                        const auto& layer = plan.loop_layers[layer_index];
                        const bool  is_member = std::find(
                                                    layer.members.begin(),
                                                    layer.members.end(),
                                                    node.node_id)
                                                != layer.members.end();
                        if (is_member
                            && (owner == plan.loop_layers.size()
                                || layer.tier > plan.loop_layers[owner].tier))
                        {
                            owner = layer_index;
                        }
                    }
                }
                if (owner != plan.loop_layers.size())
                {
                    back.loop_head_node_id =
                        plan.loop_layers[owner].head_node_id;
                    Dto::PortRefDto source;
                    source.node_id = node.node_id;
                    source.port_id = output_port.port_id;
                    plan.loop_layers[owner].data_back_edge_sources.push_back(
                        std::move(source));
                }
                else
                {
                    // 节点不在任何环层（如未连线环体的裸 for）：回边仍记档
                    // 保住 data_preds 排除，头指针回落节点自身保持形状。
                    back.loop_head_node_id = node.node_id;
                }
                plan.back_edges.push_back(std::move(back));
            }
        }
    }

    // 推导边界口清单入编译产物：输入侧（悬空声明输入）已由
    // GeneratePortBindingPlan 落成 broadcast 绑定；输出侧（声明面输出口
    // 全量）是执行侧 graphOutputs 导出的依据。
    plan.boundary_ports = DeriveBoundaryPorts(document);

    // Build node snapshots from graph document nodes. A node whose target is
    // not a componentRef keeps an empty component_guid; the runtime's
    // Configure step rejects it with INVALID_ARGUMENT (fail-closed handling
    // of unknown target kinds — no silent fallback).
    for (const auto& node : document.nodes)
    {
        Dto::CompiledNodeSnapshotDto snapshot;
        snapshot.node_id = node.node_id;

        // Extract component_guid from the node's target
        if (node.target.target_kind == "componentRef"
            && node.target.component_ref.has_value())
        {
            snapshot.component_guid = node.target.component_ref->component_guid;
        }

        // Copy settings if present
        if (!node.settings.is_null())
        {
            snapshot.compiled_settings =
                Das::Utils::CloneYyjsonValue(node.settings);
        }

        plan.node_snapshots.push_back(std::move(snapshot));
    }

    return plan;
}

DAS_CORE_GRAPHRUNTIME_NS_END
