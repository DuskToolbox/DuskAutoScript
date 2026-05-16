#pragma once

#include <das/Plugins/DasMaaPi/AgentRuntimeDto.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Das::Plugins::DasMaaPi::AgentRuntime
{
    struct AgentProcessLaunchRequest
    {
        std::string              agent_id;
        std::filesystem::path    executable;
        std::vector<std::string> arguments;
        std::filesystem::path    working_directory;
        std::vector<PiEnvVarDto> environment;
        bool                     capture_output = true;
        std::size_t              max_output_tail_bytes = 16384;
    };

    struct AgentProcessSnapshot
    {
        bool                    running = false;
        std::optional<uint32_t> pid;
        std::optional<int32_t>  exit_code;
        std::string             stdout_tail;
        std::string             stderr_tail;
    };

    class IAgentProcess
    {
    public:
        virtual ~IAgentProcess() = default;

        virtual AgentProcessSnapshot Snapshot() const = 0;
        virtual bool WaitForExit(std::chrono::milliseconds timeout) = 0;
        virtual void Terminate() = 0;
    };

    struct AgentProcessLaunchResult
    {
        bool                           ok = false;
        std::string                    message;
        std::unique_ptr<IAgentProcess> process;

        static AgentProcessLaunchResult Success(
            std::unique_ptr<IAgentProcess> process)
        {
            AgentProcessLaunchResult result;
            result.ok = true;
            result.process = std::move(process);
            return result;
        }

        static AgentProcessLaunchResult Failure(std::string message)
        {
            AgentProcessLaunchResult result;
            result.ok = false;
            result.message = std::move(message);
            return result;
        }
    };

    class IAgentProcessRunner
    {
    public:
        virtual ~IAgentProcessRunner() = default;

        virtual AgentProcessLaunchResult Launch(
            const AgentProcessLaunchRequest& request) = 0;
    };

    class BoostAgentProcessRunner final : public IAgentProcessRunner
    {
    public:
        AgentProcessLaunchResult Launch(
            const AgentProcessLaunchRequest& request) override;
    };
} // namespace Das::Plugins::DasMaaPi::AgentRuntime
