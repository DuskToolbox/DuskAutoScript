#include "../src/AgentRuntimeService.h"
#include "../src/MaaHandle.h"
#include "FakeMaaApiBoundary.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using namespace Das::Plugins::DasMaaPi;
    using namespace Das::Plugins::DasMaaPi::AgentRuntime;
    using namespace Das::Plugins::DasMaaPi::Test;
    using namespace std::chrono_literals;

    struct FakeProcessState
    {
        uint32_t               pid = 0;
        bool                   running = true;
        bool                   terminated = false;
        int                    wait_calls = 0;
        int                    terminate_calls = 0;
        int                    wait_failures_remaining = 0;
        std::optional<int32_t> exit_code;
        std::string            stdout_text;
        std::string            stderr_text;
    };

    class FakeProcess final : public IAgentProcess
    {
    public:
        explicit FakeProcess(std::shared_ptr<FakeProcessState> state)
            : state_(std::move(state))
        {}

        AgentProcessSnapshot Snapshot() const override
        {
            return AgentProcessSnapshot{
                .running = state_->running,
                .pid = state_->pid,
                .exit_code = state_->exit_code,
                .stdout_tail = state_->stdout_text,
                .stderr_tail = state_->stderr_text};
        }

        bool WaitForExit(std::chrono::milliseconds) override
        {
            ++state_->wait_calls;
            if (state_->wait_failures_remaining > 0)
            {
                --state_->wait_failures_remaining;
                return false;
            }
            state_->running = false;
            if (!state_->exit_code)
            {
                state_->exit_code = 0;
            }
            return true;
        }

        void Terminate() override
        {
            ++state_->terminate_calls;
            state_->terminated = true;
        }

    private:
        std::shared_ptr<FakeProcessState> state_;
    };

    class FakeProcessRunner final : public IAgentProcessRunner
    {
    public:
        AgentProcessLaunchResult Launch(
            const AgentProcessLaunchRequest& request) override
        {
            launches.push_back(request);
            const auto index = launches.size() - 1;
            if (fail_launch_index && *fail_launch_index == index)
            {
                return AgentProcessLaunchResult::Failure("spawn failed");
            }

            auto state = std::make_shared<FakeProcessState>();
            state->pid = static_cast<uint32_t>(5000 + index);
            state->stdout_text = next_stdout;
            state->stderr_text = next_stderr;
            state->wait_failures_remaining = next_wait_failures;
            auto process = std::make_unique<FakeProcess>(state);
            processes.push_back(std::move(state));
            return AgentProcessLaunchResult::Success(std::move(process));
        }

        std::vector<AgentProcessLaunchRequest> launches;
        std::vector<std::shared_ptr<FakeProcessState>> processes;
        std::optional<std::size_t>             fail_launch_index;
        std::string                            next_stdout;
        std::string                            next_stderr;
        int                                    next_wait_failures = 0;
    };

    AgentRuntimeMaaContext TestContext()
    {
        return AgentRuntimeMaaContext{
            .resource = 10,
            .controller = 11,
            .tasker = 12};
    }

    AgentRuntimeRequestDto StartRequest(
        std::vector<AgentSpecDto> agents = {AgentSpecDto{
            .child_exec = "python",
            .child_args = {"./agent/main.py"},
            .identifier = std::nullopt,
            .timeout_ms = 9000}})
    {
        AgentRuntimeRequestDto request;
        request.operation = "start";
        request.interface_directory = "C:/maa/project";
        request.agents = std::move(agents);
        request.pi_env.client_name = "DAS";
        request.pi_env.client_version = "0.1";
        request.pi_env.project_version = "project";
        request.extra_pi_env.push_back(PiEnvVarDto{
            .key = "PI_TRACE",
            .value = "1"});
        request.extra_pi_env.push_back(PiEnvVarDto{
            .key = "PATH",
            .value = "must-not-launch"});
        return request;
    }

    const PiEnvVarDto* FindEnv(
        const std::vector<PiEnvVarDto>& env,
        std::string_view                key)
    {
        const auto it = std::find_if(
            env.begin(),
            env.end(),
            [key](const PiEnvVarDto& item) { return item.key == key; });
        return it == env.end() ? nullptr : &*it;
    }

    bool HasCall(
        const std::vector<std::string>& calls,
        std::string_view                expected)
    {
        return std::find(calls.begin(), calls.end(), expected) != calls.end();
    }

    TEST(DasMaaPiMaaHandle, MaaHandlesCleanupInReverseOwnershipOrder)
    {
        FakeMaaApiBoundary fake;

        {
            ScopedResource resource(fake, fake.CreateResource());
            ScopedController controller(
                fake,
                fake.CreateController(
                    ControllerSpec{.name = "Android", .type = "Adb"}));
            ScopedTasker tasker(fake, fake.CreateTasker());
        }

        const std::vector<std::string> expected{
            "CreateResource",
            "CreateController:Android:Adb",
            "CreateTasker",
            "DestroyTasker:3",
            "DestroyController:2",
            "DestroyResource:1"};
        EXPECT_EQ(fake.calls, expected);
    }

    TEST(DasMaaPiMaaHandle, InvalidMaaHandlesDoNotCallDestroy)
    {
        FakeMaaApiBoundary fake;

        {
            ScopedResource resource(fake, kInvalidMaaResourceHandle);
            ScopedController controller(fake, kInvalidMaaControllerHandle);
            ScopedTasker tasker(fake, kInvalidMaaTaskerHandle);
            ScopedAgentClient client(fake, kInvalidMaaAgentClientHandle);
        }

        EXPECT_TRUE(fake.calls.empty());
    }

    TEST(DasMaaPiAgentClient, FakeBoundaryDrivesAgentClientOperations)
    {
        FakeMaaApiBoundary fake;

        const auto client = fake.CreateAgentClientV2(std::string_view("agent-1"));
        EXPECT_NE(client, kInvalidMaaAgentClientHandle);
        EXPECT_EQ(fake.GetAgentClientIdentifier(client), "agent-client-id");
        EXPECT_TRUE(fake.BindAgentClientResource(client, 10).ok);
        EXPECT_TRUE(fake.RegisterAgentClientResourceSink(client, 10).ok);
        EXPECT_TRUE(fake.RegisterAgentClientControllerSink(client, 11).ok);
        EXPECT_TRUE(fake.RegisterAgentClientTaskerSink(client, 12).ok);
        EXPECT_TRUE(fake.SetAgentClientTimeout(client, 5000).ok);
        EXPECT_TRUE(fake.ConnectAgentClient(client).ok);
        EXPECT_TRUE(fake.IsAgentClientConnected(client));
        EXPECT_TRUE(fake.IsAgentClientAlive(client));

        const std::vector<std::string> expected{
            "CreateAgentClientV2:agent-1",
            "GetAgentClientIdentifier",
            "BindAgentClientResource",
            "RegisterAgentClientResourceSink",
            "RegisterAgentClientControllerSink",
            "RegisterAgentClientTaskerSink",
            "SetAgentClientTimeout:5000",
            "ConnectAgentClient",
            "IsAgentClientConnected",
            "IsAgentClientAlive"};
        EXPECT_EQ(fake.calls, expected);
    }

    TEST(DasMaaPiAgentClient, ScopedAgentClientDisconnectsBeforeDestroy)
    {
        FakeMaaApiBoundary fake;

        {
            ScopedAgentClient client(fake, fake.CreateAgentClientTcp(0));
        }

        const std::vector<std::string> expected{
            "CreateAgentClientTcp:0",
            "DisconnectAgentClient:1",
            "DestroyAgentClient:1"};
        EXPECT_EQ(fake.calls, expected);
    }

    TEST(DasMaaPiAgentClient, RealBoundarySourceCallsAgentClientApi)
    {
        const auto source_path =
            std::filesystem::path(__FILE__).parent_path().parent_path()
            / "src" / "MaaRuntime.cpp";
        std::ifstream input(source_path);
        ASSERT_TRUE(input.is_open()) << source_path.string();

        std::ostringstream buffer;
        buffer << input.rdbuf();
        const auto source = buffer.str();

        EXPECT_NE(source.find("MaaAgentClientCreateV2"), std::string::npos);
        EXPECT_NE(source.find("MaaAgentClientCreateTcp"), std::string::npos);
        EXPECT_NE(source.find("MaaAgentClientBindResource"), std::string::npos);
        EXPECT_NE(
            source.find("MaaAgentClientRegisterResourceSink"),
            std::string::npos);
        EXPECT_NE(
            source.find("MaaAgentClientRegisterControllerSink"),
            std::string::npos);
        EXPECT_NE(
            source.find("MaaAgentClientRegisterTaskerSink"),
            std::string::npos);
        EXPECT_NE(source.find("MaaAgentClientSetTimeout"), std::string::npos);
        EXPECT_NE(source.find("MaaAgentClientConnect"), std::string::npos);
        EXPECT_NE(source.find("MaaAgentClientDisconnect"), std::string::npos);
        EXPECT_NE(source.find("MaaAgentClientDestroy"), std::string::npos);
        const std::string forbidden_stub =
            std::string("Maa AgentClient boundary is ") + "not "
            + "implemented";
        EXPECT_EQ(source.find(forbidden_stub), std::string::npos);
    }

    TEST(DasMaaPiAgentRuntimeStart, StartsSingleAgentWithIdentifierArgCwdAndPiEnv)
    {
        FakeMaaApiBoundary fake;
        FakeProcessRunner  runner;
        AgentRuntimeService service(fake, runner);

        auto result = service.Start(StartRequest(), TestContext());

        ASSERT_EQ(result.status, "succeeded");
        ASSERT_TRUE(result.session_id.has_value());
        EXPECT_EQ(result.outputs.agent_session_id, result.session_id);
        EXPECT_EQ(result.outputs.running_agent_count, 1);
        ASSERT_EQ(result.agents.size(), 1u);
        EXPECT_EQ(result.agents[0].state, "running");
        EXPECT_EQ(result.agents[0].identifier, "agent-client-id");
        ASSERT_EQ(runner.launches.size(), 1u);
        EXPECT_EQ(runner.launches[0].executable.generic_string(), "python");
        EXPECT_EQ(
            runner.launches[0].working_directory.generic_string(),
            "C:/maa/project");
        EXPECT_EQ(
            runner.launches[0].arguments,
            (std::vector<std::string>{"./agent/main.py", "agent-client-id"}));
        EXPECT_NE(FindEnv(runner.launches[0].environment, "PI_CLIENT_NAME"), nullptr);
        EXPECT_NE(FindEnv(runner.launches[0].environment, "PI_TRACE"), nullptr);
        EXPECT_EQ(FindEnv(runner.launches[0].environment, "PATH"), nullptr);
        EXPECT_TRUE(HasCall(fake.calls, "BindAgentClientResource"));
        EXPECT_TRUE(HasCall(fake.calls, "ConnectAgentClient"));
        EXPECT_TRUE(HasCall(fake.calls, "RegisterAgentClientResourceSink"));
        EXPECT_TRUE(HasCall(fake.calls, "RegisterAgentClientControllerSink"));
        EXPECT_TRUE(HasCall(fake.calls, "RegisterAgentClientTaskerSink"));
    }

    TEST(DasMaaPiAgentRuntimeRollback, LaterAgentFailureCleansStartedAgents)
    {
        FakeMaaApiBoundary fake;
        fake.agent_client_identifiers = {"agent-client-1", "agent-client-2"};
        FakeProcessRunner runner;
        runner.fail_launch_index = 1;
        AgentRuntimeService service(fake, runner);

        auto result = service.Start(
            StartRequest(
                {AgentSpecDto{.child_exec = "python"},
                 AgentSpecDto{.child_exec = "node"}}),
            TestContext());

        EXPECT_EQ(result.status, "failed");
        EXPECT_FALSE(result.session_id.has_value());
        ASSERT_EQ(runner.processes.size(), 1u);
        EXPECT_GT(runner.processes[0]->terminate_calls, 0);
        EXPECT_GT(runner.processes[0]->wait_calls, 0);
        EXPECT_TRUE(HasCall(fake.calls, "DisconnectAgentClient:1"));
        EXPECT_TRUE(HasCall(fake.calls, "DestroyAgentClient:1"));
        EXPECT_TRUE(HasCall(fake.calls, "DisconnectAgentClient:2"));
        EXPECT_TRUE(HasCall(fake.calls, "DestroyAgentClient:2"));
    }

    TEST(DasMaaPiAgentRuntimeEnvironment, RejectsNonPiEnvAtLaunchBoundary)
    {
        FakeMaaApiBoundary fake;
        FakeProcessRunner  runner;
        AgentRuntimeService service(fake, runner);

        auto result = service.Start(StartRequest(), TestContext());

        ASSERT_EQ(result.status, "succeeded");
        ASSERT_EQ(runner.launches.size(), 1u);
        EXPECT_EQ(FindEnv(runner.launches[0].environment, "PATH"), nullptr);
        EXPECT_NE(FindEnv(runner.launches[0].environment, "PI_TRACE"), nullptr);
    }

    TEST(DasMaaPiAgentRuntimeStart, ResolvesRelativeChildExecFromInterfaceDirectory)
    {
        FakeMaaApiBoundary fake;
        FakeProcessRunner  runner;
        AgentRuntimeService service(fake, runner);

        auto result = service.Start(
            StartRequest({AgentSpecDto{.child_exec = "bin/agent.exe"}}),
            TestContext());

        ASSERT_EQ(result.status, "succeeded");
        ASSERT_EQ(runner.launches.size(), 1u);
        EXPECT_EQ(
            runner.launches[0].executable.generic_string(),
            "C:/maa/project/bin/agent.exe");
    }

    TEST(DasMaaPiAgentRuntimeStart, RejectsEmptyChildExecBeforeLaunch)
    {
        FakeMaaApiBoundary fake;
        FakeProcessRunner  runner;
        AgentRuntimeService service(fake, runner);

        auto result = service.Start(
            StartRequest({AgentSpecDto{.child_exec = ""}}),
            TestContext());

        EXPECT_EQ(result.status, "failed");
        EXPECT_TRUE(runner.launches.empty());
        ASSERT_FALSE(result.diagnostics.empty());
        EXPECT_EQ(result.diagnostics.front().code, "missing-child-exec");
    }

    TEST(DasMaaPiAgentRuntimeStart, ConnectFailureTerminatesLaunchedChild)
    {
        FakeMaaApiBoundary fake;
        fake.connect_agent_client_result =
            MaaApiResult::Failure(7, "connect failed");
        FakeProcessRunner runner;
        AgentRuntimeService service(fake, runner);

        auto result = service.Start(StartRequest(), TestContext());

        EXPECT_EQ(result.status, "failed");
        ASSERT_EQ(runner.processes.size(), 1u);
        EXPECT_GT(runner.processes[0]->terminate_calls, 0);
        EXPECT_GT(runner.processes[0]->wait_calls, 0);
        EXPECT_FALSE(result.session_id.has_value());
    }

    TEST(DasMaaPiAgentRuntimeStart, SinkRegistrationFailureTerminatesLaunchedChild)
    {
        FakeMaaApiBoundary fake;
        fake.register_agent_client_tasker_sink_result =
            MaaApiResult::Failure(9, "tasker sink failed");
        FakeProcessRunner runner;
        AgentRuntimeService service(fake, runner);

        auto result = service.Start(StartRequest(), TestContext());

        EXPECT_EQ(result.status, "failed");
        ASSERT_EQ(runner.processes.size(), 1u);
        EXPECT_GT(runner.processes[0]->terminate_calls, 0);
        EXPECT_GT(runner.processes[0]->wait_calls, 0);
        EXPECT_FALSE(result.session_id.has_value());
    }

    TEST(DasMaaPiAgentRuntimeStatus, BoundsOutputTails)
    {
        FakeMaaApiBoundary fake;
        FakeProcessRunner  runner;
        runner.next_stdout = "0123456789";
        runner.next_stderr = "abcdefghij";
        AgentRuntimeService service(fake, runner);
        auto request = StartRequest();
        request.options.max_output_tail_bytes = 4;

        auto started = service.Start(request, TestContext());
        ASSERT_EQ(started.status, "succeeded");

        auto status = service.Status(*started.session_id);

        ASSERT_EQ(status.status, "succeeded");
        ASSERT_EQ(status.agents.size(), 1u);
        EXPECT_EQ(status.agents[0].stdout_tail, "6789");
        EXPECT_EQ(status.agents[0].stderr_tail, "ghij");
    }

    TEST(DasMaaPiAgentRuntimeStatus, ReportsExitedProcessFromSessionTable)
    {
        FakeMaaApiBoundary fake;
        FakeProcessRunner  runner;
        AgentRuntimeService service(fake, runner);

        auto started = service.Start(StartRequest(), TestContext());
        ASSERT_EQ(started.status, "succeeded");
        ASSERT_EQ(runner.processes.size(), 1u);
        runner.processes[0]->running = false;
        runner.processes[0]->exit_code = 42;

        auto status = service.Status(*started.session_id);

        ASSERT_EQ(status.status, "succeeded");
        ASSERT_EQ(status.agents.size(), 1u);
        EXPECT_EQ(status.agents[0].state, "exited");
        ASSERT_TRUE(status.agents[0].exit_code.has_value());
        EXPECT_EQ(*status.agents[0].exit_code, 42);
    }

    TEST(DasMaaPiAgentRuntimeStop, TimeoutKillsProcessAndRecordsFinalState)
    {
        FakeMaaApiBoundary fake;
        FakeProcessRunner  runner;
        runner.next_wait_failures = 1;
        AgentRuntimeService service(fake, runner);
        auto request = StartRequest();
        request.options.stop_timeout_ms = 1;

        auto started = service.Start(request, TestContext());
        ASSERT_EQ(started.status, "succeeded");

        auto stopped = service.Stop(*started.session_id, request.options);

        ASSERT_EQ(stopped.status, "succeeded");
        ASSERT_EQ(runner.processes.size(), 1u);
        EXPECT_EQ(runner.processes[0]->terminate_calls, 1);
        EXPECT_TRUE(stopped.signals.timed_out);
        ASSERT_EQ(stopped.agents.size(), 1u);
        EXPECT_EQ(stopped.agents[0].state, "stopped");
        EXPECT_TRUE(HasCall(fake.calls, "DisconnectAgentClient:1"));
        EXPECT_TRUE(HasCall(fake.calls, "DestroyAgentClient:1"));
    }

    TEST(DasMaaPiAgentRuntimeProcess, RealRunnerUsesBoostProcessV2AndPerChildEnvironment)
    {
        const auto source_path =
            std::filesystem::path(__FILE__).parent_path().parent_path()
            / "src" / "AgentProcessRunner.cpp";
        std::ifstream input(source_path);
        ASSERT_TRUE(input.is_open()) << source_path.string();

        std::ostringstream buffer;
        buffer << input.rdbuf();
        const auto source = buffer.str();

        EXPECT_NE(source.find("boost::process::v2::process"), std::string::npos);
        EXPECT_NE(
            source.find("boost::process::v2::process_start_dir"),
            std::string::npos);
        EXPECT_NE(
            source.find("boost::process::v2::process_stdio"),
            std::string::npos);
        EXPECT_NE(
            source.find("boost::process::v2::process_environment"),
            std::string::npos);
        EXPECT_NE(source.find("StartsWithPi(item.key)"), std::string::npos);
        EXPECT_NE(source.find("AppendBounded"), std::string::npos);
        EXPECT_EQ(source.find("environment::set("), std::string::npos);
        EXPECT_EQ(source.find("environment::unset("), std::string::npos);
    }
} // namespace
