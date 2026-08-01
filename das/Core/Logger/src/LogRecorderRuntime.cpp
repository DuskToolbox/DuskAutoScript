#define _CRT_SECURE_NO_WARNINGS

#include <das/Core/Logger/LogRecorderRuntime.h>

#include <das/Core/Logger/Logger.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#ifdef DAS_WINDOWS
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#include <unistd.h>
#endif

namespace Das::Core
{
    namespace
    {
        struct RuntimeState
        {
            std::mutex            mutex;
            bool                  initialized{false};
            std::filesystem::path session_dir;
            // 进程内图片序号，跨 recorder 实例共享（D-88-05a，不设上限）。
            std::atomic<uint32_t> image_seq{0};
        };

        RuntimeState& State()
        {
            static RuntimeState state;
            return state;
        }

        uint64_t CurrentProcessId()
        {
#ifdef DAS_WINDOWS
            return static_cast<uint64_t>(::GetCurrentProcessId());
#else
            return static_cast<uint64_t>(::getpid());
#endif
        }

        // pid 是否存活；当前进程 pid 恒为存活。
        bool ProcessAlive(uint64_t pid)
        {
#ifdef DAS_WINDOWS
            if (pid == CurrentProcessId())
            {
                return true;
            }
            HANDLE handle = ::OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                static_cast<DWORD>(pid));
            if (handle == nullptr)
            {
                return false; // 打不开即视为不存在/已死
            }
            DWORD      exit_code = 0;
            const bool ok = ::GetExitCodeProcess(handle, &exit_code) != FALSE;
            ::CloseHandle(handle);
            return ok && exit_code == STILL_ACTIVE;
#else
            if (pid == CurrentProcessId())
            {
                return true;
            }
            return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
        }

        // 从会话目录名解析 pid；目录名形如 `20260801-123456-8841`（三段全
        // 数字，前两段 8/6 位时间戳，末段 pid）。格式不符返回 nullopt。
        std::optional<uint64_t> ParseSessionPid(const std::string& name)
        {
            std::vector<std::string> parts;
            std::string              current;
            for (char c : name)
            {
                if (c == '-')
                {
                    if (current.empty())
                    {
                        return std::nullopt;
                    }
                    parts.push_back(std::move(current));
                    current.clear();
                }
                else
                {
                    if (c < '0' || c > '9')
                    {
                        return std::nullopt;
                    }
                    current.push_back(c);
                }
            }
            if (current.empty())
            {
                return std::nullopt;
            }
            parts.push_back(std::move(current));
            if (parts.size() != 3 || parts[0].size() != 8
                || parts[1].size() != 6)
            {
                return std::nullopt;
            }
            uint64_t pid = 0;
            for (char c : parts[2])
            {
                pid = pid * 10 + static_cast<uint64_t>(c - '0');
                if (pid > static_cast<uint64_t>(UINT32_MAX))
                {
                    return std::nullopt;
                }
            }
            return pid;
        }

        // 会话目录名 {ts}-{pid}，ts = 本地时间 %Y%m%d-%H%M%S。
        std::string SessionDirName()
        {
            const auto now = std::chrono::system_clock::now();
            const auto time = std::chrono::system_clock::to_time_t(now);
            std::tm    local{};
#ifdef DAS_WINDOWS
            localtime_s(&local, &time);
#else
            localtime_r(&time, &local);
#endif
            char buffer[32]{};
            std::strftime(buffer, sizeof buffer, "%Y%m%d-%H%M%S", &local);
            return std::string{buffer} + "-"
                   + std::to_string(CurrentProcessId());
        }
    } // namespace

    DasResult LogRecorderRuntime::Initialize(
        const std::filesystem::path& debug_dir)
    {
        auto&                  state = State();
        const std::scoped_lock lock{state.mutex};

        if (state.initialized)
        {
            return DAS_S_OK;
        }

        // D-88-05b：每次 DasHttp 启动先清理上次异常退出残留的孤儿会话目录，
        // 再创建本次会话目录。本函数只操作文件系统，不触碰共享状态。
        CleanupOrphanSessionDirs(debug_dir);

        const auto resolved =
            std::filesystem::absolute(debug_dir).lexically_normal();
        state.session_dir = resolved / SessionDirName();

        std::error_code ec;
        std::filesystem::create_directories(state.session_dir / "img", ec);
        if (ec)
        {
            DAS_CORE_LOG_ERROR(
                "Failed to create session dir: {}",
                ec.message());
            return DAS_E_INVALID_PATH;
        }

        state.initialized = true;
        return DAS_S_OK;
    }

    std::filesystem::path LogRecorderRuntime::SessionDir()
    {
        auto&                  state = State();
        const std::scoped_lock lock{state.mutex};
        return state.initialized ? state.session_dir : std::filesystem::path{};
    }

    std::filesystem::path LogRecorderRuntime::UserJsonlPath()
    {
        auto&                  state = State();
        const std::scoped_lock lock{state.mutex};
        return state.initialized ? state.session_dir / "user.jsonl"
                                 : std::filesystem::path{};
    }

    uint32_t LogRecorderRuntime::NextImageSeq()
    {
        auto&                  state = State();
        const std::scoped_lock lock{state.mutex};
        return state.image_seq.fetch_add(1) + 1;
    }

    void LogRecorderRuntime::CleanupOrphanSessionDirs(
        const std::filesystem::path& debug_dir)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(debug_dir, ec) || ec)
        {
            return;
        }
        for (const auto& entry :
             std::filesystem::directory_iterator(debug_dir, ec))
        {
            if (ec)
            {
                break;
            }
            if (!entry.is_directory(ec) || ec)
            {
                continue;
            }
            const auto name = entry.path().filename().string();
            const auto pid = ParseSessionPid(name);
            if (!pid)
            {
                continue; // 不是 {ts}-{pid} 会话目录，跳过
            }
            if (ProcessAlive(*pid))
            {
                continue;
            }
            std::error_code remove_ec;
            std::filesystem::remove_all(entry.path(), remove_ec);
            if (remove_ec)
            {
                DAS_CORE_LOG_WARN(
                    "CleanupOrphanSessionDirs: failed to remove {}: {}",
                    entry.path().string(),
                    remove_ec.message());
            }
            else
            {
                DAS_CORE_LOG_INFO(
                    "CleanupOrphanSessionDirs: removed orphan session dir {}",
                    entry.path().string());
            }
        }
    }

    void LogRecorderRuntime::Shutdown()
    {
        auto&                  state = State();
        const std::scoped_lock lock{state.mutex};

        if (!state.initialized)
        {
            return;
        }

        std::error_code ec;
        std::filesystem::remove_all(state.session_dir, ec);
        if (ec)
        {
            DAS_CORE_LOG_WARN("Failed to remove session dir: {}", ec.message());
        }
        state.session_dir.clear();
        state.initialized = false;
    }

    void LogRecorderRuntime::ResetForTest()
    {
        auto&                  state = State();
        const std::scoped_lock lock{state.mutex};
        state.initialized = false;
        state.session_dir.clear();
        state.image_seq.store(0);
    }

} // namespace Das::Core
