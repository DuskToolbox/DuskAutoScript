#define DAS_BUILD_SHARED

#include "FlowControlTaskComponents.h"

#include "FlowControlErrorCodes.h"
#include "FlowControlI18n.h"
#include "LaunchOutputCapture.h"

#include <das/DasString.hpp>
#include <das/Utils/BoostPathBridge.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/PathText.h>
#include <das/Utils/TaskComponentPorts.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasSettingsChangeBuilder.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/header/IDasSettingsChangeBuilder.generated.h>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_BOOST_PROCESS_WARNING

#include <boost/asio/io_context.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/shell.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/system/system_error.hpp>
#ifdef _WIN32
// creation_flags.hpp 依赖 Windows 类型（DWORD / CREATE_NEW_PROCESS_GROUP），
// 必须整块守卫——POSIX 上不存在这些类型，不守卫 include 会编译错。
#include <boost/process/v2/windows/creation_flags.hpp>
#endif

DAS_DISABLE_WARNING_END

#ifdef _WIN32
#include <windows.h>
// shellapi.h 依赖 windows.h 的基础类型（HWND/DWORD），顺序不可颠倒。
#include <shellapi.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <new>
#include <optional>
#include <regex>
#include <system_error>
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

    // 提权回退启动缝槽（测试可注入；空 = 生产实现）。互斥保护：注入与 Do
    // 可能来自不同线程。
    std::mutex& ElevatedLauncherMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    FlowControlElevatedLauncher& ElevatedLauncherSlot()
    {
        static FlowControlElevatedLauncher launcher;
        return launcher;
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

    constexpr std::array<ComponentSpec, 15> COMPONENTS{
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
        if (diagnostic_code == "launch-elevation-declined")
        {
            return "The elevation request to start the executable was "
                   "declined.";
        }
        if (diagnostic_code == "launch-exe-path-missing")
        {
            return "No executable path is configured for the launch "
                   "component.";
        }
        if (diagnostic_code == "launch-exe-not-found")
        {
            return "The configured executable was not found on PATH.";
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

    // 裸命令名判定：无目录段（"python"/"python.exe" 是裸名；带目录段的
    // "C:/x/app.exe"、"./app.exe"、"x/app.exe" 是用户显式定位，不经 PATH
    // 解析——找不到属启动失败而非解析域）。
    bool IsBareCommandName(const std::string& exe_path_utf8)
    {
        try
        {
            return !Das::Utils::PathFromUtf8(exe_path_utf8).has_parent_path();
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    // 裸命令名 PATH 解析（bp2 find_executable：逐 PATH 目录 + PATHEXT 扩
    // 展尝试 is_executable）；未命中返回空串（调用方显性失败）。
    std::string ResolveBareCommandOnPath(const std::string& exe_path_utf8)
    {
        try
        {
            const auto resolved_path =
                boost::process::v2::environment::find_executable(
                    Das::Utils::ToBoostPath(
                        Das::Utils::PathFromUtf8(exe_path_utf8)));
            if (resolved_path.empty())
            {
                return {};
            }
            return Das::Utils::PathToUtf8(Das::Utils::ToStdPath(resolved_path));
        }
        catch (const std::exception&)
        {
            return {};
        }
    }

    // 捕获日志文件名的进程名段：exe 路径去目录与扩展名的 stem 文本
    // （清洗在捕获工厂内完成）。路径解析异常回退空段（工厂侧转占位名）。
    std::string LaunchCaptureProcessStem(const std::string& exe_path_utf8)
    {
        try
        {
            return Das::Utils::PathToUtf8(
                Das::Utils::PathFromUtf8(exe_path_utf8).stem());
        }
        catch (const std::exception&)
        {
            return {};
        }
    }

    // 主启动尝试体：boost::process v2 spawn，成功后立即 detach——
    // basic_process 析构 terminate_if_running，不 detach 则组件 Do 返回即
    // 杀子进程。capture_output=false 三路 stdio 丢弃（启动器不消费子进程
    // 输出）；true 时捕获会话接管道逐行落盘（会话生命周期自持，host 退出
    // 绝不连带杀被启动进程）。失败抛异常，异常分类（是否 740 提权域）由
    // 编排函数 SpawnProcessWithElevationFallback 决定。
    std::uint32_t PrimarySpawnAttempt(
        const std::string&              exe_path,
        const std::vector<std::string>& arguments,
        const std::string&              working_directory,
        bool                            capture_output)
    {
        std::shared_ptr<LaunchOutputCaptureSession> capture_session;
        if (capture_output)
        {
            capture_session = CreateLaunchOutputCaptureSession(
                ResolveLaunchCaptureLogDirectory(),
                LaunchCaptureProcessStem(exe_path));
            // nullptr = 工厂已告警；降级丢弃形态照常启动。
        }

        // bp2 的 exe 与 start_dir 形参是 boost path：窄串经 ACP 构造会双损
        // （中文路径下启动错位），全程保 path 进出；arguments 形参是窄
        // UTF-8 通道，保持直传。
        boost::asio::io_context io_context;
        if (working_directory.empty())
        {
            boost::process::v2::process child(
                io_context,
                Das::Utils::ToBoostPath(Das::Utils::PathFromUtf8(exe_path)),
                arguments,
                capture_session
                    ? boost::process::v2::process_stdio{
                          nullptr,
                          capture_session->StdoutPipe(),
                          capture_session->StderrPipe()}
                    // bp2 语义：stdin 位 {}（默认构造）= 继承父进程句柄，
                    // 无控制台宿主（GUI 子系统进程）的 GetStdHandle 返回
                    // NULL → CreateProcessW 失败；nullptr = 重定向到 NUL
                    // 设备，任意宿主形态可启动。
                    : boost::process::v2::process_stdio{
                          nullptr,
                          nullptr,
                          nullptr}
#ifdef _WIN32
                // 控制台子进程不闪窗，GUI 子进程无影响。
                ,
                boost::process::v2::windows::process_creation_flags<
                    CREATE_NO_WINDOW>{}
#endif
            );
            const auto child_process_id =
                static_cast<std::uint32_t>(child.id());
            (void)child.detach();
            if (capture_session)
            {
                capture_session->Start(child_process_id);
            }
            return child_process_id;
        }
        boost::process::v2::process child(
            io_context,
            Das::Utils::ToBoostPath(Das::Utils::PathFromUtf8(exe_path)),
            arguments,
            boost::process::v2::process_start_dir(
                Das::Utils::ToBoostPath(
                    Das::Utils::PathFromUtf8(working_directory))),
            capture_session
                ? boost::process::v2::process_stdio{
                      nullptr,
                      capture_session->StdoutPipe(),
                      capture_session->StderrPipe()}
                : boost::process::v2::process_stdio{
                      nullptr,
                      nullptr,
                      nullptr}
#ifdef _WIN32
            ,
            boost::process::v2::windows::process_creation_flags<
                CREATE_NO_WINDOW>{}
#endif
        );
        const auto child_process_id = static_cast<std::uint32_t>(child.id());
        (void)child.detach();
        if (capture_session)
        {
            capture_session->Start(child_process_id);
        }
        return child_process_id;
    }

#ifdef _WIN32
    // lpParameters 拼接的引号规则：含空白或空串的参数双引号包裹（与
    // CreateProcess 命令行约定一致），内嵌引号加反斜杠转义——参数注入面
    // 不劣于既有 bp2 spawn 路径。
    std::wstring QuoteElevatedLaunchArgument(const std::wstring& argument)
    {
        const bool needs_quotes =
            argument.empty()
            || argument.find_first_of(L" \t") != std::wstring::npos;
        if (!needs_quotes)
        {
            return argument;
        }
        std::wstring quoted;
        quoted.push_back(L'"');
        for (const wchar_t character : argument)
        {
            if (character == L'"')
            {
                quoted.push_back(L'\\');
            }
            quoted.push_back(character);
        }
        quoted.push_back(L'"');
        return quoted;
    }
#endif

    // launch args 命令文本（D-A9 string 形态）→ argv token 序列：仅借
    // boost::process v2::shell 的解析器（Windows=CommandLineToArgvW/
    // MSVCRT 规则），绝不经 shell 执行——token 回喂既有 vector 逐参
    // spawner，参数注入面不劣于既有 bp2 spawn 路径。exe 前缀经既有引号
    // 规则（QuoteElevatedLaunchArgument）保护：含空白的 exe 不被误分
    // 词；argv()[0] 是 exe 锚，参数 token 从 [1, argc) 取。空文本 = 空
    // token 序列。
    std::vector<std::string> ParseLaunchArgumentCommandText(
        const std::string& exe_path_utf8,
        const std::string& arguments_text_utf8)
    {
        std::vector<std::string> tokens;
#ifdef _WIN32
        std::wstring command_line = QuoteElevatedLaunchArgument(
            Das::Utils::PathFromUtf8(exe_path_utf8).wstring());
        if (!arguments_text_utf8.empty())
        {
            command_line.push_back(L' ');
            command_line +=
                Das::Utils::PathFromUtf8(arguments_text_utf8).wstring();
        }
        // wcstring_ref 构造不拷贝（input_ 借用 command_line 数据）——
        // command_line 生存期覆盖 parsed_command，安全。
        const boost::process::v2::shell parsed_command{
            boost::process::v2::wcstring_ref{command_line}};
        if (parsed_command.argc() > 1)
        {
            tokens.reserve(static_cast<std::size_t>(parsed_command.argc() - 1));
        }
        for (int index = 1; index < parsed_command.argc(); ++index)
        {
            tokens.emplace_back(
                Das::Utils::PathToUtf8(
                    std::filesystem::path{
                        std::wstring{parsed_command.argv()[index]}}));
        }
#else
        // manifest 声明 supportedSystem=Windows；POSIX 仅保可编译——窄
        // UTF-8 直构造（char_type=char），token 即 UTF-8 原文。
        std::string command_line = exe_path_utf8;
        if (!arguments_text_utf8.empty())
        {
            command_line.push_back(' ');
            command_line += arguments_text_utf8;
        }
        const boost::process::v2::shell parsed_command{
            boost::process::v2::cstring_ref{command_line}};
        for (int index = 1; index < parsed_command.argc(); ++index)
        {
            tokens.emplace_back(std::string{parsed_command.argv()[index]});
        }
#endif
        return tokens;
    }

    // 生产提权回退启动缝：ShellExecuteExW。成功后取 pid 立即关句柄——
    // 不 WAIT 不持有（launch 语义 spawn 即完成）；失败路径无句柄产生。
    FlowControlElevatedLauncher DefaultElevatedLauncher()
    {
        return [](const std::string&              exe_path,
                  const std::vector<std::string>& arguments,
                  const std::string&              working_directory)
                   -> FlowControlElevatedLaunchOutcome
        {
#ifdef _WIN32
            try
            {
                // 局部 path 变量保生命周期：W API 直喂 native 宽串。
                const std::filesystem::path exe_native =
                    Das::Utils::PathFromUtf8(exe_path);
                const std::filesystem::path working_directory_native =
                    Das::Utils::PathFromUtf8(working_directory);

                std::wstring parameters;
                for (const std::string& argument : arguments)
                {
                    if (!parameters.empty())
                    {
                        parameters.push_back(L' ');
                    }
                    parameters += QuoteElevatedLaunchArgument(
                        Das::Utils::PathFromUtf8(argument).wstring());
                }

                SHELLEXECUTEINFOW execute_info{};
                execute_info.cbSize = sizeof(execute_info);
                // SEE_MASK_NOCLOSEPROCESS 拿 hProcess 取 pid 后立即关闭。
                execute_info.fMask = SEE_MASK_NOCLOSEPROCESS;
                // 普通 open verb：requireAdministrator manifest 的 exe 由 OS
                // 协商 UAC 弹窗，host 自身不提权、不注入 runas。
                execute_info.lpVerb = L"open";
                execute_info.lpFile = exe_native.c_str();
                execute_info.lpParameters =
                    parameters.empty() ? nullptr : parameters.c_str();
                execute_info.lpDirectory =
                    working_directory.empty()
                        ? nullptr
                        : working_directory_native.c_str();
                execute_info.nShow = SW_SHOWNORMAL;

                FlowControlElevatedLaunchOutcome outcome;
                if (!ShellExecuteExW(&execute_info))
                {
                    const auto shell_error = static_cast<int>(GetLastError());
                    if (shell_error == ERROR_CANCELLED)
                    {
                        outcome.status =
                            FlowControlElevatedLaunchStatus::Declined;
                        return outcome;
                    }
                    DAS_LOG_WARNING(
                        DAS_FMT_NS::format(
                            "Elevated shell launch failed: GetLastError={}",
                            shell_error)
                            .c_str());
                    return outcome;
                }
                outcome.status = FlowControlElevatedLaunchStatus::Spawned;
                if (execute_info.hProcess != nullptr)
                {
                    outcome.process_id = static_cast<std::uint32_t>(
                        GetProcessId(execute_info.hProcess));
                    CloseHandle(execute_info.hProcess);
                }
                else
                {
                    DAS_LOG_WARNING(
                        "Elevated shell launch started the process but no "
                        "process handle was returned; pid unknown");
                }
                return outcome;
            }
            catch (...)
            {
                DAS_LOG_WARNING("Elevated shell launch failed: unknown error");
                return {};
            }
#else
            // 740 是 Win32 错误码，生产主启动体在 POSIX 永不产出该分类，
            // 提权回退启动器在 POSIX 无实现（保可编译）。
            (void)exe_path;
            (void)arguments;
            (void)working_directory;
            return {};
#endif
        };
    }

    // 提权启动缝解析：测试槽非空取槽值，否则生产实现。锁内只拷贝——
    // ShellExecuteExW 可能因 UAC 弹窗长时间阻塞，不能持锁等待用户输入。
    FlowControlElevatedLauncher ResolveElevatedLauncher()
    {
        const std::scoped_lock lock(ElevatedLauncherMutex());
        if (ElevatedLauncherSlot())
        {
            return ElevatedLauncherSlot();
        }
        return DefaultElevatedLauncher();
    }

    // 生产进程启动缝：主启动体 + 提权回退编排的薄包装。
    FlowControlProcessSpawner DefaultProcessSpawner()
    {
        return [](const std::string&              exe_path,
                  const std::vector<std::string>& arguments,
                  const std::string&              working_directory,
                  bool capture_output) -> FlowControlSpawnResult
        {
            return SpawnProcessWithElevationFallback(
                &PrimarySpawnAttempt,
                ResolveElevatedLauncher(),
                exe_path,
                arguments,
                working_directory,
                capture_output);
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

void SetElevatedLauncherForTest(FlowControlElevatedLauncher launcher)
{
    const std::scoped_lock lock(ElevatedLauncherMutex());
    ElevatedLauncherSlot() = std::move(launcher);
}

FlowControlSpawnResult SpawnProcessWithElevationFallback(
    const FlowControlPrimarySpawnBody& primary_spawn_body,
    const FlowControlElevatedLauncher& elevated_launcher,
    const std::string&                 exe_path,
    const std::vector<std::string>&    arguments,
    const std::string&                 working_directory,
    bool                               capture_output)
{
    try
    {
        (void)primary_spawn_body(
            exe_path,
            arguments,
            working_directory,
            capture_output);
        return FlowControlSpawnResult{true};
    }
    catch (const boost::system::system_error& error)
    {
#ifdef _WIN32
        if (error.code().value() == ERROR_ELEVATION_REQUIRED)
        {
            // 740 分支不打通用失败日志——回退路径自行叙述（降级/结果 WARN）。
            if (capture_output)
            {
                DAS_LOG_WARNING(
                    "Process spawn hit ERROR_ELEVATION_REQUIRED; falling "
                    "back to shell elevation launch - output capture is "
                    "not available on the elevated path");
            }
            const FlowControlElevatedLaunchOutcome elevated_outcome =
                elevated_launcher(exe_path, arguments, working_directory);
            switch (elevated_outcome.status)
            {
            case FlowControlElevatedLaunchStatus::Spawned:
                DAS_LOG_WARNING(
                    DAS_FMT_NS::format(
                        "Process started via shell elevation fallback "
                        "(pid={})",
                        elevated_outcome.process_id)
                        .c_str());
                return FlowControlSpawnResult{true};
            case FlowControlElevatedLaunchStatus::Declined:
                DAS_LOG_WARNING("Shell elevation launch declined by the user");
                return FlowControlSpawnResult{
                    FlowControlSpawnStatus::ElevationDeclined};
            case FlowControlElevatedLaunchStatus::Failed:
                DAS_LOG_WARNING("Shell elevation fallback launch failed");
                return FlowControlSpawnResult{FlowControlSpawnStatus::Failed};
            }
            return FlowControlSpawnResult{FlowControlSpawnStatus::Failed};
        }
#endif
        const auto reason = DAS_FMT_NS::format(
            "Process spawn failed: {} (code = {})",
            Das::Utils::AcpTextToUtf8(error.what()),
            error.code().value());
        DAS_LOG_WARNING(reason.c_str());
        return FlowControlSpawnResult{false};
    }
    catch (const std::exception& exception)
    {
        // what() 可能携带 ACP 字节（FormatMessageA 的本地化消息），日志
        // 管道按 UTF-8 消费——转码后中文段可读，ASCII 尾巴无损。
        const auto reason = DAS_FMT_NS::format(
            "Process spawn failed: {}",
            Das::Utils::AcpTextToUtf8(exception.what()));
        DAS_LOG_WARNING(reason.c_str());
        return FlowControlSpawnResult{false};
    }
    catch (...)
    {
        DAS_LOG_WARNING("Process spawn failed: unknown error");
        return FlowControlSpawnResult{false};
    }
}

DasFlowControlTaskComponent::DasFlowControlTaskComponent(std::string_view kind)
    : kind_(kind), settings_(Das::Utils::MakeYyjsonObject())
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
    bool                      capture_output = false;
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
        if (const auto configured_arguments_text =
                FindObjectString(*settings_object, "args"))
        {
            // D-A9：args 声明改 string——命令文本直读，经 v2::shell 解析
            // 为 token 序列（exe 前缀引号保护；见
            // ParseLaunchArgumentCommandText）。解析失败显性失败，不静默
            // 丢参（错误吞掉后半段参数比启动失败更伤）。
            try
            {
                arguments = ParseLaunchArgumentCommandText(
                    exe_path,
                    *configured_arguments_text);
            }
            catch (const std::exception& parse_error)
            {
                DAS_LOG_WARNING(
                    DAS_FMT_NS::format(
                        "Launch args command text parse failed: {}",
                        parse_error.what())
                        .c_str());
                return BuildLaunchFailedPortMap(
                    "launch-spawn-failed",
                    Plugins::DasFlowControl::DAS_E_FLOW_LAUNCH_SPAWN_FAILED,
                    pp_out_port_map);
            }
        }
        else if (
            const auto configured_arguments =
                FindObjectArray(*settings_object, "args"))
        {
            // 宽容读：array<string> 时期写入的存量条目原样逐元素读（数据
            // 不迁移）；非 string 非 array 形态落既有缺失处理（空参，不
            // 猜测）。
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
        if (const auto configured_capture_output =
                FindObjectBool(*settings_object, "captureOutput"))
        {
            capture_output = *configured_capture_output;
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

    // 裸命令名且非已存在文件 → PATH 解析为完整路径；解析产物是启动路径
    // 与捕获文件名进程名的统一来源（解析后形态全链一致）。未命中 = 显性
    // 失败（专用诊断码），不落入通用 spawn 失败。
    if (IsBareCommandName(exe_path))
    {
        std::error_code file_error;
        if (!std::filesystem::is_regular_file(
                Das::Utils::PathFromUtf8(exe_path),
                file_error))
        {
            const auto resolved_path = ResolveBareCommandOnPath(exe_path);
            if (resolved_path.empty())
            {
                return BuildLaunchFailedPortMap(
                    "launch-exe-not-found",
                    Plugins::DasFlowControl::DAS_E_FLOW_LAUNCH_EXE_NOT_FOUND,
                    pp_out_port_map);
            }
            exe_path = resolved_path;
        }
    }

    if (!process_spawner_)
    {
        process_spawner_ = DefaultProcessSpawner();
    }
    const FlowControlSpawnResult spawn_result = process_spawner_(
        exe_path,
        arguments,
        working_directory,
        capture_output);
    if (spawn_result.status == FlowControlSpawnStatus::ElevationDeclined)
    {
        return BuildLaunchFailedPortMap(
            "launch-elevation-declined",
            Plugins::DasFlowControl::DAS_E_FLOW_LAUNCH_ELEVATION_DECLINED,
            pp_out_port_map);
    }
    if (spawn_result.status != FlowControlSpawnStatus::Spawned)
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

    // 启动（及可选等待）完成。规范信号口 signal_out 的成功发射由运行时
    // 兜底保证（编译器对规范口 signal 边打标），组件只发具名控制口。
    auto outputs = Das::Utils::MakeYyjsonObject();
    return BuildResultPortMap(
        "completed",
        outputs,
        Das::Utils::MakeYyjsonArray(),
        pp_out_port_map);
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
        auto* component = new DasFlowControlTaskComponent(spec->kind);
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
