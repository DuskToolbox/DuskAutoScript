// 因为我们对getenv的使用非常克制，所以这里可以使用宏屏蔽警告
// warning C4996: 'getenv': This function or variable may be unsafe. Consider
// using _dupenv_s instead.
#define _CRT_SECURE_NO_WARNINGS
#include <array>
#include <boost/dll/runtime_symbol_info.hpp>
#include <cctype>

#include <das/DasConfig.h>

#include <ctime>
#include <das/Core/Logger/CrossProcessMutex.h>
#include <das/Core/Logger/DasLogNotifierSink.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/fmt.h>
#include <filesystem>
#include <iomanip>
#include <regex>
#include <spdlog/details/null_mutex.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <sstream>
#include <string>
#include <string_view>

#if defined(_WIN32) || defined(__CYGWIN__)
#include <windows.h>
DAS_NS_ANONYMOUS_DETAILS_BEGIN
void UseUtf8Console()
{
    ::SetConsoleOutputCP(CP_UTF8);
    CONSOLE_FONT_INFOEX cfi;
    cfi.cbSize = sizeof cfi;
    cfi.nFont = 0;
    cfi.dwFontSize.X = 0;
    cfi.dwFontSize.Y = 14;
    cfi.FontFamily = FF_DONTCARE;
    cfi.FontWeight = FW_NORMAL;
    ::wcscpy_s(cfi.FaceName, LF_FACESIZE, L"Lucida Console");
    if (::SetCurrentConsoleFontEx(
            ::GetStdHandle(STD_OUTPUT_HANDLE),
            FALSE,
            &cfi)
        == 0)
    {
        const auto error_code = ::GetLastError();
        SPDLOG_ERROR(
            "Failed to set console font. GetLastError = {}",
            error_code);
    }
}

void EnableVirtualTerminalProcessing()
{
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle != INVALID_HANDLE_VALUE)
    {
        DWORD mode = 0;
        if (GetConsoleMode(handle, &mode))
        {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(handle, mode);
        }
    }
}
DAS_NS_ANONYMOUS_DETAILS_END
#define DAS_CONFIG_WIN32_CONSOLE                                               \
    ::Details::UseUtf8Console();                                               \
    ::Details::EnableVirtualTerminalProcessing()
#else
#include <cerrno>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#define DAS_CONFIG_WIN32_CONSOLE
#endif // DAS_WINDOWS

DAS_NS_ANONYMOUS_DETAILS_BEGIN
class ProcessSafeStdoutSink final
    : public spdlog::sinks::base_sink<spdlog::details::null_mutex>
{
    spdlog::sinks::stdout_color_sink_mt  inner_sink_;
    DAS::Core::Logger::CrossProcessMutex cross_process_mutex_;

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        auto lock = cross_process_mutex_.Acquire();
        inner_sink_.log(msg);
    }

    void set_formatter_(
        std::unique_ptr<spdlog::formatter> sink_formatter) override
    {
        base_sink<spdlog::details::null_mutex>::set_formatter_(
            std::move(sink_formatter));
        inner_sink_.set_formatter(
            base_sink<spdlog::details::null_mutex>::formatter_->clone());
    }

    void flush_() override
    {
        auto lock = cross_process_mutex_.Acquire();
        inner_sink_.flush();
    }

public:
    ProcessSafeStdoutSink() = default;

    ~ProcessSafeStdoutSink() override = default;
};

// 检测 PID 是否存活。本函数位于 g_logger 静态初始化的日志清理路径上——静态
// 初始化在 DLL 加载（加载器锁）内执行，禁止使用任何会创建线程的实现：
// boost::process::v2::process 携带 asio executor，首次构造会创建全局
// system_context 线程池；新线程启动需要获取加载器锁，与等待线程就绪的
// 静态初始化线程互等，进程永久挂起。因此这里只允许纯系统调用。
bool IsProcessAlive(unsigned long long pid)
{
    if (pid == 0)
    {
        return false;
    }
#if defined(_WIN32) || defined(__CYGWIN__)
    const HANDLE process_handle = ::OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        static_cast<DWORD>(pid));
    if (process_handle == nullptr)
    {
        return false;
    }
    DWORD      exit_code = 0;
    const BOOL query_succeeded =
        ::GetExitCodeProcess(process_handle, &exit_code);
    ::CloseHandle(process_handle);
    return query_succeeded != FALSE && exit_code == STILL_ACTIVE;
#else
    const int probe_result = ::kill(static_cast<pid_t>(pid), 0);
    if (probe_result == 0)
    {
        return true;
    }
    // EPERM：目标进程存在但属于其他用户，同样视为存活；ESRCH 才是已退出。
    return errno == EPERM;
#endif
}

// 构造当前进程的日志文件路径：logs/<进程exe名>-<YYYYMMDD_HH-MM-SS>-<PID>.log。
// 进程名取自 boost::dll::program_location()（与 DasDebug 崩溃 dump 命名一致），
// 每个进程独立文件名，避免多进程并发写入同一文件互相覆盖。
std::string MakeProcessLogPath()
{
    const std::string process_stem =
        boost::dll::program_location().stem().string();

    const std::time_t now = std::time(nullptr);
    std::tm           local_time{};
#if defined(_WIN32) || defined(__CYGWIN__)
    localtime_s(&local_time, &now);
#else
    localtime_r(&now, &local_time);
#endif
    char time_buffer[24];
    std::strftime(
        time_buffer,
        sizeof(time_buffer),
        "%Y%m%d_%H-%M-%S",
        &local_time);

    return DAS::fmt::format(
        "logs/{}-{}-{}.log",
        process_stem,
        time_buffer,
#if defined(_WIN32) || defined(__CYGWIN__)
        static_cast<unsigned long long>(::GetCurrentProcessId()));
#else
        static_cast<unsigned long long>(::getpid()));
#endif
}

// 删除超过 max_age_days 且进程已退出的旧日志；活跃进程的日志不删。best-effort。
void CleanupStaleProcessLogs(
    const std::filesystem::path& log_dir,
    int                          max_age_days)
{
    std::error_code ec;
    if (!std::filesystem::exists(log_dir, ec))
    {
        return;
    }

    const std::regex name_pattern(
        R"(^(.+)-(\d{8}_\d{2}-\d{2}-\d{2})-(\d+)\.log(?:\.\d+)?$)");
    const std::time_t now = std::time(nullptr);
    const std::time_t cutoff =
        now - static_cast<std::time_t>(max_age_days) * 86400;

    for (const auto& entry : std::filesystem::directory_iterator(log_dir, ec))
    {
        if (ec)
        {
            break;
        }
        if (!entry.is_regular_file())
        {
            continue;
        }

        const std::string name = entry.path().filename().string();
        std::smatch       match;
        if (!std::regex_match(name, match, name_pattern))
        {
            continue;
        }

        std::tm            file_time{};
        std::istringstream time_stream(match[2].str());
        time_stream >> std::get_time(&file_time, "%Y%m%d_%H-%M-%S");
        if (time_stream.fail())
        {
            continue;
        }
        const std::time_t file_epoch = std::mktime(&file_time);

        unsigned long long pid = 0;
        try
        {
            pid = std::stoull(match[3].str());
        }
        catch (...)
        {
            continue;
        }

        if (file_epoch < cutoff && !IsProcessAlive(pid))
        {
            std::filesystem::remove(entry.path(), ec); // best-effort，失败忽略
        }
    }
}
DAS_NS_ANONYMOUS_DETAILS_END

DAS_NS_BEGIN

namespace Core
{
    const std::shared_ptr<spdlog::logger> g_logger = []()
    {
        const std::string log_path = ::Details::MakeProcessLogPath();
        // 启动时清理：删除超期（默认 14 天）且对应进程已退出的旧日志，
        // 控制累计日志量。活跃进程的日志不会被删。
        ::Details::CleanupStaleProcessLogs(std::filesystem::path{"logs"}, 14);

        const auto std_sink =
            std::make_shared<::Details::ProcessSafeStdoutSink>();
        const auto file_sink =
            std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_path,
                50 * 1024 * 1024, // 50mb
                2);
        // DasLogNotifierSink：统一日志通知 sink（借用引用语义），多继承
        // base_sink<null_mutex>
        // + IDasLogNotifier。挂在此处与 stdout/file_sink
        // 并列；进程级固定，运行时永不动 g_logger->sinks() vector（Pitfall
        // #2：vector 固定使 sink_it_ 的无锁迭代安全）。
        const auto notifier_sink = std::make_shared<DasLogNotifierSink>();
        const auto sinks =
            std::array<spdlog::sink_ptr, 3>{std_sink, file_sink, notifier_sink};
        const auto result = std::make_shared<spdlog::logger>(
            g_logger_name,
            std::begin(sinks),
            std::end(sinks));
        spdlog::register_logger(result);
        spdlog::set_pattern(
            "[%Y-%m-%d %H:%M:%S.%e][%P][%t][%^%l%$][%!()][%s:%#][%i] %v");

        // 启动级别 env 门控（DAS_LOG_LEVEL = trace/debug/info/warn/error/
        // off，大小写无关；缺省 info）：诊断密度按排查需求显式打开。历史
        // 默认 trace 使每次排查期加入热路径的调试日志在常规运行中全量
        // 计税（格式化 + stdout + 滚动文件 + notifier 三路 sink）
        spdlog::level::level_enum startup_level = spdlog::level::info;
        if (const char* requested_level = std::getenv("DAS_LOG_LEVEL"))
        {
            const std::string_view normalized{requested_level};
            auto                   matches = [normalized](std::string_view name)
            {
                return normalized.size() == name.size()
                       && std::equal(
                           normalized.begin(),
                           normalized.end(),
                           name.begin(),
                           [](char a, char b)
                           {
                               return std::tolower(
                                          static_cast<unsigned char>(a))
                                      == b;
                           });
            };
            if (matches("trace"))
            {
                startup_level = spdlog::level::trace;
            }
            else if (matches("debug"))
            {
                startup_level = spdlog::level::debug;
            }
            else if (matches("info"))
            {
                startup_level = spdlog::level::info;
            }
            else if (matches("warn") || matches("warning"))
            {
                startup_level = spdlog::level::warn;
            }
            else if (matches("error"))
            {
                startup_level = spdlog::level::err;
            }
            else if (matches("off"))
            {
                startup_level = spdlog::level::off;
            }
        }

        spdlog::set_level(startup_level);

        DAS_CONFIG_WIN32_CONSOLE;

        SPDLOG_LOGGER_INFO(result, "The logger has been initialized.");

        return result;
    }();

    const char* const g_logger_name = "das_core_g_logger";

    TraceScope::TraceScope(
        const char* const file,
        int               line,
        const char* const func)
        : file_{file}, line_{line}, func_{func}
    {
        g_logger->log(
            spdlog::source_loc{file_, line_, func_},
            spdlog::level::trace,
            "In.");
    }

    TraceScope::~TraceScope()
    {
        g_logger->log(
            spdlog::source_loc{file_, line_, func_},
            spdlog::level::trace,
            "Out.");
    }
}

DAS_NS_END
