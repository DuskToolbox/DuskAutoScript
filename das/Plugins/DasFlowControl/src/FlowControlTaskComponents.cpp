#define DAS_BUILD_SHARED

#include "FlowControlTaskComponents.h"

#include "FlowControlErrorCodes.h"
#include "FlowControlI18n.h"

#include <das/Core/GraphRuntime/CompiledPlanFingerprint.h>
#include <das/Core/TaskScheduler/RepositoryInvokeDtos.h>
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/IDasSchedulerService.h>
#include <das/Utils/BoostPathBridge.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/PathText.h>
#include <das/Utils/TaskComponentPorts.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasSettingsChangeBuilder.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/header/IDasSettingsChangeBuilder.generated.h>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_BOOST_PROCESS_WARNING

#include <boost/asio/io_context.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/process/v2/stdio.hpp>

DAS_DISABLE_WARNING_END

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <optional>
#include <regex>
#include <utility>

DAS_NS_BEGIN

namespace
{
    struct ComponentSpec
    {
        DasGuid          guid;
        std::string_view guid_text;
        std::string_view kind;
    };

    namespace RepositoryInvokeDto =
        Das::Core::TaskScheduler::RepositoryInvoke::Dto;

    constexpr std::string_view REPOSITORY_INVOKE_KIND =
        "das.flow.invokeRepository";

    // 仓库条目解析缝（测试可注入；空 = 生产实现）。互斥保护：注入与 Do
    // 可能来自不同线程。
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

    // 主进程调度服务的具名注册名（DasHttp 按 das.focus / das.locale 同
    // 范式注册）。
    constexpr std::string_view SCHEDULER_SERVICE_NAME = "das.scheduler";

    // execution 编译请求体（与 RepositoryInvoke 编译链的请求一致）。
    constexpr std::string_view EXECUTION_COMPILE_REQUEST =
        R"({"purpose":"execution"})";

    RepositoryEntryResolveOutcome MakeResolveFailure(std::string error_message)
    {
        RepositoryEntryResolveOutcome outcome;
        outcome.ok = false;
        outcome.error_message = std::move(error_message);
        return outcome;
    }

    // 边界输出投影（子图→父图半，D-21① 单通道）：按子执行快照编译产物
    // 的 boundaryPorts.outputs 清单把 child_output 的类型化口逐口直写父组
    // 件输出（口名=边界口 id，父图数据边直连）——清单是编译期声明的合法
    // 边界口集，保留口（status/outputs/result 等）天然不在其内。快照
    // execution_input 无 {fingerprint, plan} 外壳（maapi/envelope 形态子组
    // 件）时零投影——与 JSON 桥时代行为等同（业务中立）。
    void ProjectChildTypedBoundaryOutputs(
        const RepositoryInvokeDto::ChildExecutionSnapshotDto& snapshot,
        ExportInterface::IDasReadOnlyPortMap*                 child_output,
        ExportInterface::IDasPortMap*                         output_map)
    {
        if (child_output == nullptr || output_map == nullptr
            || snapshot.execution_input.is_null())
        {
            return;
        }
        auto shell_object = snapshot.execution_input.as_object();
        if (!shell_object.has_value()
            || !shell_object->contains(std::string_view("plan")))
        {
            return;
        }
        auto plan_object =
            (*shell_object)[std::string_view("plan")].as_object();
        if (!plan_object.has_value()
            || !plan_object->contains(std::string_view("boundaryPorts")))
        {
            return;
        }
        auto boundary_ports =
            (*plan_object)[std::string_view("boundaryPorts")].as_object();
        if (!boundary_ports.has_value()
            || !boundary_ports->contains(std::string_view("outputs")))
        {
            return;
        }
        auto outputs_array =
            (*boundary_ports)[std::string_view("outputs")].as_array();
        if (!outputs_array.has_value())
        {
            return;
        }
        for (const auto& output_entry : *outputs_array)
        {
            auto output_object = output_entry.as_object();
            if (!output_object.has_value())
            {
                continue;
            }
            const auto port_id =
                (*output_object)[std::string_view("portId")].as_string();
            if (!port_id.has_value() || port_id->empty())
            {
                continue;
            }
            // 缺键/不可转写类型静默跳过——子图未产出该边界值与 JSON 桥时
            // 代的缺键口径同构。
            Das::Utils::CopyPortMapEntry(
                child_output,
                *port_id,
                output_map,
                *port_id);
        }
    }

    // 仓库条目解析的生产实现：经主进程 C ABI 查 IDasSchedulerService，调
    // CompileRepositoryEntryAuthoring 取编译产物 executionInput 与外显的
    // executionComponentGuid，组装子执行快照。跨进程服务访问必须走 C
    // ABI——插件 DLL 内联读取 IPC 上下文的 thread_local 跨 DLL 分裂恒空。
    RepositoryEntryResolveOutcome ResolveRepositoryEntryViaScheduler(
        const RepositoryInvokeDto::RepositoryTaskRefDto& repository_ref)
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
        if (DAS::IsFailed(service_base.As(scheduler.Put())) || !scheduler)
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
                repository_ref.entry_id,
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
        if (!response_object.has_value())
        {
            return MakeResolveFailure(
                "Repository entry compile response is not valid JSON");
        }

        // 失败信封：{ok:false, errorKind, message}。
        if (response_object->contains(std::string_view("ok"))
            && (*response_object)[std::string_view("ok")].is_bool()
            && !(*response_object)[std::string_view("ok")].as_bool().value_or(
                true))
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
                    (*response_object)[std::string_view("message")].as_string())
            {
                error_message = std::string(*message);
            }
            return MakeResolveFailure(error_kind + ": " + error_message);
        }

        if (!response_object->contains(std::string_view("compile")))
        {
            return MakeResolveFailure(
                "Repository entry compile response has no compile section");
        }
        auto compile_object =
            (*response_object)[std::string_view("compile")].as_object();
        if (!compile_object.has_value())
        {
            return MakeResolveFailure(
                "Repository entry compile section is not an object");
        }

        // Provider 编译失败：compile.ok 显式 false。
        if (compile_object->contains(std::string_view("ok"))
            && (*compile_object)[std::string_view("ok")].is_bool()
            && !(*compile_object)[std::string_view("ok")].as_bool().value_or(
                true))
        {
            return MakeResolveFailure(
                "Repository authoring provider failed to compile execution "
                "input");
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

        if (!compile_object->contains(
                std::string_view("executionComponentGuid")))
        {
            return MakeResolveFailure(
                "Repository entry compile did not surface "
                "executionComponentGuid");
        }
        const auto component_guid =
            (*compile_object)[std::string_view("executionComponentGuid")]
                .as_string();
        if (!component_guid.has_value() || component_guid->empty())
        {
            return MakeResolveFailure(
                "Repository entry compile did not surface "
                "executionComponentGuid");
        }

        RepositoryInvokeDto::ChildExecutionSnapshotDto snapshot;
        snapshot.version = 1;
        snapshot.source_entry_id = repository_ref.entry_id;
        snapshot.component_guid = std::string(*component_guid);
        snapshot.execution_input =
            Das::Utils::CloneYyjsonValue(execution_input);

        RepositoryEntryResolveOutcome outcome;
        outcome.ok = true;
        outcome.snapshot = std::move(snapshot);
        return outcome;
    }

    // 解析缝分派：测试注入优先，空则走生产实现。
    RepositoryEntryResolveOutcome ResolveRepositoryEntry(
        const RepositoryInvokeDto::RepositoryTaskRefDto& repository_ref)
    {
        RepositoryEntryResolver resolver;
        {
            const std::scoped_lock lock(RepositoryResolverMutex());
            resolver = RepositoryResolverSlot();
        }
        if (resolver)
        {
            return resolver(repository_ref);
        }
        return ResolveRepositoryEntryViaScheduler(repository_ref);
    }

    constexpr std::string_view BRANCH_KIND = "das.flow.branch";
    constexpr std::string_view DELAY_KIND = "das.flow.delay";
    constexpr std::string_view FOR_KIND = "das.flow.for";
    constexpr std::string_view WHILE_KIND = "das.flow.while";
    constexpr std::string_view MERGE_KIND = "das.flow.merge";
    constexpr std::string_view LAUNCH_KIND = "das.flow.launch";
    constexpr std::string_view COMPARE_DOUBLE_KIND = "das.flow.compare_double";
    constexpr std::string_view COMPARE_INT64_KIND = "das.flow.compare_int64";
    constexpr std::string_view COMPARE_STRING_KIND = "das.flow.compare_string";
    constexpr std::string_view ARITHMETIC_NUMBER_KIND =
        "das.flow.arithmetic_number";
    constexpr std::string_view CONSTANT_BOOL_KIND = "das.flow.constant_bool";
    constexpr std::string_view CONSTANT_INT64_KIND = "das.flow.constant_int64";
    constexpr std::string_view CONSTANT_DOUBLE_KIND =
        "das.flow.constant_double";
    constexpr std::string_view CONSTANT_STRING_KIND =
        "das.flow.constant_string";
    constexpr std::string_view CONCAT_STRING_KIND = "das.flow.concat_string";

    // 等窗轮询的小时间片（stop 只能轮询，切片保证响应性）与超时上限兜底
    // （防病态配置把执行占死）；waitWindowTimeoutMs 缺省用默认值。
    constexpr auto WAIT_WINDOW_POLL_SLICE = std::chrono::milliseconds(100);
    constexpr auto DEFAULT_WAIT_WINDOW_TIMEOUT =
        std::chrono::milliseconds(30000);
    constexpr auto MAX_WAIT_WINDOW_TIMEOUT = std::chrono::milliseconds(120000);

    constexpr std::array<ComponentSpec, 16> COMPONENTS{
        ComponentSpec{
            DasGuid{
                0x68f10001,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},
            "68F10001-0000-4000-8000-000000000001",
            "das.flow.branch"},
        ComponentSpec{
            DasGuid{
                0x68f10003,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03}},
            "68F10003-0000-4000-8000-000000000003",
            "das.flow.delay"},
        ComponentSpec{
            DasGuid{
                0x68f10004,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04}},
            "68F10004-0000-4000-8000-000000000004",
            "das.flow.for"},
        ComponentSpec{
            DasGuid{
                0x68f10005,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05}},
            "68F10005-0000-4000-8000-000000000005",
            "das.flow.while"},
        ComponentSpec{
            DasGuid{
                0x68f10007,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07}},
            "68F10007-0000-4000-8000-000000000007",
            "das.flow.invokeRepository"},
        ComponentSpec{
            DasGuid{
                0x68f10008,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08}},
            "68F10008-0000-4000-8000-000000000008",
            "das.flow.merge"},
        ComponentSpec{
            DasGuid{
                0x68f10009,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09}},
            "68F10009-0000-4000-8000-000000000009",
            "das.flow.constant_bool"},
        ComponentSpec{
            DasGuid{
                0x68f1000a,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a}},
            "68F1000A-0000-4000-8000-00000000000A",
            "das.flow.constant_int64"},
        ComponentSpec{
            DasGuid{
                0x68f1000b,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b}},
            "68F1000B-0000-4000-8000-00000000000B",
            "das.flow.constant_double"},
        ComponentSpec{
            DasGuid{
                0x68f1000c,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c}},
            "68F1000C-0000-4000-8000-00000000000C",
            "das.flow.constant_string"},
        ComponentSpec{
            DasGuid{
                0x68f1000d,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d}},
            "68F1000D-0000-4000-8000-00000000000D",
            "das.flow.compare_int64"},
        ComponentSpec{
            DasGuid{
                0x68f1000e,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0e}},
            "68F1000E-0000-4000-8000-00000000000E",
            "das.flow.compare_double"},
        ComponentSpec{
            DasGuid{
                0x68f1000f,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f}},
            "68F1000F-0000-4000-8000-00000000000F",
            "das.flow.compare_string"},
        ComponentSpec{
            DasGuid{
                0x68f10010,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10}},
            "68F10010-0000-4000-8000-000000000010",
            "das.flow.arithmetic_number"},
        ComponentSpec{
            DasGuid{
                0x68f10011,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11}},
            "68F10011-0000-4000-8000-000000000011",
            "das.flow.concat_string"},
        ComponentSpec{
            DasGuid{
                0x95fd2ffa,
                0x88f2,
                0x4f85,
                {0xa5, 0x36, 0xc7, 0x08, 0xd2, 0x5b, 0x64, 0x1f}},
            "95FD2FFA-88F2-4F85-A536-C708D25B641F",
            "das.flow.launch"},
    };

    std::optional<ComponentSpec> FindSpec(const DasGuid& component_guid)
    {
        for (const auto& spec : COMPONENTS)
        {
            if (spec.guid == component_guid)
            {
                return spec;
            }
        }
        return std::nullopt;
    }

    // PortMap/Json 桥接与结果口 helper 收敛到公共头（das/Utils/
    // TaskComponentPorts.h）——本文件不再持有本地拷贝。
    using Das::Utils::BuildResultPortMap;
    using Das::Utils::CloneJson;
    using Das::Utils::CopyPortMapEntry;
    using Das::Utils::CopyTypedPort;
    using Das::Utils::EmitSignal;
    using Das::Utils::GetPortBool;
    using Das::Utils::GetPortFloat;
    using Das::Utils::GetPortInt;
    using Das::Utils::GetPortJson;
    using Das::Utils::GetPortString;
    using Das::Utils::SetPortFloat;
    using Das::Utils::SetPortInt;
    using Das::Utils::SetPortJson;
    using Das::Utils::SetPortString;
    using Das::Utils::WrapJson;

    std::string StatusFromChild(std::string_view child_status)
    {
        if (child_status == "failed")
        {
            return "failed";
        }
        if (child_status == "cancelled")
        {
            return "cancelled";
        }
        return "completed";
    }

    // ---- das.flow.launch 支撑（通用 exe/窗口原语，业务中立） ----

    // Compile an ECMA-262 regex. std::nullopt for an empty pattern (matches
    // anything) so callers can short-circuit.
    std::optional<std::regex> CompileWindowRegex(std::string_view pattern)
    {
        if (pattern.empty())
        {
            return std::nullopt;
        }
        return std::regex(std::string(pattern));
    }

    bool WindowRegexMatches(
        const std::optional<std::regex>& regex,
        std::string_view                 text)
    {
        if (!regex)
        {
            return true; // 空正则 = 匹配任意
        }
        return std::regex_search(std::string(text), *regex);
    }

    // cpp_yyjson 的 object operator[] 对缺失键抛 out_of_range（mutable 侧
    // 则插入 null 键，读带副作用）；配置键全部可选，统一经下列 helper
    // contains-first 读取（缺键/类型不符 → nullopt）。
    template <typename ObjectRef>
    std::optional<std::string> FindObjectString(
        const ObjectRef& object,
        std::string_view key)
    {
        if (!object.contains(key))
        {
            return std::nullopt;
        }
        if (const auto text = object[key].as_string())
        {
            return std::string(*text);
        }
        return std::nullopt;
    }

    template <typename ObjectRef>
    std::optional<int64_t> FindObjectInt(
        const ObjectRef& object,
        std::string_view key)
    {
        if (!object.contains(key))
        {
            return std::nullopt;
        }
        if (const auto number = object[key].as_int())
        {
            return *number;
        }
        return std::nullopt;
    }

    template <typename ObjectRef>
    auto FindObjectArray(const ObjectRef& object, std::string_view key)
        -> decltype(object[key].as_array())
    {
        if (!object.contains(key))
        {
            return std::nullopt;
        }
        return object[key].as_array();
    }

    template <typename ObjectRef>
    std::optional<double> FindObjectDouble(
        const ObjectRef& object,
        std::string_view key)
    {
        if (!object.contains(key))
        {
            return std::nullopt;
        }
        if (const auto number = object[key].as_num())
        {
            return *number;
        }
        return std::nullopt;
    }

    template <typename ObjectRef>
    std::optional<bool> FindObjectBool(
        const ObjectRef& object,
        std::string_view key)
    {
        if (!object.contains(key))
        {
            return std::nullopt;
        }
        if (const auto flag = object[key].as_bool())
        {
            return *flag;
        }
        return std::nullopt;
    }

    std::string_view LaunchDiagnosticFallbackText(
        std::string_view diagnostic_code)
    {
        if (diagnostic_code == "launch-window-timeout")
        {
            return "Timed out waiting for the target window to appear.";
        }
        if (diagnostic_code == "launch-window-ambiguous")
        {
            return "Multiple windows matched the wait configuration; refine "
                   "the match pattern.";
        }
        if (diagnostic_code == "launch-spawn-failed")
        {
            return "Failed to start the configured executable.";
        }
        if (diagnostic_code == "launch-exe-path-missing")
        {
            return "No executable path is configured for the launch "
                   "component.";
        }
        return "";
    }

    // 失败出口：尽力构建结构化 diagnostic 载荷（code + i18n 消息，供直驱
    // 调用方冗余显示），并回传执行失败错误码——GraphRuntime 失败接管路径
    // 据此终止图并携带 node id 冒泡。
    DasResult BuildLaunchFailedPortMap(
        std::string_view               diagnostic_code,
        DasResult                      error_code,
        ExportInterface::IDasPortMap** pp_out_port_map)
    {
        DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
        const DasResult hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }

        SetPortString(output_map.Get(), "status", "failed");

        const std::string i18n_key =
            std::string{"flow.diagnostic."} + std::string{diagnostic_code};
        const std::string message_text =
            Plugins::DasFlowControl::TranslateFlowControlKey(
                i18n_key.c_str(),
                LaunchDiagnosticFallbackText(diagnostic_code));

        auto diagnostic = Das::Utils::MakeYyjsonObject();
        auto diagnostic_object = *diagnostic.as_object();
        diagnostic_object[std::string_view("code")] =
            yyjson::value(std::string{diagnostic_code});
        diagnostic_object[std::string_view("message")] =
            yyjson::value(std::move(message_text));
        SetPortJson(output_map.Get(), "diagnostic", diagnostic);

        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();
        return error_code;
    }

#ifdef _WIN32
    std::string WideToUtf8(std::wstring_view text)
    {
        if (text.empty())
        {
            return {};
        }
        const int size_needed = WideCharToMultiByte(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (size_needed <= 0)
        {
            return {};
        }
        std::string result(size_needed, '\0');
        WideCharToMultiByte(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            result.data(),
            size_needed,
            nullptr,
            nullptr);
        return result;
    }

    BOOL CALLBACK CollectVisibleWindowProc(HWND hwnd, LPARAM l_param)
    {
        auto* windows =
            reinterpret_cast<std::vector<FlowControlWindowInfo>*>(l_param);
        if (IsWindowVisible(hwnd))
        {
            wchar_t   class_name_buffer[256] = {};
            const int class_name_length =
                GetClassNameW(hwnd, class_name_buffer, 256);
            wchar_t title_buffer[256] = {};
            // GetWindowTextW 跨进程返回缓存标题、GetClassNameW 不发消息，
            // 轮询无 hang 面。
            const int title_length = GetWindowTextW(hwnd, title_buffer, 256);

            FlowControlWindowInfo info;
            info.handle = hwnd;
            info.class_name = WideToUtf8(
                std::wstring_view{
                    class_name_buffer,
                    class_name_length > 0
                        ? static_cast<size_t>(class_name_length)
                        : 0});
            info.window_name = WideToUtf8(
                std::wstring_view{
                    title_buffer,
                    title_length > 0 ? static_cast<size_t>(title_length) : 0});
            windows->emplace_back(std::move(info));
        }
        return TRUE;
    }
#endif

    // 生产进程启动缝：boost::process v2 spawn，成功后立即 detach——
    // basic_process 析构 terminate_if_running，不 detach 则组件 Do 返回即
    // 杀子进程。stdio 全部丢弃（启动器不消费子进程输出）。
    FlowControlProcessSpawner DefaultProcessSpawner()
    {
        return [](const std::string&              exe_path,
                  const std::vector<std::string>& arguments,
                  const std::string&              working_directory) -> bool
        {
            try
            {
                // bp2 的 exe 与 start_dir 形参是 boost path：窄串经 ACP
                // 构造会双损（中文路径下启动错位），全程保 path 进出；
                // arguments 形参是窄 UTF-8 通道，保持直传。
                boost::asio::io_context io_context;
                if (working_directory.empty())
                {
                    boost::process::v2::process child(
                        io_context,
                        Das::Utils::ToBoostPath(
                            Das::Utils::PathFromUtf8(exe_path)),
                        arguments,
                        boost::process::v2::process_stdio{
                            {},
                            nullptr,
                            nullptr});
                    (void)child.detach();
                    return true;
                }
                boost::process::v2::process child(
                    io_context,
                    Das::Utils::ToBoostPath(Das::Utils::PathFromUtf8(exe_path)),
                    arguments,
                    boost::process::v2::process_start_dir(
                        Das::Utils::ToBoostPath(
                            Das::Utils::PathFromUtf8(working_directory))),
                    boost::process::v2::process_stdio{{}, nullptr, nullptr});
                (void)child.detach();
                return true;
            }
            catch (...)
            {
                return false;
            }
        };
    }

    // 生产窗口枚举缝：全桌可见顶层窗口（类名 + 缓存标题）。
    FlowControlWindowEnumerator DefaultWindowEnumerator()
    {
        return []() -> std::vector<FlowControlWindowInfo>
        {
#ifdef _WIN32
            std::vector<FlowControlWindowInfo> windows;
            EnumWindows(
                CollectVisibleWindowProc,
                reinterpret_cast<LPARAM>(&windows));
            return windows;
#else
            // 插件 manifest 声明 supportedSystem=Windows；非 Windows 仅保
            // 证可编译（枚举恒空 → 等窗口路径按超时失败）。
            return {};
#endif
        };
    }

} // namespace

void SetRepositoryEntryResolverForTest(RepositoryEntryResolver resolver)
{
    const std::scoped_lock lock(RepositoryResolverMutex());
    RepositoryResolverSlot() = std::move(resolver);
}

DasFlowControlTaskComponent::DasFlowControlTaskComponent(
    std::string_view                               kind,
    DasPtr<PluginInterface::IDasTaskComponentHost> host)
    : kind_(kind), settings_(Das::Utils::MakeYyjsonObject()),
      host_(std::move(host))
{
}

DasResult DasFlowControlTaskComponent::GetGuid(DasGuid* p_out_guid)
{
    if (p_out_guid == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *p_out_guid = DasIidOf<DasFlowControlTaskComponent>();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::GetRuntimeClassName(
    IDasReadOnlyString** pp_out_name)
{
    if (pp_out_name == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    return CreateIDasReadOnlyStringFromUtf8(
        "Das.FlowControlTaskComponent",
        pp_out_name);
}

DasResult DasFlowControlTaskComponent::ApplySettingsChange(
    ExportInterface::IDasJson*  p_request_json,
    ExportInterface::IDasJson** pp_out_result_json)
{
    if (pp_out_result_json == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    yyjson::value accepted = CloneJson(settings_);
    if (p_request_json != nullptr)
    {
        DasPtr<IDasReadOnlyString> request_string;
        if (DAS::IsOk(p_request_json->ToString(0, request_string.Put()))
            && request_string)
        {
            const char* request_u8 = nullptr;
            request_string->GetUtf8(&request_u8);
            auto parsed = Das::Utils::ParseYyjsonFromString(
                request_u8 != nullptr ? request_u8 : "");
            if (parsed && parsed->is_object())
            {
                auto request_obj = *parsed->as_object();
                if (request_obj.contains(std::string_view("settings")))
                {
                    // GraphRuntime 投递的 {"settings": S, "payload": P} 包装：
                    // compiled_settings 是节点当前配置的全量投影——替换语义
                    // 而非逐键合并（每次投递即节点当前配置全量）。
                    // settings 为 null 时保持现值。
                    const auto& inner =
                        request_obj[std::string_view("settings")];
                    if (!inner.is_null())
                    {
                        accepted = CloneJson(inner);
                    }
                }
                else
                {
                    auto kind =
                        request_obj[std::string_view("kind")].as_string();
                    if (kind && *kind == "setValue")
                    {
                        auto payload = request_obj[std::string_view("payload")]
                                           .as_object();
                        if (payload)
                        {
                            auto key = (*payload)[std::string_view("path")]
                                           .as_string();
                            if (key
                                && payload->contains(std::string_view("value")))
                            {
                                (*accepted
                                      .as_object())[std::string_view(*key)] =
                                    CloneJson(
                                        (*payload)[std::string_view("value")]);
                            }
                        }
                    }
                }
            }
        }
    }

    // invokeRepository 的 repositoryRef：正整数 entryId，其余形状整包拒绝
    // （不落半截状态——项目未发布，无 legacy 值需要宽容）。
    std::optional<int64_t> accepted_repository_entry_id;
    if (kind_ == REPOSITORY_INVOKE_KIND)
    {
        auto accepted_object = accepted.as_object();
        if (accepted_object.has_value()
            && accepted_object->contains(std::string_view("repositoryRef")))
        {
            const auto& repository_ref =
                (*accepted_object)[std::string_view("repositoryRef")];
            if (!repository_ref.is_null())
            {
                const auto entry_id = repository_ref.as_int();
                if (!entry_id.has_value() || *entry_id <= 0)
                {
                    return DAS_E_INVALID_ARGUMENT;
                }
                accepted_repository_entry_id = *entry_id;
            }
        }
    }

    settings_ = CloneJson(accepted);

    {
        const std::scoped_lock repository_entry_lock(repository_entry_mutex_);
        repository_entry_id_ = accepted_repository_entry_id.value_or(0);
    }

    // acceptedSettings 响应经协议 builder 组装：固定 wire 字段名进方法名，
    // 调用方拼错字段在编译期报错。
    Das::DasPtr<Das::ExportInterface::IDasSettingsChangeBuilder> builder;
    DasResult hr = CreateIDasSettingsChangeBuilder(builder.Put());
    if (DAS::IsFailed(hr) || !builder)
    {
        return DAS::IsFailed(hr) ? hr : DAS_E_FAIL;
    }

    auto accepted_json = WrapJson(CloneJson(accepted));
    if (!accepted_json)
    {
        return DAS_E_INVALID_JSON;
    }
    hr = builder->SetAcceptedSettings(accepted_json.Get());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    return builder->ToJson(pp_out_result_json);
}

// 两层失败语义：快照解析/子组件创建失败 = 无法完成调用动作 → DAS_E_FLOW_
// INVOKE_* 执行失败错误码（GraphRuntime 失败接管路径终止图）；子任务自身
// 成败 = 业务结果 → status/childStatus 端口、返回成功码。
DasResult DasFlowControlTaskComponent::DoRepositoryInvoke(
    PluginInterface::IDasStopToken*       stop_token,
    ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
    ExportInterface::IDasPortMap**        pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    bool stop_requested = false;
    if (stop_token != nullptr
        && DAS::IsOk(stop_token->StopRequested(&stop_requested))
        && stop_requested)
    {
        return BuildResultPortMap(
            "cancelled",
            Das::Utils::MakeYyjsonObject(),
            Das::Utils::MakeYyjsonArray(),
            pp_out_port_map);
    }

    // 快照来源分派：compiledSnapshot 端口喂线优先（既有路径零变化）；端口
    // 缺失且 settings 携带有效 repositoryRef 时自解析仓库条目（调用节点
    // 执行路径）。
    auto snapshot_json = GetPortJson(p_input_port_map, "compiledSnapshot");
    RepositoryInvokeDto::ChildExecutionSnapshotDto snapshot;
    if (snapshot_json && snapshot_json->is_object())
    {
        try
        {
            snapshot =
                yyjson::cast<RepositoryInvokeDto::ChildExecutionSnapshotDto>(
                    *snapshot_json);
        }
        catch (const std::exception&)
        {
            DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
            const DasResult hr = CreateIDasPortMap(output_map.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            SetPortString(output_map.Get(), "status", "failed");
            SetPortString(
                output_map.Get(),
                "diagnostic",
                "Compiled child snapshot JSON is not valid.");
            *pp_out_port_map = output_map.Get();
            output_map.Get()->AddRef();
            return Plugins::DasFlowControl::DAS_E_FLOW_INVOKE_SNAPSHOT_INVALID;
        }
    }
    else
    {
        int64_t repository_entry_id = 0;
        {
            const std::scoped_lock repository_entry_lock(
                repository_entry_mutex_);
            repository_entry_id = repository_entry_id_;
        }
        if (repository_entry_id <= 0)
        {
            // Return a result with diagnostic information
            DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
            const DasResult hr = CreateIDasPortMap(output_map.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            SetPortString(output_map.Get(), "status", "failed");
            SetPortString(
                output_map.Get(),
                "diagnostic",
                "Repository invoke requires a compiled child snapshot.");
            *pp_out_port_map = output_map.Get();
            output_map.Get()->AddRef();
            return Plugins::DasFlowControl::DAS_E_FLOW_INVOKE_SNAPSHOT_MISSING;
        }

        RepositoryInvokeDto::RepositoryTaskRefDto repository_ref;
        repository_ref.entry_id = repository_entry_id;
        // expected_revision/source_fingerprint 恒不填——调用节点无版本
        // 对账概念，条目解析端的 optional 校验天然跳过。
        const auto resolve_outcome = ResolveRepositoryEntry(repository_ref);
        if (!resolve_outcome.ok)
        {
            DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
            const DasResult hr = CreateIDasPortMap(output_map.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            SetPortString(output_map.Get(), "status", "failed");
            SetPortString(
                output_map.Get(),
                "diagnostic",
                "Repository entry resolve failed: "
                    + resolve_outcome.error_message);
            *pp_out_port_map = output_map.Get();
            output_map.Get()->AddRef();
            return Plugins::DasFlowControl::DAS_E_FLOW_INVOKE_RESOLVE_FAILED;
        }
        snapshot = std::move(resolve_outcome.snapshot);
    }

    if (snapshot.version != 1)
    {
        DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
        const DasResult hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }
        SetPortString(output_map.Get(), "status", "failed");
        SetPortString(
            output_map.Get(),
            "diagnostic",
            "Compiled child snapshot version is not supported.");
        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();
        return Plugins::DasFlowControl::
            DAS_E_FLOW_INVOKE_SNAPSHOT_VERSION_UNSUPPORTED;
    }

    if (!host_)
    {
        DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
        const DasResult hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }
        SetPortString(output_map.Get(), "status", "failed");
        SetPortString(
            output_map.Get(),
            "diagnostic",
            "Task component host is unavailable.");
        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();
        return Plugins::DasFlowControl::DAS_E_FLOW_INVOKE_HOST_UNAVAILABLE;
    }

    DasGuid    child_component_guid{};
    const auto guid_result =
        DasMakeDasGuid(snapshot.component_guid.c_str(), &child_component_guid);
    if (DAS::IsFailed(guid_result))
    {
        DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
        const DasResult hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }
        SetPortString(output_map.Get(), "status", "failed");
        SetPortString(
            output_map.Get(),
            "diagnostic",
            "Compiled child snapshot component GUID is invalid.");
        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();
        return Plugins::DasFlowControl::DAS_E_FLOW_INVOKE_CHILD_GUID_INVALID;
    }

    DasPtr<PluginInterface::IDasTaskComponent> child_component;
    const auto                                 create_result =
        host_->CreateTaskComponent(child_component_guid, child_component.Put());
    if (DAS::IsFailed(create_result) || !child_component)
    {
        DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
        const DasResult hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }
        SetPortString(output_map.Get(), "status", "failed");
        SetPortString(
            output_map.Get(),
            "diagnostic",
            "Child task component could not be created.");
        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();
        return Plugins::DasFlowControl::DAS_E_FLOW_INVOKE_CHILD_CREATE_FAILED;
    }

    // Build child input PortMap from snapshot execution_input
    DAS::DasPtr<ExportInterface::IDasPortMap> child_input;
    DasResult hr = CreateIDasPortMap(child_input.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }

    if (!snapshot.execution_input.is_null())
    {
        auto input_serialized =
            Das::Utils::SerializeYyjsonValue(snapshot.execution_input);
        if (input_serialized)
        {
            SetPortString(
                child_input.Get(),
                "executionInput",
                *input_serialized);
        }
    }

    // 边界输入类型化直塞（调用节点值流的父图→子图半，D-21② 单通道）：
    // 父图 input map 全部条目原键拷贝进子输入（键=边界口稳定 id）——
    // child 侧经 GraphRuntime 直传覆盖层物化，plan 根本不被改动（完整性
    // 门自洽，无 patch 无指纹重算）。executionInput 是本段刚写的装配串
    // 口，跳过防覆盖。
    if (p_input_port_map != nullptr)
    {
        DasPtr<ExportInterface::IDasStringVector> parent_keys;
        if (DAS::IsOk(p_input_port_map->GetKeys(parent_keys.Put()))
            && parent_keys)
        {
            uint64_t parent_key_count = 0;
            if (DAS::IsOk(parent_keys->Size(&parent_key_count)))
            {
                for (uint64_t key_index = 0; key_index < parent_key_count;
                     ++key_index)
                {
                    IDasReadOnlyString* key_string = nullptr;
                    if (DAS::IsOk(parent_keys->At(key_index, &key_string))
                        && key_string != nullptr)
                    {
                        const char* key_utf8 = nullptr;
                        key_string->GetUtf8(&key_utf8);
                        const std::string port_name(
                            key_utf8 != nullptr ? key_utf8 : "");
                        key_string->Release();
                        if (port_name.empty() || port_name == "executionInput")
                        {
                            continue;
                        }
                        CopyPortMapEntry(
                            p_input_port_map,
                            port_name,
                            child_input.Get(),
                            port_name);
                    }
                }
            }
        }
    }

    // Call child Do() with PortMap
    DAS::DasPtr<ExportInterface::IDasPortMap> child_output;
    hr = child_component->Do(stop_token, child_input.Get(), child_output.Put());
    if (DAS::IsFailed(hr) || !child_output)
    {
        DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
        const DasResult err_hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(err_hr))
        {
            return err_hr;
        }
        SetPortString(output_map.Get(), "status", "failed");
        SetPortString(
            output_map.Get(),
            "diagnostic",
            "Child task component returned a failure result.");
        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();
        return DAS_S_OK;
    }

    // Extract child status from output PortMap. status 是 raw string port
    // （component 用 SetPortString 写），用 GetPortString 直接读取，不能用
    // GetPortJson（会把裸字符串当 JSON 解析而失败，导致 child 失败被误报成
    // completed）。
    const std::string child_status =
        GetPortString(child_output.Get(), "status", "completed");

    auto child_outputs_json = GetPortJson(child_output.Get(), "outputs");
    if (!child_outputs_json)
    {
        child_outputs_json = Das::Utils::MakeYyjsonObject();
    }

    const std::string status = StatusFromChild(child_status);

    DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
    hr = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }

    SetPortString(output_map.Get(), "status", status);
    SetPortString(output_map.Get(), "childStatus", child_status);
    SetPortJson(output_map.Get(), "childOutputs", *child_outputs_json);

    // 边界输出投影（子图→父图半，D-21① 单通道）：child 的类型化边界口按
    // 快照编译产物清单逐口直写自身输出（口名=边界口 id，父图数据边直
    // 连；对象引用直达——JSON 桥的 graphOutputs 解析段已退役）。
    ProjectChildTypedBoundaryOutputs(
        snapshot,
        child_output.Get(),
        output_map.Get());

    *pp_out_port_map = output_map.Get();
    output_map.Get()->AddRef();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::DoBranch(
    PluginInterface::IDasStopToken*       stop_token,
    ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
    ExportInterface::IDasPortMap**        pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    bool stop_requested = false;
    if (stop_token != nullptr
        && DAS::IsOk(stop_token->StopRequested(&stop_requested))
        && stop_requested)
    {
        return BuildResultPortMap(
            "cancelled",
            Das::Utils::MakeYyjsonObject(),
            Das::Utils::MakeYyjsonArray(),
            pp_out_port_map);
    }

    // 纯 signal 路由：读 condition → emit true/false。runtime 据 signal_routes
    // gate 分支；组件不感知自己因哪个 signal 被激活（DAS-60 Stage 4）。
    // 内嵌 compiledSnapshot 子图模型已移除，分支体是主图独立节点。
    const bool condition =
        p_input_port_map ? GetPortBool(p_input_port_map, "condition", false)
                         : false;
    const std::string_view selected = condition ? "true" : "false";

    auto outputs = Das::Utils::MakeYyjsonObject();
    (*outputs.as_object())[std::string_view("selected")] = selected;
    auto signals = Das::Utils::MakeYyjsonArray();
    (*signals.as_array()).emplace_back(selected);

    return BuildResultPortMap("completed", outputs, signals, pp_out_port_map);
}

DasResult DasFlowControlTaskComponent::DoFor(
    PluginInterface::IDasStopToken*       stop_token,
    ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
    ExportInterface::IDasPortMap**        pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    bool stop_requested = false;
    if (stop_token != nullptr
        && DAS::IsOk(stop_token->StopRequested(&stop_requested))
        && stop_requested)
    {
        DAS::DasPtr<ExportInterface::IDasPortMap> cancelled_map;
        const DasResult hr = CreateIDasPortMap(cancelled_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }
        SetPortString(cancelled_map.Get(), "status", "cancelled");
        *pp_out_port_map = cancelled_map.Get();
        cancelled_map.Get()->AddRef();
        return DAS_S_OK;
    }

    // 循环参数走 input port（binding 装配：settings 默认值或上游 data edge）。
    const int64_t start =
        p_input_port_map ? GetPortInt(p_input_port_map, "start", 0) : 0;
    const int64_t end =
        p_input_port_map ? GetPortInt(p_input_port_map, "end", 0) : 0;
    int64_t step =
        p_input_port_map ? GetPortInt(p_input_port_map, "step", 1) : 1;
    if (step == 0)
    {
        step = 1; // 0 步长无意义且会死循环，兜底为 1。
    }

    // 计数状态入帧：index 输入口经编译器合成的自环绑定携带上圈输出（输
    // 出写本圈序号，下圈读回即 +step 推进）。首轮缺键回落 start-step 使
    // 首圈输出恰为 start；外层环清场删掉帧值后同样回落——嵌套外圈重启
    // 时计数自动从头开始，组件自身无跨 Do 状态。
    const int64_t current =
        p_input_port_map
            ? GetPortInt(p_input_port_map, "index", start - step) + step
            : start;

    DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
    const DasResult hr = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    SetPortString(output_map.Get(), "status", "completed");

    const bool in_range = (step > 0) ? (current < end) : (current > end);
    if (in_range)
    {
        // 写循环变量 index（data 输出端口，循环体经 binding 读取），emit
        // continue 激活循环体；循环体完成后 back-edge 回到 for 重新 Do()。
        SetPortInt(output_map.Get(), "index", current);
        EmitSignal(output_map.Get(), "continue");
    }
    else
    {
        // 范围耗尽，emit break 退出循环（不再激活循环体）。
        EmitSignal(output_map.Get(), "break");
    }

    *pp_out_port_map = output_map.Get();
    output_map.Get()->AddRef();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::DoWhile(
    PluginInterface::IDasStopToken*       stop_token,
    ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
    ExportInterface::IDasPortMap**        pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    bool stop_requested = false;
    if (stop_token != nullptr
        && DAS::IsOk(stop_token->StopRequested(&stop_requested))
        && stop_requested)
    {
        DAS::DasPtr<ExportInterface::IDasPortMap> cancelled_map;
        const DasResult hr = CreateIDasPortMap(cancelled_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }
        SetPortString(cancelled_map.Get(), "status", "cancelled");
        *pp_out_port_map = cancelled_map.Get();
        cancelled_map.Get()->AddRef();
        return DAS_S_OK;
    }

    // 每轮读动态 condition（由循环体/外部每轮回边后重算并写入 PortFrame）；
    // 第一圈回边尚无值 → 回落 first_condition 口（缺省 true）承载启动条件。
    const bool first_condition =
        p_input_port_map
            ? GetPortBool(p_input_port_map, "first_condition", true)
            : true;
    const bool condition =
        p_input_port_map
            ? GetPortBool(p_input_port_map, "condition", first_condition)
            : first_condition;

    DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
    const DasResult hr = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    SetPortString(output_map.Get(), "status", "completed");
    EmitSignal(output_map.Get(), condition ? "continue" : "break");

    *pp_out_port_map = output_map.Get();
    output_map.Get()->AddRef();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::DoCompareDouble(
    PluginInterface::IDasStopToken*       stop_token,
    ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
    ExportInterface::IDasPortMap**        pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // 纯数据原语：无子调用无等待，不轮询取消。
    (void)stop_token;

    std::string op = "<";
    double      left_default = 0.0;
    double      right_default = 3.0;
    if (auto settings_obj = settings_.as_object())
    {
        if (const auto v = FindObjectString(*settings_obj, "operator"))
        {
            op = *v;
        }
        if (const auto v = FindObjectDouble(*settings_obj, "left"))
        {
            left_default = *v;
        }
        if (const auto v = FindObjectDouble(*settings_obj, "right"))
        {
            right_default = *v;
        }
    }

    // 悬空口回落面板默认值（缺省即语义）：连线则动态、不连则阈值填面板。
    const double left =
        p_input_port_map ? GetPortFloat(p_input_port_map, "left", left_default)
                         : left_default;
    const double right =
        p_input_port_map
            ? GetPortFloat(p_input_port_map, "right", right_default)
            : right_default;

    bool result = false;
    if (op == "<")
    {
        result = left < right;
    }
    else if (op == "<=")
    {
        result = left <= right;
    }
    else if (op == ">")
    {
        result = left > right;
    }
    else if (op == ">=")
    {
        result = left >= right;
    }
    else if (op == "==")
    {
        result = left == right;
    }
    else if (op == "!=")
    {
        result = left != right;
    }
    else
    {
        // 运算符是行为不是口面类型：值域外显性失败，不静默选一个。
        return Plugins::DasFlowControl::DAS_E_FLOW_OPERATOR_INVALID;
    }

    DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
    const DasResult hr = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    SetPortString(output_map.Get(), "status", "completed");
    const DasReadOnlyString result_key{"result"};
    output_map.Get()->SetBool(result_key.Get(), result);
    EmitSignal(output_map.Get(), "signal_out");

    *pp_out_port_map = output_map.Get();
    output_map.Get()->AddRef();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::DoCompareInt64(
    PluginInterface::IDasStopToken*       stop_token,
    ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
    ExportInterface::IDasPortMap**        pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // 纯数据原语：无子调用无等待，不轮询取消。
    (void)stop_token;

    std::string op = "<";
    int64_t     left_default = 0;
    int64_t     right_default = 0;
    if (auto settings_obj = settings_.as_object())
    {
        if (const auto v = FindObjectString(*settings_obj, "operator"))
        {
            op = *v;
        }
        if (const auto v = FindObjectInt(*settings_obj, "left"))
        {
            left_default = *v;
        }
        if (const auto v = FindObjectInt(*settings_obj, "right"))
        {
            right_default = *v;
        }
    }

    // 悬空口回落面板默认值（缺省即语义）：连线则动态、不连则阈值填面板。
    const int64_t left =
        p_input_port_map ? GetPortInt(p_input_port_map, "left", left_default)
                         : left_default;
    const int64_t right =
        p_input_port_map ? GetPortInt(p_input_port_map, "right", right_default)
                         : right_default;

    bool result = false;
    if (op == "<")
    {
        result = left < right;
    }
    else if (op == "<=")
    {
        result = left <= right;
    }
    else if (op == ">")
    {
        result = left > right;
    }
    else if (op == ">=")
    {
        result = left >= right;
    }
    else if (op == "==")
    {
        result = left == right;
    }
    else if (op == "!=")
    {
        result = left != right;
    }
    else
    {
        // 运算符是行为不是口面类型：值域外显性失败，不静默选一个。
        return Plugins::DasFlowControl::DAS_E_FLOW_OPERATOR_INVALID;
    }

    DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
    const DasResult hr = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    SetPortString(output_map.Get(), "status", "completed");
    const DasReadOnlyString result_key{"result"};
    output_map.Get()->SetBool(result_key.Get(), result);
    EmitSignal(output_map.Get(), "signal_out");

    *pp_out_port_map = output_map.Get();
    output_map.Get()->AddRef();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::DoCompareString(
    PluginInterface::IDasStopToken*       stop_token,
    ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
    ExportInterface::IDasPortMap**        pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // 纯数据原语：无子调用无等待，不轮询取消。
    (void)stop_token;

    std::string op = "==";
    std::string left_default;
    std::string right_default;
    if (auto settings_obj = settings_.as_object())
    {
        if (const auto v = FindObjectString(*settings_obj, "operator"))
        {
            op = *v;
        }
        if (const auto v = FindObjectString(*settings_obj, "left"))
        {
            left_default = *v;
        }
        if (const auto v = FindObjectString(*settings_obj, "right"))
        {
            right_default = *v;
        }
    }

    // 悬空口回落面板默认值（缺省即语义）：连线则动态、不连则阈值填面板。
    const std::string left =
        p_input_port_map ? GetPortString(p_input_port_map, "left", left_default)
                         : left_default;
    const std::string right =
        p_input_port_map
            ? GetPortString(p_input_port_map, "right", right_default)
            : right_default;

    // 字符串的运算符值域只含等值/不等值：序比较对字符串无业务定义，
    // 越域显性失败（比较语义归比较节点，值域单源在 Do 内校验）。
    bool result = false;
    if (op == "==")
    {
        result = left == right;
    }
    else if (op == "!=")
    {
        result = left != right;
    }
    else
    {
        return Plugins::DasFlowControl::DAS_E_FLOW_OPERATOR_INVALID;
    }

    DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
    const DasResult hr = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    SetPortString(output_map.Get(), "status", "completed");
    const DasReadOnlyString result_key{"result"};
    output_map.Get()->SetBool(result_key.Get(), result);
    EmitSignal(output_map.Get(), "signal_out");

    *pp_out_port_map = output_map.Get();
    output_map.Get()->AddRef();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::DoArithmeticNumber(
    PluginInterface::IDasStopToken*       stop_token,
    ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
    ExportInterface::IDasPortMap**        pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // 纯数据原语：无子调用无等待，不轮询取消。
    (void)stop_token;

    std::string op = "+";
    double      left_default = 0.0;
    double      right_default = 1.0;
    if (auto settings_obj = settings_.as_object())
    {
        if (const auto v = FindObjectString(*settings_obj, "operator"))
        {
            op = *v;
        }
        if (const auto v = FindObjectDouble(*settings_obj, "left"))
        {
            left_default = *v;
        }
        if (const auto v = FindObjectDouble(*settings_obj, "right"))
        {
            right_default = *v;
        }
    }

    const double left =
        p_input_port_map ? GetPortFloat(p_input_port_map, "left", left_default)
                         : left_default;
    const double right =
        p_input_port_map
            ? GetPortFloat(p_input_port_map, "right", right_default)
            : right_default;

    double result = 0.0;
    if (op == "+")
    {
        result = left + right;
    }
    else if (op == "-")
    {
        result = left - right;
    }
    else if (op == "*")
    {
        result = left * right;
    }
    else if (op == "/")
    {
        if (right == 0.0)
        {
            // 除零显性失败：静默映射到 inf 会把配置错误伪装成数据。
            return Plugins::DasFlowControl::DAS_E_FLOW_DIVIDE_BY_ZERO;
        }
        result = left / right;
    }
    else
    {
        return Plugins::DasFlowControl::DAS_E_FLOW_OPERATOR_INVALID;
    }

    DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
    const DasResult hr = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    SetPortString(output_map.Get(), "status", "completed");
    SetPortFloat(output_map.Get(), "result", result);
    EmitSignal(output_map.Get(), "signal_out");

    *pp_out_port_map = output_map.Get();
    output_map.Get()->AddRef();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::DoConstantBool(
    PluginInterface::IDasStopToken* stop_token,
    ExportInterface::IDasReadOnlyPortMap* /*p_input_port_map*/,
    ExportInterface::IDasPortMap** pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // 纯数据原语：无子调用无等待，不轮询取消。
    (void)stop_token;

    // config 缺 value 键回落零值（缺省即语义）。
    bool value = false;
    if (auto settings_obj = settings_.as_object())
    {
        if (const auto v = FindObjectBool(*settings_obj, "value"))
        {
            value = *v;
        }
    }

    DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
    const DasResult hr = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    SetPortString(output_map.Get(), "status", "completed");
    const DasReadOnlyString value_key{"value"};
    output_map.Get()->SetBool(value_key.Get(), value);
    EmitSignal(output_map.Get(), "signal_out");

    *pp_out_port_map = output_map.Get();
    output_map.Get()->AddRef();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::DoConstantInt64(
    PluginInterface::IDasStopToken* stop_token,
    ExportInterface::IDasReadOnlyPortMap* /*p_input_port_map*/,
    ExportInterface::IDasPortMap** pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // 纯数据原语：无子调用无等待，不轮询取消。
    (void)stop_token;

    // config 缺 value 键回落零值（缺省即语义）。
    int64_t value = 0;
    if (auto settings_obj = settings_.as_object())
    {
        if (const auto v = FindObjectInt(*settings_obj, "value"))
        {
            value = *v;
        }
    }

    DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
    const DasResult hr = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    SetPortString(output_map.Get(), "status", "completed");
    SetPortInt(output_map.Get(), "value", value);
    EmitSignal(output_map.Get(), "signal_out");

    *pp_out_port_map = output_map.Get();
    output_map.Get()->AddRef();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::DoConstantDouble(
    PluginInterface::IDasStopToken* stop_token,
    ExportInterface::IDasReadOnlyPortMap* /*p_input_port_map*/,
    ExportInterface::IDasPortMap** pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // 纯数据原语：无子调用无等待，不轮询取消。
    (void)stop_token;

    // config 缺 value 键回落零值（缺省即语义）。
    double value = 0.0;
    if (auto settings_obj = settings_.as_object())
    {
        if (const auto v = FindObjectDouble(*settings_obj, "value"))
        {
            value = *v;
        }
    }

    DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
    const DasResult hr = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    SetPortString(output_map.Get(), "status", "completed");
    SetPortFloat(output_map.Get(), "value", value);
    EmitSignal(output_map.Get(), "signal_out");

    *pp_out_port_map = output_map.Get();
    output_map.Get()->AddRef();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::DoConstantString(
    PluginInterface::IDasStopToken* stop_token,
    ExportInterface::IDasReadOnlyPortMap* /*p_input_port_map*/,
    ExportInterface::IDasPortMap** pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // 纯数据原语：无子调用无等待，不轮询取消。
    (void)stop_token;

    // config 缺 value 键回落空串（缺省即语义）。
    std::string value;
    if (auto settings_obj = settings_.as_object())
    {
        if (const auto v = FindObjectString(*settings_obj, "value"))
        {
            value = *v;
        }
    }

    DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
    const DasResult hr = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    SetPortString(output_map.Get(), "status", "completed");
    SetPortString(output_map.Get(), "value", value);
    EmitSignal(output_map.Get(), "signal_out");

    *pp_out_port_map = output_map.Get();
    output_map.Get()->AddRef();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::DoConcatString(
    PluginInterface::IDasStopToken*       stop_token,
    ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
    ExportInterface::IDasPortMap**        pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // 纯数据原语：无子调用无等待，不轮询取消。
    (void)stop_token;

    std::string left_default;
    std::string right_default;
    if (auto settings_obj = settings_.as_object())
    {
        if (const auto v = FindObjectString(*settings_obj, "left"))
        {
            left_default = *v;
        }
        if (const auto v = FindObjectString(*settings_obj, "right"))
        {
            right_default = *v;
        }
    }

    // 悬空口回落面板默认值（缺省即语义）：连线则动态、不连则默认。
    const std::string left =
        p_input_port_map ? GetPortString(p_input_port_map, "left", left_default)
                         : left_default;
    const std::string right =
        p_input_port_map
            ? GetPortString(p_input_port_map, "right", right_default)
            : right_default;

    DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
    const DasResult hr = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    SetPortString(output_map.Get(), "status", "completed");
    SetPortString(output_map.Get(), "result", left + right);
    EmitSignal(output_map.Get(), "signal_out");

    *pp_out_port_map = output_map.Get();
    output_map.Get()->AddRef();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::DoMerge(
    PluginInterface::IDasStopToken*       stop_token,
    ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
    ExportInterface::IDasPortMap**        pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // merge 是纯数据汇合原语，无子调用，不轮询取消。
    (void)stop_token;

    DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
    const DasResult hr = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    SetPortString(output_map.Get(), "status", "completed");

    // φ-join：merge 由 in_true/in_false signal gate 激活（runtime 保证只有命中
    // 分支到达 merge）。被 gate 掉的分支不产出 → 其 value_* 在输入 PortMap
    // 缺席。 取在场的那路 value 输出到 result；组件自身不读 signal（零感知）。
    if (p_input_port_map == nullptr
        || !CopyTypedPort(
            p_input_port_map,
            "value_true",
            output_map.Get(),
            "result"))
    {
        // value_true 缺席（true 分支未命中）→ 尝试 value_false。
        if (p_input_port_map != nullptr)
        {
            CopyTypedPort(
                p_input_port_map,
                "value_false",
                output_map.Get(),
                "result");
        }
    }

    *pp_out_port_map = output_map.Get();
    output_map.Get()->AddRef();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::DoDelay(
    PluginInterface::IDasStopToken* stop_token,
    ExportInterface::IDasReadOnlyPortMap* /*p_input_port_map*/,
    ExportInterface::IDasPortMap** pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // delay_ms 来自 config（settings_）；缺省 0 → 不等待，立即发 next。
    int64_t delay_ms = 0;
    if (auto settings_obj = settings_.as_object())
    {
        if (const auto v = FindObjectInt(*settings_obj, "delay_ms"))
        {
            delay_ms = *v;
        }
    }

    if (delay_ms > 0)
    {
        using clock = std::chrono::steady_clock;
        const auto deadline =
            clock::now() + std::chrono::milliseconds(delay_ms);

        // stop_token 仅轮询接口（无回调），无法主动 notify，故以小时间片
        // （50ms）在条件变量上等待、每片重查 stop——保证 stop 触发后约一片
        // 内响应并返回 cancelled。runtime 当前无循环次数护栏，等待必须可取消。
        std::mutex              wait_mutex;
        std::condition_variable wait_cv;

        std::unique_lock<std::mutex> lock(wait_mutex);
        while (true)
        {
            bool stop_requested = false;
            if (stop_token != nullptr
                && DAS::IsOk(stop_token->StopRequested(&stop_requested))
                && stop_requested)
            {
                return BuildResultPortMap(
                    "cancelled",
                    Das::Utils::MakeYyjsonObject(),
                    Das::Utils::MakeYyjsonArray(),
                    pp_out_port_map);
            }

            const auto now = clock::now();
            if (now >= deadline)
            {
                break;
            }

            const auto remaining_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now);
            const auto slice_ms = std::min<int64_t>(remaining_ms.count(), 50);
            // wait_for 仅在谓词命中（stop 为真）时提前返回，否则等满该切片
            // 超时后回到循环顶部重查 stop 与 deadline。
            wait_cv.wait_for(
                lock,
                std::chrono::milliseconds(slice_ms),
                [&stop_token]()
                {
                    bool stop = false;
                    return stop_token != nullptr
                           && DAS::IsOk(stop_token->StopRequested(&stop))
                           && stop;
                });
        }
    }

    // 等待结束（或 delay_ms==0）→ 发 next。
    auto outputs = Das::Utils::MakeYyjsonObject();
    auto signals = Das::Utils::MakeYyjsonArray();
    (*signals.as_array()).emplace_back("next");
    return BuildResultPortMap("completed", outputs, signals, pp_out_port_map);
}

void DasFlowControlTaskComponent::SetProcessSpawnerForTest(
    FlowControlProcessSpawner spawner)
{
    process_spawner_ = std::move(spawner);
}

void DasFlowControlTaskComponent::SetWindowEnumeratorForTest(
    FlowControlWindowEnumerator enumerator)
{
    window_enumerator_ = std::move(enumerator);
}

DasResult DasFlowControlTaskComponent::DoLaunch(
    PluginInterface::IDasStopToken* stop_token,
    ExportInterface::IDasReadOnlyPortMap* /*p_input_port_map*/,
    ExportInterface::IDasPortMap** pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    bool stop_requested = false;
    if (stop_token != nullptr
        && DAS::IsOk(stop_token->StopRequested(&stop_requested))
        && stop_requested)
    {
        return BuildResultPortMap(
            "cancelled",
            Das::Utils::MakeYyjsonObject(),
            Das::Utils::MakeYyjsonArray(),
            pp_out_port_map);
    }

    // config 消费（settings_；全部缺省安全）。两个等窗正则均缺省 = 不
    // 等待，spawn 成功即完成（合法用法）。
    std::string               exe_path;
    std::vector<std::string>  arguments;
    std::string               working_directory;
    bool                      wait_window_configured = false;
    std::string               class_regex;
    std::string               window_regex;
    std::chrono::milliseconds wait_window_timeout = DEFAULT_WAIT_WINDOW_TIMEOUT;

    if (auto settings_object = settings_.as_object())
    {
        if (const auto configured_exe_path =
                FindObjectString(*settings_object, "exePath"))
        {
            exe_path = *configured_exe_path;
        }
        if (const auto configured_arguments =
                FindObjectArray(*settings_object, "args"))
        {
            for (const auto& argument : *configured_arguments)
            {
                if (const auto argument_text = argument.as_string())
                {
                    arguments.emplace_back(std::string(*argument_text));
                }
            }
        }
        if (const auto configured_working_directory =
                FindObjectString(*settings_object, "workingDirectory"))
        {
            working_directory = *configured_working_directory;
        }
        if (const auto configured_class_regex =
                FindObjectString(*settings_object, "waitWindowClassRegex"))
        {
            class_regex = *configured_class_regex;
        }
        if (const auto configured_window_regex =
                FindObjectString(*settings_object, "waitWindowWindowRegex"))
        {
            window_regex = *configured_window_regex;
        }
        if (const auto configured_timeout_ms =
                FindObjectInt(*settings_object, "waitWindowTimeoutMs"))
        {
            wait_window_timeout =
                std::chrono::milliseconds(*configured_timeout_ms);
        }
    }

    // 任一正则非空即等待；两个正则均缺省/为空 = 不等待（仅配超时不触发）。
    wait_window_configured = !class_regex.empty() || !window_regex.empty();

    if (exe_path.empty())
    {
        return BuildLaunchFailedPortMap(
            "launch-exe-path-missing",
            Plugins::DasFlowControl::DAS_E_FLOW_LAUNCH_EXE_PATH_MISSING,
            pp_out_port_map);
    }

    if (!process_spawner_)
    {
        process_spawner_ = DefaultProcessSpawner();
    }
    if (!process_spawner_(exe_path, arguments, working_directory))
    {
        return BuildLaunchFailedPortMap(
            "launch-spawn-failed",
            Plugins::DasFlowControl::DAS_E_FLOW_LAUNCH_SPAWN_FAILED,
            pp_out_port_map);
    }

    if (wait_window_configured)
    {
        if (!window_enumerator_)
        {
            window_enumerator_ = DefaultWindowEnumerator();
        }

        // 超时兜底：负值归零（立即超时），上限防病态配置占死执行。
        auto timeout = wait_window_timeout;
        if (timeout < std::chrono::milliseconds(0))
        {
            timeout = std::chrono::milliseconds(0);
        }
        if (timeout > MAX_WAIT_WINDOW_TIMEOUT)
        {
            timeout = MAX_WAIT_WINDOW_TIMEOUT;
        }

        const auto class_regex_compiled = CompileWindowRegex(class_regex);
        const auto window_regex_compiled = CompileWindowRegex(window_regex);
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        // stop_token 仅轮询接口（无回调），照 DoDelay 以小时间片在条件
        // 变量上等待、每片重查 stop——保证 stop 触发后约一片内响应。
        std::mutex              wait_mutex;
        std::condition_variable wait_cv;

        std::unique_lock<std::mutex> lock(wait_mutex);
        while (true)
        {
            bool poll_stop_requested = false;
            if (stop_token != nullptr
                && DAS::IsOk(stop_token->StopRequested(&poll_stop_requested))
                && poll_stop_requested)
            {
                return BuildResultPortMap(
                    "cancelled",
                    Das::Utils::MakeYyjsonObject(),
                    Das::Utils::MakeYyjsonArray(),
                    pp_out_port_map);
            }

            size_t match_count = 0;
            for (const auto& window : window_enumerator_())
            {
                if (WindowRegexMatches(class_regex_compiled, window.class_name)
                    && WindowRegexMatches(
                        window_regex_compiled,
                        window.window_name))
                {
                    ++match_count;
                }
            }

            if (match_count == 1)
            {
                break; // 目标窗口出现
            }
            if (match_count > 1)
            {
                return BuildLaunchFailedPortMap(
                    "launch-window-ambiguous",
                    Plugins::DasFlowControl::DAS_E_FLOW_LAUNCH_WINDOW_AMBIGUOUS,
                    pp_out_port_map);
            }

            if (std::chrono::steady_clock::now() >= deadline)
            {
                return BuildLaunchFailedPortMap(
                    "launch-window-timeout",
                    Plugins::DasFlowControl::DAS_E_FLOW_LAUNCH_WINDOW_TIMEOUT,
                    pp_out_port_map);
            }

            wait_cv.wait_for(
                lock,
                WAIT_WINDOW_POLL_SLICE,
                [stop_token]()
                {
                    bool stop = false;
                    return stop_token != nullptr
                           && DAS::IsOk(stop_token->StopRequested(&stop))
                           && stop;
                });
        }
    }

    // 启动（及可选等待）完成 → 发规范信号口 signal_out（formSequence 投影
    // 信号边用这对端口名门控下游）。
    auto outputs = Das::Utils::MakeYyjsonObject();
    auto signals = Das::Utils::MakeYyjsonArray();
    (*signals.as_array()).emplace_back("signal_out");
    return BuildResultPortMap("completed", outputs, signals, pp_out_port_map);
}

DasResult DasFlowControlTaskComponent::Do(
    PluginInterface::IDasStopToken*       stop_token,
    ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
    ExportInterface::IDasPortMap**        pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    if (kind_ == REPOSITORY_INVOKE_KIND)
    {
        return DoRepositoryInvoke(
            stop_token,
            p_input_port_map,
            pp_out_port_map);
    }

    if (kind_ == BRANCH_KIND)
    {
        return DoBranch(stop_token, p_input_port_map, pp_out_port_map);
    }

    if (kind_ == FOR_KIND)
    {
        return DoFor(stop_token, p_input_port_map, pp_out_port_map);
    }

    if (kind_ == WHILE_KIND)
    {
        return DoWhile(stop_token, p_input_port_map, pp_out_port_map);
    }

    if (kind_ == COMPARE_DOUBLE_KIND)
    {
        return DoCompareDouble(stop_token, p_input_port_map, pp_out_port_map);
    }

    if (kind_ == COMPARE_INT64_KIND)
    {
        return DoCompareInt64(stop_token, p_input_port_map, pp_out_port_map);
    }

    if (kind_ == COMPARE_STRING_KIND)
    {
        return DoCompareString(stop_token, p_input_port_map, pp_out_port_map);
    }

    if (kind_ == ARITHMETIC_NUMBER_KIND)
    {
        return DoArithmeticNumber(
            stop_token,
            p_input_port_map,
            pp_out_port_map);
    }

    if (kind_ == CONSTANT_BOOL_KIND)
    {
        return DoConstantBool(stop_token, p_input_port_map, pp_out_port_map);
    }

    if (kind_ == CONSTANT_INT64_KIND)
    {
        return DoConstantInt64(stop_token, p_input_port_map, pp_out_port_map);
    }

    if (kind_ == CONSTANT_DOUBLE_KIND)
    {
        return DoConstantDouble(stop_token, p_input_port_map, pp_out_port_map);
    }

    if (kind_ == CONSTANT_STRING_KIND)
    {
        return DoConstantString(stop_token, p_input_port_map, pp_out_port_map);
    }

    if (kind_ == CONCAT_STRING_KIND)
    {
        return DoConcatString(stop_token, p_input_port_map, pp_out_port_map);
    }

    if (kind_ == MERGE_KIND)
    {
        return DoMerge(stop_token, p_input_port_map, pp_out_port_map);
    }

    if (kind_ == DELAY_KIND)
    {
        return DoDelay(stop_token, p_input_port_map, pp_out_port_map);
    }

    if (kind_ == LAUNCH_KIND)
    {
        return DoLaunch(stop_token, p_input_port_map, pp_out_port_map);
    }

    bool stop_requested = false;
    if (stop_token != nullptr
        && DAS::IsOk(stop_token->StopRequested(&stop_requested))
        && stop_requested)
    {
        return BuildResultPortMap(
            "cancelled",
            Das::Utils::MakeYyjsonObject(),
            Das::Utils::MakeYyjsonArray(),
            pp_out_port_map);
    }

    // 防御性兜底：未识别 kind 发 next（所有注册 kind 均已分发，正常不应到达）。
    auto outputs = Das::Utils::MakeYyjsonObject();
    auto signals = Das::Utils::MakeYyjsonArray();
    auto signals_arr = *signals.as_array();
    signals_arr.emplace_back("next");

    return BuildResultPortMap("completed", outputs, signals, pp_out_port_map);
}

DasFlowControlTaskComponentFactory::DasFlowControlTaskComponentFactory(
    PluginInterface::IDasTaskComponentHost* p_host)
    : host_(DasPtr<PluginInterface::IDasTaskComponentHost>(p_host))
{
}

DasResult DasFlowControlTaskComponentFactory::GetGuid(DasGuid* p_out_guid)
{
    if (p_out_guid == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *p_out_guid = DasIidOf<DasFlowControlTaskComponentFactory>();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponentFactory::GetRuntimeClassName(
    IDasReadOnlyString** pp_out_name)
{
    if (pp_out_name == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    return CreateIDasReadOnlyStringFromUtf8(
        "Das.FlowControlTaskComponentFactory",
        pp_out_name);
}

DasResult DasFlowControlTaskComponentFactory::CreateComponent(
    const DasGuid&                       component_guid,
    PluginInterface::IDasTaskComponent** pp_out_component)
{
    if (pp_out_component == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_component = nullptr;

    const auto spec = FindSpec(component_guid);
    if (!spec)
    {
        return DAS_E_NOT_FOUND;
    }

    try
    {
        auto* component = new DasFlowControlTaskComponent(spec->kind, host_);
        component->AddRef();
        *pp_out_component =
            static_cast<PluginInterface::IDasTaskComponent*>(component);
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DAS_NS_END
