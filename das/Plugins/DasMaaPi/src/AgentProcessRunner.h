#pragma once

#include <das/Plugins/DasMaaPi/AgentRuntimeDto.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
        // 逐行分发子进程 stdout/stderr（is_stderr 区分两流）。BoostAgentProcess
        // async_read 累积 chunk → 按 '\n' 拆行 → 每完整行调一次；EOF 时残留
        // accumulator 作末行 flush。default 空：不关心的 caller 零开销。
        // 异常被 try/catch 兜底（DAS_LOG_WARNING），不逃逸 async 回调。
        std::function<void(std::string_view line, bool is_stderr)> on_output;
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

    // Detail 子 namespace：内部 helper 与公共 BoostAgentProcessRunner API
    // 分层。BuildProcessEnvironment 是 spawn 子进程环境装配纯函数，从匿名
    // namespace 提升（task 5 Open Question #1）：单测经 test-only overload
    // 注入 fixture 继承 env，避开 boost::process::v2::environment::current()
    // 在测试进程内拉真继承 env 不可控。
    namespace Detail
    {
        // 生产单参版：内部转 boost::process::v2::environment::current() 喂
        // 双参 test overload。既有 BoostAgentProcess 构造内调用零改动
        // （source-compatible 提升，命名空间限定经 ADL 命中）。
        std::vector<std::string> BuildProcessEnvironment(
            const std::vector<PiEnvVarDto>& pi_env);

        // test-only overload：单测注入 fixture 继承 env，纯函数无副作用。
        std::vector<std::string> BuildProcessEnvironment(
            const std::vector<PiEnvVarDto>& pi_env,
            const std::vector<std::pair<std::string, std::string>>&
                inherited_env);
    } // namespace Detail

    class BoostAgentProcessRunner final : public IAgentProcessRunner
    {
    public:
        AgentProcessLaunchResult Launch(
            const AgentProcessLaunchRequest& request) override;
    };
} // namespace Das::Plugins::DasMaaPi::AgentRuntime
