#define DAS_BUILD_SHARED

#include "MaapiAuthoringSession.h"

#include "PluginUtils.h"

#include <das/Plugins/DasMaaPi/PiCompiler.h>

#include <set>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    namespace
    {
        // D-84-07: Apply 严格 dispatch 表。把入站 change 路由到 Phase 84 严格
        // handler；legacy kind 一律拒绝为 unsupportedChange 并 emit error
        // 诊断。 返回 true 表示 mutation 已应用（candidate 已写入）；false
        // 表示拒绝 （调用方丢弃 candidate、不递增 revision）。无论
        // true/false，均可能 向 diagnostics 追加 info/warning/error 条目。
        // invokeCommand kind 不在本表 — 它的 ApplyInvokeCommandChange 返回
        // InvokeCommandResult 结构体（含可选 command_result），由调用方
        // ApplyChange 显式 dispatch，不走本 bool 表。
        bool DispatchStrictChange(
            const std::string&              kind,
            const yyjson::value&            payload,
            AcceptedSettingsDto&            candidate,
            const std::optional<PiCatalog>& catalog,
            std::vector<PiDiagnosticDto>&   diagnostics)
        {
            const PiCatalog* catalog_ptr =
                catalog.has_value() ? &*catalog : nullptr;
            if (kind == "setValue")
            {
                return ApplyStrictSetValueChange(
                    candidate,
                    payload,
                    catalog_ptr,
                    diagnostics);
            }
            if (kind == "addFormSequenceItem")
            {
                return ApplyAddFormSequenceItem(
                    candidate,
                    catalog_ptr,
                    payload,
                    diagnostics);
            }
            if (kind == "moveFormSequenceItem")
            {
                return ApplyMoveFormSequenceItem(
                    candidate,
                    payload,
                    diagnostics);
            }
            if (kind == "removeFormSequenceItem")
            {
                return ApplyRemoveFormSequenceItem(
                    candidate,
                    payload,
                    diagnostics);
            }
            if (kind == "convertView")
            {
                // MaaPI 只支持 formSequence；任何 convertView 请求都是
                // unsupportedChange。
                PiDiagnosticDto diag;
                diag.severity = "error";
                diag.message = "convertView not supported in MaaPI; only "
                               "formSequence view is available";
                diag.int_code = DAS_E_INVALID_ARGUMENT;
                diagnostics.push_back(std::move(diag));
                return false;
            }
            if (kind == "uiEvent")
            {
                return ApplyUiEventChange(candidate, payload, diagnostics);
            }
            // 84-01 严格 change kinds 中 MaaPI 不支持的（graphNode 系列）+
            // 所有 legacy alias（applyPreset/addSequenceItem/moveSequenceItem/
            // removeSequenceItem 等）一律 unsupportedChange。
            PiDiagnosticDto diag;
            diag.severity = "error";
            diag.message =
                "Unsupported change kind for MaaPI: " + std::string(kind);
            diag.int_code = DAS_E_INVALID_ARGUMENT;
            diagnostics.push_back(std::move(diag));
            return false;
        }

        // D-84-10: 校验 baseRevision == 当前 session revision。失败时 emit
        // 诊断并返回 false。
        bool ValidateBaseRevision(
            int64_t                       base_revision,
            int64_t                       session_revision,
            std::vector<PiDiagnosticDto>& diagnostics)
        {
            if (base_revision == session_revision)
            {
                return true;
            }
            PiDiagnosticDto diag;
            diag.severity = "error";
            diag.message = "baseRevision mismatch: expected "
                           + std::to_string(session_revision) + " got "
                           + std::to_string(base_revision);
            diag.int_code = DAS_E_OUT_OF_RANGE;
            diagnostics.push_back(std::move(diag));
            return false;
        }

        // D-84-08: 严格解析 Apply 请求外层 {baseRevision, kind, payload}。
        // 成功时填充 out_base_revision/out_kind/out_payload；失败时 emit
        // 诊断并返回 false。
        bool ParseApplyRequest(
            const yyjson::value&          request,
            int64_t&                      out_base_revision,
            std::string&                  out_kind,
            yyjson::value&                out_payload,
            std::vector<PiDiagnosticDto>& diagnostics)
        {
            auto obj_opt = request.as_object();
            if (!obj_opt)
            {
                PiDiagnosticDto diag;
                diag.severity = "error";
                diag.message = "Apply request must be a JSON object";
                diag.int_code = DAS_E_INVALID_ARGUMENT;
                diagnostics.push_back(std::move(diag));
                return false;
            }
            auto                  obj = *obj_opt;
            std::set<std::string> actual;
            for (const auto& [k, _] : obj)
            {
                actual.emplace(k);
            }
            if (actual
                != std::set<std::string>{"baseRevision", "kind", "payload"})
            {
                PiDiagnosticDto diag;
                diag.severity = "error";
                diag.message =
                    "Apply request must have exactly {baseRevision, kind, "
                    "payload}";
                diag.int_code = DAS_E_INVALID_ARGUMENT;
                diagnostics.push_back(std::move(diag));
                return false;
            }
            if (!obj[std::string_view("baseRevision")].is_sint()
                && !obj[std::string_view("baseRevision")].is_uint())
            {
                PiDiagnosticDto diag;
                diag.severity = "error";
                diag.message = "baseRevision must be integer";
                diag.int_code = DAS_E_INVALID_ARGUMENT;
                diagnostics.push_back(std::move(diag));
                return false;
            }
            out_base_revision =
                obj[std::string_view("baseRevision")].is_sint()
                    ? obj[std::string_view("baseRevision")].as_sint().value_or(
                          -1)
                    : static_cast<std::int64_t>(
                          obj[std::string_view("baseRevision")]
                              .as_uint()
                              .value_or(0));
            if (!obj[std::string_view("kind")].is_string())
            {
                PiDiagnosticDto diag;
                diag.severity = "error";
                diag.message = "kind must be string";
                diag.int_code = DAS_E_INVALID_ARGUMENT;
                diagnostics.push_back(std::move(diag));
                return false;
            }
            out_kind = std::string(
                obj[std::string_view("kind")].as_string().value_or(""));
            out_payload =
                Das::Utils::CloneYyjsonValue(obj[std::string_view("payload")]);
            if (!out_payload.is_object())
            {
                PiDiagnosticDto diag;
                diag.severity = "error";
                diag.message = "payload must be a JSON object";
                diag.int_code = DAS_E_INVALID_ARGUMENT;
                diagnostics.push_back(std::move(diag));
                return false;
            }
            return true;
        }

        // 把 port derivation 写入 acceptedProperties（Provider 私有运行期派生，
        // 不进入 document UI IR）。基于 settings_.pi.tasks[0] 计算。
        // 84-07 修复：dynamic_ports 不再写入 envelope 根（违反 84-01
        // ApplyResult 严格 schema）；portDefinitions/portMap 已经在
        // acceptedProperties 内，是 Provider-owned runtime state。
        void InjectPortDerivations(
            yyjson::value&                  result_json,
            const AcceptedSettingsDto&      settings,
            const std::optional<PiCatalog>& catalog)
        {
            if (!catalog || settings.pi.tasks.empty())
            {
                return;
            }
            const auto& task_name = settings.pi.tasks[0].task_name;
            if (task_name.empty())
            {
                return;
            }
            auto port_definitions = DerivePortDefinitions(*catalog, task_name);
            auto port_map = DerivePortMap(*catalog, task_name);

            auto result_obj_opt = result_json.as_object();
            if (!result_obj_opt)
            {
                return;
            }
            auto result_obj = *result_obj_opt;
            if (!result_obj.contains(std::string_view("acceptedProperties")))
            {
                return;
            }
            auto accepted_opt =
                result_obj[std::string_view("acceptedProperties")].as_object();
            if (!accepted_opt)
            {
                return;
            }
            auto accepted = *accepted_opt;
            accepted[std::string_view("portDefinitions")] =
                BuildPortDefinitionsJson(port_definitions);

            yyjson::value port_map_json(Das::Utils::MakeYyjsonObject());
            auto          pm_obj = port_map_json.as_object();
            for (const auto& [port_id, param_name] : port_map)
            {
                (*pm_obj)[std::string_view(port_id)] = std::make_pair(
                    std::string_view(param_name),
                    yyjson::copy_string);
            }
            accepted[std::string_view("portMap")] = std::move(port_map_json);
        }
    } // namespace

    MaapiAuthoringSession::MaapiAuthoringSession(
        AcceptedSettingsDto settings,
        int64_t             revision)
        : settings_(std::move(settings)), revision_(revision)
    {
    }

    DasResult MaapiAuthoringSession::GetDocument(
        ExportInterface::IDasJson*,
        ExportInterface::IDasJson** pp_out_document_json)
    {
        if (!pp_out_document_json)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_document_json = nullptr;

        // D-84-08: 严格 Get envelope = {document, diagnostics}。
        // SchedulerService ValidateAuthoringGetResult 要求 top-level
        // diagnostics 数组（即使空）。BuildDocument 既投影 document 又填充
        // diagnostics，envelope 同级包装二者（与 Compile 对称）。
        std::vector<PiDiagnosticDto> diagnostics;
        auto                         document = BuildDocument(
            settings_,
            revision_,
            diagnostics,
            &pending_update_,
            &device_cache_);

        yyjson::value envelope(Das::Utils::MakeYyjsonObject());
        auto          env_obj = envelope.as_object();
        (*env_obj)[std::string_view("document")] = std::move(document);

        yyjson::value diagnostics_json(Das::Utils::MakeYyjsonArray());
        for (const auto& diagnostic : diagnostics)
        {
            yyjson::value item(Das::Utils::MakeYyjsonObject());
            auto          item_obj = item.as_object();
            (*item_obj)[std::string_view("severity")] =
                JsonString(diagnostic.severity);
            (*item_obj)[std::string_view("message")] =
                JsonString(diagnostic.message);
            if (diagnostic.int_code)
            {
                (*item_obj)[std::string_view("code")] = *diagnostic.int_code;
            }
            if (diagnostic.path)
            {
                (*item_obj)[std::string_view("path")] =
                    JsonString(*diagnostic.path);
            }
            diagnostics_json.as_array()->emplace_back(std::move(item));
        }
        (*env_obj)[std::string_view("diagnostics")] =
            std::move(diagnostics_json);

        auto wrapped = WrapJson(std::move(envelope));
        *pp_out_document_json = wrapped.Get();
        if (*pp_out_document_json == nullptr)
        {
            return DAS_E_FAIL;
        }
        (*pp_out_document_json)->AddRef();
        return DAS_S_OK;
    }

    DasResult MaapiAuthoringSession::ApplyChange(
        ExportInterface::IDasJson*  p_request_json,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (!pp_out_result_json)
        {
            return DAS_E_INVALID_POINTER;
        }
        // IN-01: 与 GetDocument/Compile 对齐——入口清空 out，避免调用方传入
        // 非空 out 指针时旧对象泄漏。
        *pp_out_result_json = nullptr;

        std::vector<PiDiagnosticDto> diagnostics;
        auto catalog = TryParseCatalog(settings_, diagnostics);

        int64_t       base_revision = -1;
        std::string   kind;
        yyjson::value payload(Das::Utils::MakeYyjsonObject());

        // 1) 严格解析请求。失败 → emit 诊断、跳过 mutation、revision 不变。
        bool request_ok = false;
        if (auto request = ReadJson(p_request_json))
        {
            request_ok = ParseApplyRequest(
                *request,
                base_revision,
                kind,
                payload,
                diagnostics);
        }
        else
        {
            PiDiagnosticDto diag;
            diag.severity = "error";
            diag.message = "Apply request could not be parsed as JSON";
            diag.int_code = DAS_E_INVALID_JSON;
            diagnostics.push_back(std::move(diag));
        }

        // 2) 校验 baseRevision。失败 → 同样不 mutation、不递增 revision。
        bool revision_ok = false;
        if (request_ok)
        {
            revision_ok =
                ValidateBaseRevision(base_revision, revision_, diagnostics);
        }

        // 3) Dispatch。成功时把 candidate 写回 settings_ 并 ++revision_。
        //    失败时 settings_ 原样保留，revision_ 不变。
        int64_t result_revision = revision_;
        // invokeCommand 业务结果回传（checkUpdate 填 {commandId, data:
        // {progressKey}}；其它命令 nullopt）。原 ApplyInvokeCommandChange
        // 内部自填（不再由本函数硬编码 checkUpdate 特例）。
        std::optional<yyjson::value> command_result;
        if (request_ok && revision_ok)
        {
            const auto          path_before = settings_.adapter.interface_path;
            AcceptedSettingsDto candidate = settings_;
            bool                dispatch_ok = false;
            if (kind == "invokeCommand")
            {
                // invokeCommand 单独 dispatch — 返回 InvokeCommandResult
                // 结构体（零 T* 出参指针：pending_update_ 经 *this session
                // 引用直读写）。
                auto cmd_result = ApplyInvokeCommandChange(
                    *this,
                    candidate,
                    payload,
                    diagnostics,
                    test_http_,
                    progress_service_seam_);
                dispatch_ok = cmd_result.ok;
                command_result = std::move(cmd_result.command_result);
            }
            else
            {
                dispatch_ok = DispatchStrictChange(
                    kind,
                    payload,
                    candidate,
                    catalog,
                    diagnostics);
            }
            if (dispatch_ok)
            {
                // interfacePath 变更会改变 catalog；新路径必须可解析，否则保留
                // 失败语义——不落盘、不 ++revision，把解析诊断回传前端（远端保持
                // 原值，只在前端留记录）。dispatch 用的是旧 settings_ 解析的
                // catalog，看不到新路径是否有效，故此处按 candidate 复查。
                bool accept = true;
                if (candidate.adapter.interface_path
                    != settings_.adapter.interface_path)
                {
                    std::vector<PiDiagnosticDto> probe;
                    const auto& path = candidate.adapter.interface_path;
                    accept = !path || path->empty()
                             || TryParseCatalog(candidate, probe).has_value();
                    if (!accept)
                    {
                        diagnostics.insert(
                            diagnostics.end(),
                            probe.begin(),
                            probe.end());
                    }
                }
                if (accept)
                {
                    settings_ = std::move(candidate);
                    ++revision_;
                    result_revision = revision_;
                }
                // WR-06: interface_path 变更后按新 settings_ 重新解析 catalog，
                // 否则 InjectPortDerivations 沿用旧路径解析的 catalog（旧路径
                // 失效时为 null 直接跳过），返回的 acceptedProperties 的
                // portDefinitions/portMap 与 document 所用的新 interface.json
                // 不一致（BuildApplyResult 内部已用新路径重解析，document
                // 正确）。
                if (settings_.adapter.interface_path != path_before)
                {
                    catalog = TryParseCatalog(settings_, diagnostics);
                }
            }
        }

        // 4) 构建严格 Apply envelope {acceptedProperties, document,
        //    diagnostics}。失败请求也返回 envelope，但 document.revision
        //    保持原 revision_（无递增），acceptedProperties 反映未变的
        //    settings_。diagnostics 内含 error severity 条目。checkUpdate
        //    成功时附带可选 commandResult（无业务结果的命令不带）。
        auto result_json = BuildApplyResult(
            settings_,
            result_revision,
            std::move(diagnostics),
            /*project_document=*/kind != "uiEvent",
            &pending_update_,
            command_result ? &*command_result : nullptr,
            &device_cache_);

        // 5) 注入 portDefinitions/portMap 到 acceptedProperties（Provider 私有
        //    运行期派生，不属于 document UI IR）。
        InjectPortDerivations(result_json, settings_, catalog);

        auto wrapped = WrapJson(std::move(result_json));
        *pp_out_result_json = wrapped.Get();
        // IN-01: WrapJson 内部 SerializeYyjsonValue 失败时返回空 DasPtr，此处
        // 判空防止 (*pp_out_result_json)->AddRef() 空指针解引用（与
        // GetDocument/Compile 对齐）。
        if (*pp_out_result_json == nullptr)
        {
            return DAS_E_FAIL;
        }
        (*pp_out_result_json)->AddRef();
        return DAS_S_OK;
    }

    DasResult MaapiAuthoringSession::Compile(
        ExportInterface::IDasJson*  p_request_json,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (!pp_out_result_json)
        {
            return DAS_E_INVALID_POINTER;
        }

        // D-84-08/09: Compile 严格 envelope = {compile, diagnostics}。
        // 请求体允许为空 `{}`（Phase 84 客户端不发 purpose/document/
        // properties），但 Provider 仍容忍旧测试 fixture 携带 purpose 字段
        // 用于在 preview/execution 之间切换 PiCompiler 输出。Provider 不持久
        // 化任何状态。
        std::string purpose = "preview";
        if (auto request = ReadJson(p_request_json))
        {
            if (auto obj = request->as_object();
                obj && obj->contains(std::string_view("purpose"))
                && (*obj)[std::string_view("purpose")].is_string())
            {
                purpose = std::string(
                    (*obj)[std::string_view("purpose")].as_string().value_or(
                        "preview"));
            }
        }

        std::vector<PiDiagnosticDto> diagnostics;
        auto catalog = TryParseCatalog(settings_, diagnostics);
        // D-84-08/09: 严格 Compile envelope = {compile, diagnostics}。Provider
        // 不持久化；请求体忽略。compile 字段是 PiCompiler 输出的整体 preview
        // 对象（对 Scheduler opaque），diagnostics 与 compile 同级。
        yyjson::value compile_payload(Das::Utils::MakeYyjsonObject());
        if (!catalog)
        {
            auto obj = compile_payload.as_object();
            (*obj)[std::string_view("ok")] = false;
            (*obj)[std::string_view("canExecute")] = false;
            (*obj)[std::string_view("summary")] =
                Das::Utils::MakeYyjsonObject();
        }
        else
        {
            // PiCompiler 已经在 owning yyjson value 跨函数 move 上踩过坑
            // (cpp_yyjson-value-moved-from-null-deref.md)；SerializeCompileResult
            // 直接生成 IDasJson 句柄避免崩溃。这里再读回为 yyjson::value 以
            // 便包装进严格 envelope。
            const DasPtr<ExportInterface::IDasJson> compiled_handle =
                SerializeCompileResult(
                    CompileMaapi(settings_, *catalog, purpose),
                    purpose);
            if (compiled_handle)
            {
                DasPtr<IDasReadOnlyString> text;
                if (!DAS::IsFailed(compiled_handle->ToString(0, text.Put()))
                    && text)
                {
                    const char* raw = nullptr;
                    if (!DAS::IsFailed(text->GetUtf8(&raw)) && raw != nullptr)
                    {
                        if (auto parsed =
                                Das::Utils::ParseYyjsonFromString(raw))
                        {
                            compile_payload = std::move(*parsed);
                        }
                    }
                }
            }
        }

        // 组装严格 {compile, diagnostics} envelope。
        yyjson::value envelope(Das::Utils::MakeYyjsonObject());
        auto          env_obj = envelope.as_object();
        (*env_obj)[std::string_view("compile")] = std::move(compile_payload);
        yyjson::value diagnostics_json(Das::Utils::MakeYyjsonArray());
        for (const auto& diagnostic : diagnostics)
        {
            yyjson::value item(Das::Utils::MakeYyjsonObject());
            auto          item_obj = item.as_object();
            (*item_obj)[std::string_view("severity")] =
                JsonString(diagnostic.severity);
            if (diagnostic.int_code)
            {
                (*item_obj)[std::string_view("code")] = *diagnostic.int_code;
            }
            (*item_obj)[std::string_view("message")] =
                JsonString(diagnostic.message);
            if (diagnostic.path)
            {
                (*item_obj)[std::string_view("path")] =
                    JsonString(*diagnostic.path);
            }
            diagnostics_json.as_array()->emplace_back(std::move(item));
        }
        (*env_obj)[std::string_view("diagnostics")] =
            std::move(diagnostics_json);

        auto wrapped = WrapJson(std::move(envelope));
        *pp_out_result_json = wrapped.Get();
        if (*pp_out_result_json == nullptr)
        {
            return DAS_E_FAIL;
        }
        (*pp_out_result_json)->AddRef();
        return DAS_S_OK;
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END
