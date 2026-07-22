#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    // Request handed to IPretaskRunner for each PI v2.7.0 pretask. The runtime
    // fills `working_directory` from the PI interface_directory so pretask
    // processes see the same relative-path layout as the project author.
    struct PretaskLaunchRequest
    {
        std::string              exec;
        std::vector<std::string> args;
        std::filesystem::path    working_directory;
    };

    // Result of a synchronous pretask run.
    //   launched=false  -> spawn failed (failure_message carries the OS error)
    //   launched=true   -> exit_code populated with the process exit code;
    //                      nonzero codes trigger pretask-nonzero-exit.
    struct PretaskRunResult
    {
        bool               launched = false;
        std::string        failure_message;
        std::optional<int> exit_code;
    };

    // Synchronous pretask runner. Distinct from AgentRuntime's async
    // IAgentProcessRunner: pretasks need a blocking spawn+wait per the
    // PI v2.7.0 protocol ("按序执行") and never require async I/O.
    class IPretaskRunner
    {
    public:
        virtual ~IPretaskRunner() = default;
        virtual PretaskRunResult Run(const PretaskLaunchRequest& request) = 0;
    };

    // Production runner backed by boost::process v2. Spawns the child in the
    // requested CWD, inherits stdio (no capture), and blocks until exit. The
    // runner is stateless and safe to share across MaaRuntime::Run invocations
    // (MaaRuntime calls Run serially per pretask — no concurrency).
    class BoostPretaskRunner final : public IPretaskRunner
    {
    public:
        PretaskRunResult Run(const PretaskLaunchRequest& request) override;
    };

    void            SetPretaskRunnerForTest(IPretaskRunner* runner);
    IPretaskRunner* PretaskRunnerForRuntime();
} // namespace Das::Plugins::DasMaaPi
