#include "AgentRuntimeService.h"

#include "AgentRuntimeRequest.h"
#include "MaaHandle.h"

#include <das/DasApi.h>
#include <das/_autogen/idl/abi/DasLogger.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Das::Plugins::DasMaaPi::AgentRuntime
{
    namespace
    {
        std::size_t MaxTailBytes(const AgentRuntimeOptionsDto& options)
        {
            return options.max_output_tail_bytes <= 0
                       ? 0U
                       : static_cast<std::size_t>(
                             options.max_output_tail_bytes);
        }

        std::string Tail(std::string text, std::size_t max_bytes)
        {
            if (max_bytes == 0)
            {
                return {};
            }
            if (text.size() <= max_bytes)
            {
                return text;
            }
            return text.substr(text.size() - max_bytes);
        }

        bool StartsWithPi(std::string_view value)
        {
            return value.starts_with("PI_");
        }

        void LogError(std::string_view message)
        {
            const std::string owned(message);
            DAS_LOG_ERROR(owned.c_str());
        }

        AgentDiagnosticDto Diagnostic(
            std::string                code,
            std::string                message,
            std::optional<std::string> agent_id = std::nullopt,
            std::optional<std::string> path = std::nullopt)
        {
            return AgentDiagnosticDto{
                .severity = "error",
                .code = std::move(code),
                .message = std::move(message),
                .agent_id = std::move(agent_id),
                .path = std::move(path)};
        }

        AgentRuntimeResultDto ResultEnvelope(
            std::string                     status,
            std::optional<std::string>      session_id,
            std::vector<AgentStateDto>      agents,
            std::vector<AgentDiagnosticDto> diagnostics,
            bool                            timed_out = false)
        {
            AgentRuntimeResultDto result;
            result.status = std::move(status);
            result.session_id = std::move(session_id);
            result.agents = std::move(agents);
            result.diagnostics = std::move(diagnostics);
            result.signals.succeeded = result.status == "succeeded";
            result.signals.failed = result.status != "succeeded";
            result.signals.timed_out = timed_out;
            result.outputs.agent_session_id = result.session_id;
            result.outputs.running_agent_count =
                static_cast<int32_t>(std::count_if(
                    result.agents.begin(),
                    result.agents.end(),
                    [](const AgentStateDto& agent)
                    { return agent.state == "running"; }));
            return result;
        }

        bool IsBareCommand(const std::string& value)
        {
            if (value.find('/') != std::string::npos
                || value.find('\\') != std::string::npos)
            {
                return false;
            }

            const std::filesystem::path path(value);
            return !path.empty() && !path.has_parent_path()
                   && !path.has_root_name() && !path.has_root_directory();
        }

        std::filesystem::path ResolveChildExec(
            const std::string& child_exec,
            const std::string& interface_directory)
        {
            const std::filesystem::path exec_path(child_exec);
            if (IsBareCommand(child_exec))
            {
                return exec_path;
            }
            if (exec_path.is_absolute() || exec_path.has_root_name()
                || exec_path.has_root_directory())
            {
                return exec_path.lexically_normal();
            }
            return (std::filesystem::path(interface_directory) / exec_path)
                .lexically_normal();
        }

        std::vector<PiEnvVarDto> FilterLaunchEnvironment(
            const AgentRuntimeRequestDto& request)
        {
            auto env = BuildLaunchEnvironment(request);
            env.erase(
                std::remove_if(
                    env.begin(),
                    env.end(),
                    [](const PiEnvVarDto& item)
                    { return !StartsWithPi(item.key); }),
                env.end());
            return env;
        }

        std::chrono::milliseconds StopTimeout(
            const AgentRuntimeOptionsDto& options)
        {
            if (options.stop_timeout_ms <= 0)
            {
                return std::chrono::milliseconds(0);
            }
            return std::chrono::milliseconds(options.stop_timeout_ms);
        }
    } // namespace

    class AgentRuntimeService::Impl final
    {
    public:
        Impl(IMaaApiBoundary& boundary, IAgentProcessRunner& runner)
            : boundary_(boundary), runner_(runner)
        {
        }

        ~Impl() { Shutdown(); }

        AgentRuntimeResultDto Start(
            const AgentRuntimeRequestDto& request,
            const AgentRuntimeMaaContext& context)
        {
            std::lock_guard lock(mutex_);

            std::vector<SessionAgent> staged_agents;
            const auto                session_id =
                "agent-session-" + std::to_string(next_session_id_++);
            const auto env = FilterLaunchEnvironment(request);

            for (std::size_t index = 0; index < request.agents.size(); ++index)
            {
                const auto&  spec = request.agents[index];
                SessionAgent agent;
                agent.agent_id = "agent-" + std::to_string(index);

                if (spec.child_exec.empty())
                {
                    auto diagnostic = Diagnostic(
                        "missing-child-exec",
                        "agent childExec must not be empty",
                        agent.agent_id,
                        "agents." + std::to_string(index) + ".childExec");
                    return FailStart(
                        staged_agents,
                        &agent,
                        request.options,
                        std::move(diagnostic));
                }

                const auto client = CreateAgentClient(spec, request.options);
                if (client == kInvalidMaaAgentClientHandle)
                {
                    auto diagnostic = Diagnostic(
                        "create-agent-client-failed",
                        "Maa AgentClient creation failed",
                        agent.agent_id);
                    return FailStart(
                        staged_agents,
                        &agent,
                        request.options,
                        std::move(diagnostic));
                }
                agent.client.reset(boundary_, client);

                if (auto bind = boundary_.BindAgentClientResource(
                        agent.client.get(),
                        context.resource);
                    !bind.ok)
                {
                    auto diagnostic = Diagnostic(
                        "bind-agent-resource-failed",
                        bind.message,
                        agent.agent_id);
                    return FailStart(
                        staged_agents,
                        &agent,
                        request.options,
                        std::move(diagnostic));
                }

                agent.identifier =
                    boundary_.GetAgentClientIdentifier(agent.client.get());
                if (!agent.identifier || agent.identifier->empty())
                {
                    auto diagnostic = Diagnostic(
                        "agent-identifier-failed",
                        "Maa AgentClient did not provide an identifier",
                        agent.agent_id);
                    return FailStart(
                        staged_agents,
                        &agent,
                        request.options,
                        std::move(diagnostic));
                }

                if (auto timeout = boundary_.SetAgentClientTimeout(
                        agent.client.get(),
                        spec.timeout_ms);
                    !timeout.ok)
                {
                    auto diagnostic = Diagnostic(
                        "set-agent-timeout-failed",
                        timeout.message,
                        agent.agent_id);
                    return FailStart(
                        staged_agents,
                        &agent,
                        request.options,
                        std::move(diagnostic));
                }

                AgentProcessLaunchRequest launch;
                launch.agent_id = agent.agent_id;
                launch.executable = ResolveChildExec(
                    spec.child_exec,
                    request.interface_directory);
                launch.working_directory = request.interface_directory;
                launch.arguments = spec.child_args;
                launch.arguments.push_back(*agent.identifier);
                launch.environment = env;
                launch.capture_output = request.options.capture_output;
                launch.max_output_tail_bytes = MaxTailBytes(request.options);

                auto launched = runner_.Launch(launch);
                if (!launched.ok || !launched.process)
                {
                    auto diagnostic = Diagnostic(
                        "launch-agent-process-failed",
                        launched.message.empty()
                            ? "Agent child process launch failed"
                            : launched.message,
                        agent.agent_id,
                        launch.executable.generic_string());
                    return FailStart(
                        staged_agents,
                        &agent,
                        request.options,
                        std::move(diagnostic));
                }
                agent.process = std::move(launched.process);

                if (auto connect =
                        boundary_.ConnectAgentClient(agent.client.get());
                    !connect.ok)
                {
                    auto diagnostic = Diagnostic(
                        "connect-agent-client-failed",
                        connect.message,
                        agent.agent_id);
                    return FailStart(
                        staged_agents,
                        &agent,
                        request.options,
                        std::move(diagnostic));
                }

                if (auto sink = boundary_.RegisterAgentClientResourceSink(
                        agent.client.get(),
                        context.resource);
                    !sink.ok)
                {
                    auto diagnostic = Diagnostic(
                        "register-resource-sink-failed",
                        sink.message,
                        agent.agent_id);
                    return FailStart(
                        staged_agents,
                        &agent,
                        request.options,
                        std::move(diagnostic));
                }
                if (auto sink = boundary_.RegisterAgentClientControllerSink(
                        agent.client.get(),
                        context.controller);
                    !sink.ok)
                {
                    auto diagnostic = Diagnostic(
                        "register-controller-sink-failed",
                        sink.message,
                        agent.agent_id);
                    return FailStart(
                        staged_agents,
                        &agent,
                        request.options,
                        std::move(diagnostic));
                }
                if (auto sink = boundary_.RegisterAgentClientTaskerSink(
                        agent.client.get(),
                        context.tasker);
                    !sink.ok)
                {
                    auto diagnostic = Diagnostic(
                        "register-tasker-sink-failed",
                        sink.message,
                        agent.agent_id);
                    return FailStart(
                        staged_agents,
                        &agent,
                        request.options,
                        std::move(diagnostic));
                }

                staged_agents.push_back(std::move(agent));
            }

            SessionEntry session;
            session.session_id = session_id;
            session.options = request.options;
            session.agents = std::move(staged_agents);
            auto result = BuildResult(session, "succeeded");
            sessions_.emplace(session.session_id, std::move(session));
            return result;
        }

        AgentRuntimeResultDto Stop(
            std::string_view              session_id,
            const AgentRuntimeOptionsDto& options)
        {
            std::lock_guard lock(mutex_);
            auto            it = sessions_.find(std::string(session_id));
            if (it == sessions_.end())
            {
                return MissingSession(session_id);
            }

            auto& session = it->second;
            session.options = options;
            bool timed_out = false;
            for (auto& agent : session.agents)
            {
                timed_out =
                    CleanupAgent(agent, session.options, "stopped", false)
                    || timed_out;
            }
            session.timed_out = session.timed_out || timed_out;
            session.stopped = true;
            return BuildResult(session, "succeeded", {}, session.timed_out);
        }

        AgentRuntimeResultDto Status(std::string_view session_id)
        {
            std::lock_guard lock(mutex_);
            auto            it = sessions_.find(std::string(session_id));
            if (it == sessions_.end())
            {
                return MissingSession(session_id);
            }

            auto& session = it->second;
            for (auto& agent : session.agents)
            {
                if (agent.cleaned || !agent.process)
                {
                    continue;
                }
                auto snapshot = agent.process->Snapshot();
                if (!snapshot.running)
                {
                    agent.client.reset();
                    agent.cleaned = true;
                    agent.final_state =
                        BuildAgentState(agent, session.options, "exited");
                }
            }
            return BuildResult(session, "succeeded", {}, session.timed_out);
        }

    private:
        struct SessionAgent
        {
            std::string                    agent_id;
            std::optional<std::string>     identifier;
            ScopedAgentClient              client;
            std::unique_ptr<IAgentProcess> process;
            bool                           cleaned = false;
            AgentStateDto                  final_state;
        };

        struct SessionEntry
        {
            std::string               session_id;
            AgentRuntimeOptionsDto    options;
            std::vector<SessionAgent> agents;
            bool                      stopped = false;
            bool                      timed_out = false;
        };

        MaaAgentClientHandle CreateAgentClient(
            const AgentSpecDto&           spec,
            const AgentRuntimeOptionsDto& options)
        {
            if (options.tcp_compat_mode)
            {
                return boundary_.CreateAgentClientTcp(0);
            }
            if (spec.identifier && !spec.identifier->empty())
            {
                return boundary_.CreateAgentClientV2(
                    std::string_view(*spec.identifier));
            }
            return boundary_.CreateAgentClientV2(std::nullopt);
        }

        AgentRuntimeResultDto FailStart(
            std::vector<SessionAgent>&    staged_agents,
            SessionAgent*                 current_agent,
            const AgentRuntimeOptionsDto& options,
            AgentDiagnosticDto            diagnostic)
        {
            LogError(diagnostic.message);
            std::vector<AgentStateDto> states;
            bool                       timed_out = false;
            for (auto& agent : staged_agents)
            {
                timed_out =
                    CleanupAgent(agent, options, "stopped", true) || timed_out;
                states.push_back(agent.final_state);
            }
            if (current_agent != nullptr)
            {
                timed_out =
                    CleanupAgent(*current_agent, options, "failed", true)
                    || timed_out;
                states.push_back(current_agent->final_state);
            }
            return ResultEnvelope(
                "failed",
                std::nullopt,
                std::move(states),
                {std::move(diagnostic)},
                timed_out);
        }

        bool CleanupAgent(
            SessionAgent&                 agent,
            const AgentRuntimeOptionsDto& options,
            std::string_view              final_state,
            bool                          force_terminate)
        {
            if (agent.cleaned)
            {
                return false;
            }

            agent.client.reset();

            bool timed_out = false;
            if (agent.process)
            {
                auto snapshot = agent.process->Snapshot();
                if (snapshot.running)
                {
                    if (force_terminate)
                    {
                        agent.process->Terminate();
                        agent.process->WaitForExit(StopTimeout(options));
                    }
                    else if (!agent.process->WaitForExit(StopTimeout(options)))
                    {
                        timed_out = true;
                        agent.process->Terminate();
                        agent.process->WaitForExit(StopTimeout(options));
                    }
                }
            }

            agent.cleaned = true;
            agent.final_state = BuildAgentState(agent, options, final_state);
            return timed_out;
        }

        AgentStateDto BuildAgentState(
            const SessionAgent&             agent,
            const AgentRuntimeOptionsDto&   options,
            std::optional<std::string_view> forced_state = std::nullopt) const
        {
            AgentProcessSnapshot snapshot;
            if (agent.process)
            {
                snapshot = agent.process->Snapshot();
            }

            std::string state;
            if (forced_state)
            {
                state = std::string(*forced_state);
            }
            else if (agent.cleaned)
            {
                state = agent.final_state.state.empty()
                            ? "stopped"
                            : agent.final_state.state;
            }
            else
            {
                state = snapshot.running ? "running" : "exited";
            }

            const auto max_bytes = MaxTailBytes(options);
            return AgentStateDto{
                .agent_id = agent.agent_id,
                .state = std::move(state),
                .identifier = agent.identifier,
                .pid = snapshot.pid,
                .exit_code = snapshot.exit_code,
                .stdout_tail = Tail(std::move(snapshot.stdout_tail), max_bytes),
                .stderr_tail =
                    Tail(std::move(snapshot.stderr_tail), max_bytes)};
        }

        AgentRuntimeResultDto BuildResult(
            const SessionEntry&             session,
            std::string                     status,
            std::vector<AgentDiagnosticDto> diagnostics = {},
            bool                            timed_out = false) const
        {
            std::vector<AgentStateDto> states;
            states.reserve(session.agents.size());
            for (const auto& agent : session.agents)
            {
                states.push_back(BuildAgentState(agent, session.options));
            }
            return ResultEnvelope(
                std::move(status),
                session.session_id,
                std::move(states),
                std::move(diagnostics),
                timed_out);
        }

        AgentRuntimeResultDto MissingSession(std::string_view session_id) const
        {
            return ResultEnvelope(
                "failed",
                std::nullopt,
                {},
                {Diagnostic(
                    "agent-session-not-found",
                    "Agent runtime session was not found",
                    std::nullopt,
                    std::string(session_id))});
        }

        void Shutdown()
        {
            std::lock_guard lock(mutex_);
            for (auto& [_, session] : sessions_)
            {
                bool timed_out = false;
                for (auto& agent : session.agents)
                {
                    timed_out =
                        CleanupAgent(agent, session.options, "stopped", false)
                        || timed_out;
                }
                session.timed_out = session.timed_out || timed_out;
                session.stopped = true;
            }
        }

        IMaaApiBoundary&                    boundary_;
        IAgentProcessRunner&                runner_;
        std::mutex                          mutex_;
        std::map<std::string, SessionEntry> sessions_;
        std::uint64_t                       next_session_id_ = 1;
    };

    AgentRuntimeService::AgentRuntimeService(
        IMaaApiBoundary&     boundary,
        IAgentProcessRunner& runner)
        : impl_(std::make_unique<Impl>(boundary, runner))
    {
    }

    AgentRuntimeService::~AgentRuntimeService() = default;

    AgentRuntimeResultDto AgentRuntimeService::Start(
        const AgentRuntimeRequestDto& request,
        const AgentRuntimeMaaContext& context)
    {
        return impl_->Start(request, context);
    }

    AgentRuntimeResultDto AgentRuntimeService::Stop(
        std::string_view              session_id,
        const AgentRuntimeOptionsDto& options)
    {
        return impl_->Stop(session_id, options);
    }

    AgentRuntimeResultDto AgentRuntimeService::Status(
        std::string_view session_id)
    {
        return impl_->Status(session_id);
    }
} // namespace Das::Plugins::DasMaaPi::AgentRuntime
