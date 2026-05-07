#define _CRT_SECURE_NO_WARNINGS

#include <das/Core/Debug/DebugEvent.h>
#include <das/Core/Debug/DebugRuntime.h>
#include <das/Core/Debug/DebugSink.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

DAS_CORE_DEBUG_NS_BEGIN
namespace
{
    struct RuntimeState
    {
        std::mutex                         mutex;
        bool                               initialized{false};
        bool                               enabled{false};
        std::filesystem::path              debug_dir{"logs/debug"};
        std::vector<std::shared_ptr<IDebugSink>>  sinks;
        std::vector<std::shared_ptr<IDebugDrain>> drains;
    };

    RuntimeState& State()
    {
        static RuntimeState state;
        return state;
    }

    auto ResolveDebugDir(const std::filesystem::path& configured)
        -> std::filesystem::path
    {
        const auto path =
            configured.empty() ? std::filesystem::path{"logs/debug"} : configured;
        return std::filesystem::absolute(path).lexically_normal();
    }

    bool ReadEnabledSnapshot()
    {
        if (const auto* env = std::getenv("DAS_DEBUG"))
        {
            return std::string{env} == "1";
        }

#ifdef _DEBUG
        return true;
#else
        return false;
#endif
    }

    auto NowIsoString() -> std::string
    {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm    utc{};
#ifdef DAS_WINDOWS
        gmtime_s(&utc, &time);
#else
        gmtime_r(&time, &utc);
#endif

        std::ostringstream stream;
        stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
        return stream.str();
    }

} // namespace

DebugEvent MakeDebugEvent(
    std::string type,
    std::string params_json,
    std::string result_json)
{
    DebugEvent event{};
    event.type = std::move(type);
    event.timestamp = NowIsoString();
    event.params_json = std::move(params_json);
    event.result_json = std::move(result_json);
    return event;
}

DasResult DebugRuntime::Initialize(const DebugRuntimeOptions& options)
{
    auto& state = State();
    std::lock_guard lock{state.mutex};

    if (state.initialized)
    {
        return DAS_S_OK;
    }

    state.debug_dir = ResolveDebugDir(options.debug_dir);
    state.enabled = ReadEnabledSnapshot();

    if (state.enabled)
    {
        std::error_code ec;
        std::filesystem::create_directories(state.debug_dir, ec);
        if (ec)
        {
            return DAS_E_INVALID_PATH;
        }
        std::filesystem::create_directories(state.debug_dir / "img", ec);
        if (ec)
        {
            return DAS_E_INVALID_PATH;
        }
    }

    state.initialized = true;
    return DAS_S_OK;
}

bool DebugRuntime::IsEnabled()
{
    auto& state = State();
    std::lock_guard lock{state.mutex};
    return state.enabled;
}

const std::filesystem::path& DebugRuntime::DebugDir()
{
    return State().debug_dir;
}

DasResult DebugRuntime::SubmitEvent(const DebugEvent& event)
{
    std::vector<std::shared_ptr<IDebugSink>> sinks;
    {
        auto& state = State();
        std::lock_guard lock{state.mutex};
        if (!state.enabled)
        {
            return DAS_S_OK;
        }
        sinks = state.sinks;
    }

    for (const auto& sink : sinks)
    {
        if (sink)
        {
            const auto result = sink->Submit(event);
            if (result < 0)
            {
                return result;
            }
        }
    }
    return DAS_S_OK;
}

void DebugRuntime::RegisterSink(std::shared_ptr<IDebugSink> sink)
{
    if (!sink)
    {
        return;
    }

    auto& state = State();
    std::lock_guard lock{state.mutex};
    state.sinks.emplace_back(std::move(sink));
}

void DebugRuntime::RegisterDrain(std::shared_ptr<IDebugDrain> drain)
{
    if (!drain)
    {
        return;
    }

    auto& state = State();
    std::lock_guard lock{state.mutex};
    state.drains.emplace_back(std::move(drain));
}

DasResult DebugRuntime::Flush()
{
    std::vector<std::shared_ptr<IDebugSink>>  sinks;
    std::vector<std::shared_ptr<IDebugDrain>> drains;
    {
        auto& state = State();
        std::lock_guard lock{state.mutex};
        if (!state.enabled)
        {
            return DAS_S_OK;
        }
        sinks = state.sinks;
        drains = state.drains;
    }

    for (const auto& sink : sinks)
    {
        if (sink)
        {
            const auto result = sink->Flush();
            if (result < 0)
            {
                return result;
            }
        }
    }

    for (const auto& drain : drains)
    {
        if (drain)
        {
            const auto result = drain->Flush();
            if (result < 0)
            {
                return result;
            }
        }
    }
    return DAS_S_OK;
}

void DebugRuntime::Shutdown()
{
    std::vector<std::shared_ptr<IDebugSink>>  sinks;
    std::vector<std::shared_ptr<IDebugDrain>> drains;
    {
        auto& state = State();
        std::lock_guard lock{state.mutex};
        sinks = state.sinks;
        drains = state.drains;
    }

    static_cast<void>(Flush());

    for (const auto& sink : sinks)
    {
        if (sink)
        {
            sink->Shutdown();
        }
    }

    for (const auto& drain : drains)
    {
        if (drain)
        {
            drain->Shutdown();
        }
    }
}

void DebugRuntime::ResetForTest()
{
    auto& state = State();
    std::lock_guard lock{state.mutex};
    state.initialized = false;
    state.enabled = false;
    state.debug_dir = std::filesystem::path{"logs/debug"};
    state.sinks.clear();
    state.drains.clear();
}

DAS_CORE_DEBUG_NS_END
