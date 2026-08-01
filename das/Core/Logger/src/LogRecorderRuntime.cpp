#define _CRT_SECURE_NO_WARNINGS

#include <das/Core/Logger/LogRecorderRuntime.h>

#include <das/Core/Logger/Logger.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

#ifdef DAS_WINDOWS
#include <windows.h>
#else
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
    }

} // namespace Das::Core
