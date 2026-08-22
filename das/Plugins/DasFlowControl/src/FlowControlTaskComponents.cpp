#define DAS_BUILD_SHARED

#include "FlowControlTaskComponents.h"

#include "FlowControlI18n.h"

#include <das/Core/TaskScheduler/RepositoryInvokeDtos.h>
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
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

    constexpr std::string_view BRANCH_KIND = "das.flow.branch";
    constexpr std::string_view DELAY_KIND = "das.flow.delay";
    constexpr std::string_view FOR_KIND = "das.flow.for";
    constexpr std::string_view WHILE_KIND = "das.flow.while";
    constexpr std::string_view MERGE_KIND = "das.flow.merge";
    constexpr std::string_view LAUNCH_KIND = "das.flow.launch";

    // waitWindow 轮询的小时间片（stop 只能轮询，切片保证响应性）与超时
    // 上限兜底（防病态配置把执行占死）；timeoutMs 缺省用默认值。
    constexpr auto WAIT_WINDOW_POLL_SLICE = std::chrono::milliseconds(100);
    constexpr auto DEFAULT_WAIT_WINDOW_TIMEOUT =
        std::chrono::milliseconds(30000);
    constexpr auto MAX_WAIT_WINDOW_TIMEOUT = std::chrono::milliseconds(120000);

    constexpr std::array<ComponentSpec, 7> COMPONENTS{
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
    using Das::Utils::CopyTypedPort;
    using Das::Utils::EmitSignal;
    using Das::Utils::GetPortBool;
    using Das::Utils::GetPortInt;
    using Das::Utils::GetPortJson;
    using Das::Utils::GetPortString;
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

    // cpp_yyjson 的 object operator[] 对缺失键抛 out_of_range；launch 的
    // 配置键全部可选，统一经下列 helper 读取（缺键/类型不符 → nullopt）。
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
    auto FindObjectObject(const ObjectRef& object, std::string_view key)
        -> decltype(object[key].as_object())
    {
        if (!object.contains(key))
        {
            return std::nullopt;
        }
        return object[key].as_object();
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

    // 失败出口：status=failed + 结构化 diagnostic（code + i18n 消息）。
    DasResult BuildLaunchFailedPortMap(
        std::string_view               diagnostic_code,
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
        return DAS_S_OK;
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
                boost::asio::io_context io_context;
                if (working_directory.empty())
                {
                    boost::process::v2::process child(
                        io_context,
                        exe_path,
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
                    exe_path,
                    arguments,
                    boost::process::v2::process_start_dir(working_directory),
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

    settings_ = CloneJson(accepted);

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

    // Read compiledSnapshot from input port map
    auto snapshot_json = GetPortJson(p_input_port_map, "compiledSnapshot");
    if (!snapshot_json || !snapshot_json->is_object())
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
        return DAS_S_OK;
    }

    RepositoryInvokeDto::ChildExecutionSnapshotDto snapshot;
    try
    {
        snapshot = yyjson::cast<RepositoryInvokeDto::ChildExecutionSnapshotDto>(
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
        return DAS_S_OK;
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
        return DAS_S_OK;
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
        return DAS_S_OK;
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
        return DAS_S_OK;
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
        return DAS_S_OK;
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

    // 首次进入初始化计数器；循环状态是组件成员，跨多次 Do() 保持。组件实例
    // 每次 GraphRuntime::Configure 重建，自然按图执行重置（不进 PortFrame）。
    if (!loop_started_)
    {
        loop_index_ = start;
        loop_started_ = true;
    }

    DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
    const DasResult hr = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    SetPortString(output_map.Get(), "status", "completed");

    const bool in_range =
        (step > 0) ? (loop_index_ < end) : (loop_index_ > end);
    if (in_range)
    {
        // 写循环变量 index（data 输出端口，循环体经 binding 读取），emit
        // continue 激活循环体；循环体完成后 back-edge 回到 for 重新 Do()。
        SetPortInt(output_map.Get(), "index", loop_index_);
        EmitSignal(output_map.Get(), "continue");
        loop_index_ += step;
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

    // 每轮读动态 condition（由循环体/外部每轮回边后重算并写入 PortFrame）。
    const bool condition =
        p_input_port_map ? GetPortBool(p_input_port_map, "condition", false)
                         : false;

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
        if (const auto v =
                (*settings_obj)[std::string_view("delay_ms")].as_int())
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

    // config 消费（settings_；全部缺省安全）。waitWindow 缺省 = 不等待，
    // spawn 成功即完成（合法用法）。
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
        if (const auto wait_window =
                FindObjectObject(*settings_object, "waitWindow"))
        {
            wait_window_configured = true;
            if (const auto configured_class_regex =
                    FindObjectString(*wait_window, "classRegex"))
            {
                class_regex = *configured_class_regex;
            }
            if (const auto configured_window_regex =
                    FindObjectString(*wait_window, "windowRegex"))
            {
                window_regex = *configured_window_regex;
            }
            if (const auto configured_timeout_ms =
                    FindObjectInt(*wait_window, "timeoutMs"))
            {
                wait_window_timeout =
                    std::chrono::milliseconds(*configured_timeout_ms);
            }
        }
    }

    if (exe_path.empty())
    {
        return BuildLaunchFailedPortMap(
            "launch-exe-path-missing",
            pp_out_port_map);
    }

    if (!process_spawner_)
    {
        process_spawner_ = DefaultProcessSpawner();
    }
    if (!process_spawner_(exe_path, arguments, working_directory))
    {
        return BuildLaunchFailedPortMap("launch-spawn-failed", pp_out_port_map);
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
                    pp_out_port_map);
            }

            if (std::chrono::steady_clock::now() >= deadline)
            {
                return BuildLaunchFailedPortMap(
                    "launch-window-timeout",
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

DasResult DasFlowControlTaskComponentFactory::SetTaskComponentHost(
    PluginInterface::IDasTaskComponentHost* p_host)
{
    host_ = DasPtr<PluginInterface::IDasTaskComponentHost>(p_host);
    return DAS_S_OK;
}

DAS_NS_END
