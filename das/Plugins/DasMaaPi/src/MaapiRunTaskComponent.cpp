#define DAS_BUILD_SHARED

#include "MaapiRunTaskComponent.h"

#include "MaapiExecutionKernel.h"
#include "MaapiScheduleBrain.h"
#include "PluginUtils.h"

#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/IDasSchedulerService.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>

#include <spdlog/spdlog.h>

#include <map>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    namespace
    {
        // 仓库条目解析缝（测试可注入；空 = 生产实现）。互斥保护：注入与
        // Do 可能来自不同线程。
        std::mutex& RepositoryResolverMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        RepositoryEntryResolver& RepositoryResolverSlot()
        {
            static RepositoryEntryResolver resolver;
            return resolver;
        }
    } // namespace

    void SetRepositoryEntryResolverForTest(RepositoryEntryResolver resolver)
    {
        const std::scoped_lock lock(RepositoryResolverMutex());
        RepositoryResolverSlot() = std::move(resolver);
    }

    namespace
    {
        // 主进程调度服务的具名注册名（DasHttp 按 das.progress / das.locale
        // 同范式注册）。
        constexpr std::string_view SCHEDULER_SERVICE_NAME = "das.scheduler";

        // execution 编译请求体（与 RepositoryInvoke 编译链的请求一致）。
        constexpr std::string_view EXECUTION_COMPILE_REQUEST =
            R"({"purpose":"execution"})";

        DasResult ReturnJson(
            yyjson::value                     value,
            ExportInterface::IDasJson** const pp_out_result_json)
        {
            auto wrapped = WrapJson(std::move(value));
            if (!wrapped)
            {
                return DAS_E_INVALID_JSON;
            }

            *pp_out_result_json = wrapped.Get();
            (*pp_out_result_json)->AddRef();
            return DAS_S_OK;
        }

        yyjson::value BuildCompletedTasksArray(
            const std::vector<std::string>& completed_tasks)
        {
            auto arr = Das::Utils::MakeYyjsonArray();
            for (const auto& task : completed_tasks)
            {
                arr.as_array()->emplace_back(yyjson::value(std::string{task}));
            }
            return arr;
        }

        // 读取输入口 executionInput 的 envelope JSON 字符串。端口缺失 /
        // 空值返回 nullopt——组件无静默兜底，由调用方显性失败。
        std::optional<std::string> ReadExecutionInputJson(
            ExportInterface::IDasReadOnlyPortMap* p_input_port_map)
        {
            if (p_input_port_map == nullptr)
            {
                return std::nullopt;
            }

            IDasReadOnlyString*     p_value = nullptr;
            const DasReadOnlyString key{"executionInput"};
            if (DAS::IsFailed(p_input_port_map->GetString(key.Get(), &p_value))
                || p_value == nullptr)
            {
                return std::nullopt;
            }

            const char* utf8 = nullptr;
            p_value->GetUtf8(&utf8);
            std::string value(utf8 != nullptr ? utf8 : "");
            p_value->Release();
            if (value.empty())
            {
                return std::nullopt;
            }
            return value;
        }

        // GraphRuntime Configure 阶段的 settings 投递包装：请求体形如
        // {"settings": S}，S 为节点当前配置的全量投影（替换语义）。请求
        // 缺失 / 非对象 / 无 settings 键 / settings 为 null 时按空对象
        // 处理（空投递 = 节点无配置）。
        yyjson::value ReadDeliveredSettings(
            ExportInterface::IDasJson* p_request_json)
        {
            if (p_request_json == nullptr)
            {
                return Das::Utils::MakeYyjsonObject();
            }

            DasPtr<IDasReadOnlyString> request_string;
            if (DAS::IsFailed(p_request_json->ToString(0, request_string.Put()))
                || !request_string)
            {
                return Das::Utils::MakeYyjsonObject();
            }

            const char* request_utf8 = nullptr;
            request_string->GetUtf8(&request_utf8);
            auto parsed = Das::Utils::ParseYyjsonFromString(
                request_utf8 != nullptr ? request_utf8 : "");
            if (!parsed || !parsed->is_object())
            {
                return Das::Utils::MakeYyjsonObject();
            }

            auto request_object = parsed->as_object();
            if (!request_object->contains(std::string_view("settings")))
            {
                return Das::Utils::MakeYyjsonObject();
            }
            const auto& inner = (*request_object)[std::string_view("settings")];
            if (inner.is_null() || !inner.is_object())
            {
                return Das::Utils::MakeYyjsonObject();
            }
            return Das::Utils::CloneYyjsonValue(inner);
        }

        RepositoryResolveOutcome MakeResolveFailure(std::string error_message)
        {
            RepositoryResolveOutcome outcome;
            outcome.ok = false;
            outcome.error_message = std::move(error_message);
            return outcome;
        }

        // 仓库条目解析的生产实现：经主进程 C ABI 查 IDasSchedulerService，
        // 调 CompileRepositoryEntryAuthoring 取编译产物 executionInput
        // （envelope）。跨进程服务访问必须走 C ABI——插件 DLL 内联读取
        // IPC 上下文的 thread_local 跨 DLL 分裂恒空（既有铁律，见
        // CurrentIpcContextScope.h）。
        RepositoryResolveOutcome ResolveRepositoryEntryViaScheduler(
            int64_t entry_id)
        {
            DasPtr<IDasBase> service_base;
            if (DAS::IsFailed(DasQueryMainProcessInterfaceByName(
                    std::string(SCHEDULER_SERVICE_NAME).c_str(),
                    service_base.Put()))
                || !service_base)
            {
                return MakeResolveFailure(
                    "Scheduler service is unavailable (das.scheduler)");
            }

            DasPtr<IDasSchedulerService> scheduler;
            if (DAS::IsFailed(service_base.As(scheduler)) || !scheduler)
            {
                return MakeResolveFailure(
                    "Scheduler service does not expose IDasSchedulerService");
            }

            DasPtr<IDasReadOnlyString> request;
            if (DAS::IsFailed(CreateIDasReadOnlyStringFromUtf8(
                    std::string(EXECUTION_COMPILE_REQUEST).c_str(),
                    request.Put()))
                || !request)
            {
                return MakeResolveFailure(
                    "Failed to build the execution compile request");
            }

            DasPtr<IDasReadOnlyString> response;
            const DasResult            compile_result =
                scheduler->CompileRepositoryEntryAuthoring(
                    entry_id,
                    request.Get(),
                    response.Put());
            if (DAS::IsFailed(compile_result) || !response)
            {
                return MakeResolveFailure(
                    "Repository entry compile call failed with result "
                    + std::to_string(compile_result));
            }

            const char* response_utf8 = nullptr;
            response->GetUtf8(&response_utf8);
            auto parsed = Das::Utils::ParseYyjsonFromString(
                response_utf8 != nullptr ? response_utf8 : "");
            if (!parsed || !parsed->is_object())
            {
                return MakeResolveFailure(
                    "Repository entry compile response is not valid JSON");
            }
            auto response_object = parsed->as_object();
            if (!response_object)
            {
                return MakeResolveFailure(
                    "Repository entry compile response is not valid JSON");
            }

            // 失败信封：{ok:false, errorKind, message}。
            if (response_object->contains(std::string_view("ok"))
                && (*response_object)[std::string_view("ok")].is_bool()
                && !(*response_object)[std::string_view("ok")]
                        .as_bool()
                        .value_or(true))
            {
                std::string error_kind = "unknown";
                std::string error_message = "Repository entry compile failed";
                if (const auto kind =
                        (*response_object)[std::string_view("errorKind")]
                            .as_string())
                {
                    error_kind = std::string(*kind);
                }
                if (const auto message =
                        (*response_object)[std::string_view("message")]
                            .as_string())
                {
                    error_message = std::string(*message);
                }
                return MakeResolveFailure(error_kind + ": " + error_message);
            }

            if (!response_object->contains(std::string_view("compile")))
            {
                return MakeResolveFailure(
                    "Repository entry compile response has no compile "
                    "section");
            }
            auto compile_object =
                (*response_object)[std::string_view("compile")].as_object();
            if (!compile_object)
            {
                return MakeResolveFailure(
                    "Repository entry compile section is not an object");
            }

            // Provider 编译失败：compile.ok 显式 false。
            if (compile_object->contains(std::string_view("ok"))
                && (*compile_object)[std::string_view("ok")].is_bool()
                && !(*compile_object)[std::string_view("ok")]
                        .as_bool()
                        .value_or(true))
            {
                return MakeResolveFailure(
                    "Repository authoring provider failed to compile "
                    "execution input");
            }

            if (!compile_object->contains(std::string_view("executionInput")))
            {
                return MakeResolveFailure(
                    "Repository entry compile did not return executionInput");
            }
            const auto& execution_input =
                (*compile_object)[std::string_view("executionInput")];
            if (!execution_input.is_object())
            {
                return MakeResolveFailure(
                    "Repository entry executionInput is not an object");
            }

            auto serialized = Das::Utils::SerializeYyjsonValue(execution_input);
            if (!serialized)
            {
                return MakeResolveFailure(
                    "Failed to serialize the compiled executionInput");
            }

            RepositoryResolveOutcome outcome;
            outcome.ok = true;
            outcome.envelope_json = std::move(*serialized);
            return outcome;
        }

        // 解析缝分派：测试注入优先，空则走生产实现。
        RepositoryResolveOutcome ResolveRepositoryEntry(int64_t entry_id)
        {
            RepositoryEntryResolver resolver;
            {
                const std::scoped_lock lock(RepositoryResolverMutex());
                resolver = RepositoryResolverSlot();
            }
            if (resolver)
            {
                return resolver(entry_id);
            }
            return ResolveRepositoryEntryViaScheduler(entry_id);
        }

        // 解析失败出口：三输出键照常（completedTasks 空数组 / stopped=false
        // / diagnostics 携带可读原因），返回失败码由调用方转 DAS_E_FAIL。
        DasResult BuildRepositoryResolveFailurePortMap(
            const std::string&                   error_message,
            ExportInterface::IDasPortMap** const pp_out_port_map)
        {
            DasPtr<ExportInterface::IDasPortMap> output_map;
            const DasResult hr = CreateIDasPortMap(output_map.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }

            output_map->SetString(
                DasReadOnlyString("completedTasks").Get(),
                DasReadOnlyString("[]").Get());
            output_map->SetBool(DasReadOnlyString("stopped").Get(), false);

            auto diagnostics_array = Das::Utils::MakeYyjsonArray();
            auto diagnostic_object = Das::Utils::MakeYyjsonObject();
            (*diagnostic_object.as_object())[std::string_view("severity")] =
                std::make_pair(std::string_view("error"), yyjson::copy_string);
            (*diagnostic_object.as_object())[std::string_view("code")] =
                std::make_pair(
                    std::string_view("repository-resolve-failed"),
                    yyjson::copy_string);
            (*diagnostic_object.as_object())[std::string_view("message")] =
                std::make_pair(
                    std::string_view(error_message),
                    yyjson::copy_string);
            diagnostics_array.as_array()->emplace_back(
                std::move(diagnostic_object));

            auto diagnostics_serialized =
                Das::Utils::SerializeYyjsonValue(diagnostics_array);
            if (diagnostics_serialized)
            {
                output_map->SetString(
                    DasReadOnlyString("diagnostics").Get(),
                    DasReadOnlyString(diagnostics_serialized->c_str()).Get());
            }

            *pp_out_port_map = output_map.Get();
            output_map.Get()->AddRef();
            return DAS_S_OK;
        }
    } // namespace

    DasResult MaapiRunTaskComponent::GetGuid(DasGuid* p_out_guid)
    {
        if (p_out_guid == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_out_guid = DasIidOf<MaapiRunTaskComponent>();
        return DAS_S_OK;
    }

    DasResult MaapiRunTaskComponent::GetRuntimeClassName(
        IDasReadOnlyString** pp_out_name)
    {
        if (pp_out_name == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "Das.MaaPi.RunTaskComponent",
            pp_out_name);
    }

    DasResult MaapiRunTaskComponent::ApplySettingsChange(
        ExportInterface::IDasJson*  p_request_json,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (pp_out_result_json == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_result_json = nullptr;

        // manifest config 声明 repositoryRef（type=taskRepositoryRef，值 =
        // 纯 entryId 整数）与 runtimeInputs（object）两键。Configure 投递是
        // 全量替换语义；形状非法整包拒绝（不落半截状态）——项目未发布，
        // 无 legacy 值需要宽容。
        auto                   settings = ReadDeliveredSettings(p_request_json);
        auto                   settings_obj = settings.as_object();
        std::optional<int64_t> accepted_entry_id;
        std::optional<yyjson::value> accepted_runtime_inputs;

        if (settings_obj.has_value()
            && settings_obj->contains(std::string_view("repositoryRef")))
        {
            const auto& repository_ref =
                (*settings_obj)[std::string_view("repositoryRef")];
            if (!repository_ref.is_null())
            {
                const auto entry_id = repository_ref.as_int();
                if (!entry_id.has_value() || *entry_id <= 0)
                {
                    spdlog::error(
                        "[DasMaaPi] ApplySettingsChange: repositoryRef must "
                        "be a positive integer entry id, got non-conforming "
                        "value");
                    return DAS_E_INVALID_ARGUMENT;
                }
                accepted_entry_id = *entry_id;
            }
        }

        if (settings_obj.has_value()
            && settings_obj->contains(std::string_view("runtimeInputs")))
        {
            const auto& runtime_inputs =
                (*settings_obj)[std::string_view("runtimeInputs")];
            if (!runtime_inputs.is_null())
            {
                if (!runtime_inputs.is_object())
                {
                    spdlog::error(
                        "[DasMaaPi] ApplySettingsChange: runtimeInputs must "
                        "be an object");
                    return DAS_E_INVALID_ARGUMENT;
                }
                accepted_runtime_inputs =
                    Das::Utils::CloneYyjsonValue(runtime_inputs);
            }
        }

        {
            const std::scoped_lock lock(settings_mutex_);
            repository_entry_id_ = accepted_entry_id.value_or(0);
            runtime_inputs_ =
                accepted_runtime_inputs.has_value()
                    ? Das::Utils::CloneYyjsonValue(*accepted_runtime_inputs)
                    : yyjson::value{};
        }

        // acceptedSettings 回显本轮实际接受的键（存储值）。
        auto accepted = Das::Utils::MakeYyjsonObject();
        auto accepted_object = accepted.as_object();
        if (accepted_entry_id.has_value())
        {
            (*accepted_object)[std::string_view("repositoryRef")] =
                *accepted_entry_id;
        }
        if (accepted_runtime_inputs.has_value())
        {
            (*accepted_object)[std::string_view("runtimeInputs")] =
                Das::Utils::CloneYyjsonValue(*accepted_runtime_inputs);
        }

        auto result = Das::Utils::MakeYyjsonObject();
        (*result.as_object())[std::string_view("acceptedSettings")] =
            std::move(accepted);
        return ReturnJson(std::move(result), pp_out_result_json);
    }

    DasResult MaapiRunTaskComponent::Do(
        PluginInterface::IDasStopToken*       stop_token,
        ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        ExportInterface::IDasPortMap**        pp_out_port_map)
    {
        if (pp_out_port_map == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_port_map = nullptr;

        // envelope 来源分派：executionInput 端口喂线优先（既有路径零变化）；
        // 端口缺失且 settings 携带有效 repositoryRef 时自行解析仓库条目
        // （invokeRepository 逻辑内化，用户不手动包节点）。
        const auto execution_input_json =
            ReadExecutionInputJson(p_input_port_map);

        std::string   envelope_source;
        yyjson::value runtime_inputs;
        if (execution_input_json.has_value())
        {
            envelope_source = std::move(*execution_input_json);
        }
        else
        {
            int64_t repository_entry_id = 0;
            {
                const std::scoped_lock lock(settings_mutex_);
                repository_entry_id = repository_entry_id_;
                runtime_inputs = Das::Utils::CloneYyjsonValue(runtime_inputs_);
            }
            if (repository_entry_id <= 0)
            {
                return DAS_E_INVALID_ARGUMENT;
            }

            const auto outcome = ResolveRepositoryEntry(repository_entry_id);
            if (!outcome.ok)
            {
                spdlog::error(
                    "[DasMaaPi] Do: repository entry resolve failed: {}",
                    outcome.error_message);
                const DasResult failure_map_result =
                    BuildRepositoryResolveFailurePortMap(
                        outcome.error_message,
                        pp_out_port_map);
                if (DAS::IsFailed(failure_map_result))
                {
                    return failure_map_result;
                }
                return DAS_E_FAIL;
            }
            envelope_source = std::move(outcome.envelope_json);
        }

        auto envelope_json = Das::Utils::ParseYyjsonFromString(envelope_source);
        if (!envelope_json.has_value())
        {
            return DAS_E_INVALID_JSON;
        }

        // runtimeInputs 运行时输入覆盖：settings 键 → envelope 运行时输入段
        // （maapi.tasks[].pipelineOverride）的对象级覆盖——复用执行内核
        // 既有的传参通道（键恒等映射）。
        std::map<std::string, std::string>   runtime_input_port_map;
        std::map<std::string, yyjson::value> runtime_input_values;
        if (auto runtime_inputs_object = runtime_inputs.as_object())
        {
            for (const auto& [input_name, input_value] : *runtime_inputs_object)
            {
                runtime_input_port_map.emplace(
                    std::string(input_name),
                    std::string(input_name));
                runtime_input_values.emplace(
                    std::string(input_name),
                    Das::Utils::CloneYyjsonValue(input_value));
            }
        }

        MaaRuntimeResult run_result;
        const DasResult  execute_result = ExecuteEnvelope(
            *envelope_json,
            MaaApiBoundaryForRuntime(),
            stop_token,
            runtime_input_port_map,
            runtime_input_values,
            run_result);

        DasPtr<ExportInterface::IDasPortMap> output_map;
        DasResult hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }

        auto completed_tasks_array =
            BuildCompletedTasksArray(run_result.completed_tasks);
        auto completed_serialized =
            Das::Utils::SerializeYyjsonValue(completed_tasks_array);
        if (completed_serialized)
        {
            output_map->SetString(
                DasReadOnlyString("completedTasks").Get(),
                DasReadOnlyString(completed_serialized->c_str()).Get());
        }

        // stopped 语义：内核 stop 前置短路返回 DAS_E_TIMEOUT；运行中取消由
        // MaaRuntime 置 stopped 标志（das_result 为 DAS_E_FAIL）——两者都算停。
        output_map->SetBool(
            DasReadOnlyString("stopped").Get(),
            run_result.stopped || run_result.das_result == DAS_E_TIMEOUT);

        if (!run_result.diagnostics.empty())
        {
            auto diagnostics_array = Das::Utils::MakeYyjsonArray();
            for (const auto& diagnostic : run_result.diagnostics)
            {
                auto diagnostic_object = Das::Utils::MakeYyjsonObject();
                auto diagnostic_fields = diagnostic_object.as_object();
                (*diagnostic_fields)[std::string_view("severity")] =
                    std::make_pair(
                        std::string_view(diagnostic.severity),
                        yyjson::copy_string);
                (*diagnostic_fields)[std::string_view("code")] = std::make_pair(
                    std::string_view(diagnostic.code),
                    yyjson::copy_string);
                (*diagnostic_fields)[std::string_view("message")] =
                    std::make_pair(
                        std::string_view(diagnostic.message),
                        yyjson::copy_string);
                diagnostics_array.as_array()->emplace_back(
                    std::move(diagnostic_object));
            }
            auto diagnostics_serialized =
                Das::Utils::SerializeYyjsonValue(diagnostics_array);
            if (diagnostics_serialized)
            {
                output_map->SetString(
                    DasReadOnlyString("diagnostics").Get(),
                    DasReadOnlyString(diagnostics_serialized->c_str()).Get());
            }
        }
        else
        {
            output_map->SetString(
                DasReadOnlyString("diagnostics").Get(),
                DasReadOnlyString("[]").Get());
        }

        // D-B5 nextTime 默认输出口：值 = 共享 schedule brain（101 先例延续，
        // 插件自治）算出的下次执行时间，ISO 8601 UTC 定长 19 字符（111-01
        // wire 契约，111-02 adapter 按同 id 捕获，连到 end「下次时间」口）。
        // 本次 Do 刚执行（今日已 fired）→ daily 恒推明日 slot，与 MaapiTask
        // Do→GetNextExecutionTime 序列同语义。engaged-only 纪律（Q4 裁决）：
        // schedule=off / 描述非法 → 不发键，adapter 捕获不到自然回落静态
        // 通道；执行失败不发（成功路径末尾）。
        if (!DAS::IsFailed(execute_result))
        {
            if (auto parsed_envelope = ParseExecutionEnvelope(*envelope_json);
                !DAS::IsFailed(parsed_envelope.result))
            {
                const std::time_t now = now_();
                const auto        next = ComputeNextScheduleRun(
                    parsed_envelope.envelope.maapi.schedule,
                    now,
                    EncodeLocalYmd(now));
                if (next.status == ScheduleNextStatus::Ok)
                {
                    output_map->SetString(
                        DasReadOnlyString("nextTime").Get(),
                        DasReadOnlyString(
                            FormatNextRunIso8601Utc(next.next_unix).c_str())
                            .Get());
                }
            }
        }

        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();

        if (DAS::IsFailed(execute_result))
        {
            return execute_result;
        }

        return DAS_S_OK;
    }

    DasResult MaapiRunTaskComponentFactory::GetGuid(DasGuid* p_out_guid)
    {
        if (p_out_guid == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_out_guid = DasIidOf<MaapiRunTaskComponentFactory>();
        return DAS_S_OK;
    }

    DasResult MaapiRunTaskComponentFactory::GetRuntimeClassName(
        IDasReadOnlyString** pp_out_name)
    {
        if (pp_out_name == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "Das.MaaPi.RunTaskComponentFactory",
            pp_out_name);
    }

    DasResult MaapiRunTaskComponentFactory::CreateComponent(
        const DasGuid&                       component_guid,
        PluginInterface::IDasTaskComponent** pp_out_component)
    {
        if (pp_out_component == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_component = nullptr;
        if (component_guid != DasIidOf<MaapiRunTaskComponent>())
        {
            return DAS_E_NOT_FOUND;
        }

        try
        {
            auto* component = new MaapiRunTaskComponent();
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            return DAS_E_OUT_OF_MEMORY;
        }
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END
