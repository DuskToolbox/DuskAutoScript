#include "../src/AgentRuntimeService.h"
#include "../src/MaaHandle.h"
#include "FakeMaaApiBoundary.h"

#include <das/Utils/PathText.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
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
        uint32_t                  pid = 0;
        bool                      running = true;
        bool                      terminated = false;
        int                       wait_calls = 0;
        int                       terminate_calls = 0;
        int                       wait_failures_remaining = 0;
        std::optional<int32_t>    exit_code;
        std::string               stdout_text;
        std::string               stderr_text;
        std::vector<std::string>* call_log = nullptr;
    };

    class FakeProcess final : public IAgentProcess
    {
    public:
        explicit FakeProcess(std::shared_ptr<FakeProcessState> state)
            : state_(std::move(state))
        {
        }

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
            if (state_->call_log)
            {
                state_->call_log->emplace_back("WaitForExit");
            }
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
            if (state_->call_log)
            {
                state_->call_log->emplace_back("Terminate");
            }
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
            state->call_log = call_log;
            auto process = std::make_unique<FakeProcess>(state);
            processes.push_back(std::move(state));
            return AgentProcessLaunchResult::Success(std::move(process));
        }

        std::vector<AgentProcessLaunchRequest>         launches;
        std::vector<std::shared_ptr<FakeProcessState>> processes;
        std::optional<std::size_t>                     fail_launch_index;
        std::string                                    next_stdout;
        std::string                                    next_stderr;
        int                                            next_wait_failures = 0;
        std::vector<std::string>*                      call_log = nullptr;
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
        request.agent = std::move(agents);
        request.pi_env.client_name = "DAS";
        request.pi_env.client_version = "0.1";
        request.pi_env.project_version = "project";
        request.extra_pi_env.push_back(
            PiEnvVarDto{.key = "PI_TRACE", .value = "1"});
        request.extra_pi_env.push_back(
            PiEnvVarDto{.key = "PATH", .value = "must-not-launch"});
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

    std::size_t CallIndex(
        const std::vector<std::string>& calls,
        std::string_view                expected)
    {
        const auto it = std::find(calls.begin(), calls.end(), expected);
        EXPECT_NE(it, calls.end()) << expected;
        return static_cast<std::size_t>(std::distance(calls.begin(), it));
    }

    TEST(DasMaaPiMaaHandle, MaaHandlesCleanupInReverseOwnershipOrder)
    {
        FakeMaaApiBoundary fake;

        {
            ControllerSpec spec;
            spec.name = "Android";
            spec.type = "Adb";
            ControllerInitParams params;
            params.controller_type = "Adb";
            AdbInitParams adb;
            adb.address = "127.0.0.1:5555";
            params.adb = adb;
            const ScopedResource   resource(fake, fake.CreateResource());
            const ScopedController controller(
                fake,
                fake.CreateControllerFromParams(spec, params).handle);
            const ScopedTasker tasker(fake, fake.CreateTasker());
        }

        const std::vector<std::string> expected{
            "CreateResource",
            "CreateControllerFromParams:Android:Adb",
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
            const ScopedResource   resource(fake, INVALID_MAA_RESOURCE_HANDLE);
            const ScopedController controller(
                fake,
                INVALID_MAA_CONTROLLER_HANDLE);
            const ScopedTasker      tasker(fake, INVALID_MAA_TASKER_HANDLE);
            const ScopedAgentClient client(
                fake,
                INVALID_MAA_AGENT_CLIENT_HANDLE);
        }

        EXPECT_TRUE(fake.calls.empty());
    }

    TEST(DasMaaPiAgentClient, FakeBoundaryDrivesAgentClientOperations)
    {
        FakeMaaApiBoundary fake;

        const auto client =
            fake.CreateAgentClientV2(std::string_view("agent-1"));
        EXPECT_NE(client, INVALID_MAA_AGENT_CLIENT_HANDLE);
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
            const ScopedAgentClient client(fake, fake.CreateAgentClientTcp(0));
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
            std::filesystem::path(DAS_PATH_LITERAL(__FILE__))
                .parent_path()
                .parent_path()
            / DAS_PATH_LITERAL("src") / DAS_PATH_LITERAL("MaaRuntime.cpp");
        const std::ifstream input(source_path);
        ASSERT_TRUE(input.is_open()) << Das::Utils::PathToUtf8(source_path);

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

    // 验证 MaaRuntime.cpp 内 agent 输出本地落盘的 spdlog rotating logger 接入
    // 点：AgentOutputLogger 工厂函数 + rotating_file_sink_mt + 显式
    // register_logger + logger->info 调用 + "[AGENT {" 字面量 + DAS_FMT_NS
    // 字符串拼接整理。AgentOutputLogger 是 anonymous namespace 函数级 static
    // 单例，hermetic 单测访问不到；本地写盘靠 rotating_file_sink 既有保证
    // （LogArchiveSpdlogTest 范式）+ 真实 agent spawn 人工验证。
    TEST(DasMaaPiAgentClient, AgentOutputLoggerWiredInSource)
    {
        const auto source_path =
            std::filesystem::path(DAS_PATH_LITERAL(__FILE__))
                .parent_path()
                .parent_path()
            / DAS_PATH_LITERAL("src") / DAS_PATH_LITERAL("MaaRuntime.cpp");
        const std::ifstream input(source_path);
        ASSERT_TRUE(input.is_open()) << Das::Utils::PathToUtf8(source_path);

        std::ostringstream buffer;
        buffer << input.rdbuf();
        const auto source = buffer.str();

        EXPECT_NE(source.find("AgentOutputLogger"), std::string::npos);
        EXPECT_NE(
            source.find("spdlog::sinks::rotating_file_sink_mt"),
            std::string::npos);
        EXPECT_NE(source.find("spdlog::register_logger"), std::string::npos);
        EXPECT_NE(source.find("logger->info"), std::string::npos);
        EXPECT_NE(source.find("[AGENT {}]"), std::string::npos);
        // fmt 整理：原 "\""+std::string(x)+"\"" 改走 DAS_FMT_NS::format
        // 统一风格
        EXPECT_NE(source.find("DAS_FMT_NS::format"), std::string::npos);
        EXPECT_NE(source.find("MAX_AGENT_LOG_BYTES"), std::string::npos);
        EXPECT_NE(source.find("MAX_AGENT_LOG_FILES"), std::string::npos);
        // boost::dll::this_line_location 必须 #include 在
        // DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY
        // 守卫外（测试构建守卫生效也要可用）
        EXPECT_NE(
            source.find("boost::dll::this_line_location"),
            std::string::npos);
        const auto guard_block =
            source.find("#ifndef DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY");
        ASSERT_NE(guard_block, std::string::npos);
        const auto this_line_include =
            source.find("<boost/dll/runtime_symbol_info.hpp>");
        ASSERT_NE(this_line_include, std::string::npos);
        // this_line_location include 必须在守卫块之后、且不在守卫内（即守卫
        // 的 #endif 之前但有独立 include 行）
        EXPECT_GT(this_line_include, guard_block);
    }

    // 验证 AgentProcessRunner.cpp 内 Windows CREATE_NO_WINDOW 经 v2
    // process_creation_flags 模板实例化的接入点：两构造分支末尾的
    // initializer + 整块 #ifdef _WIN32 守卫的 creation_flags.hpp include。
    // kernel 实际效果（Windows 不弹控制台）走构造论证 + 人工 spawn 验证
    // （RESEARCH §不可 hermetic 测的）。
    TEST(DasMaaPiAgentClient, CreateNoWindowWiredInSource)
    {
        const auto source_path =
            std::filesystem::path(DAS_PATH_LITERAL(__FILE__))
                .parent_path()
                .parent_path()
            / DAS_PATH_LITERAL("src")
            / DAS_PATH_LITERAL("AgentProcessRunner.cpp");
        const std::ifstream input(source_path);
        ASSERT_TRUE(input.is_open()) << Das::Utils::PathToUtf8(source_path);

        std::ostringstream buffer;
        buffer << input.rdbuf();
        const auto source = buffer.str();

        // 整块 #ifdef _WIN32 守卫的 creation_flags.hpp include（POSIX
        // 编译跳过）
        EXPECT_NE(
            source.find("<boost/process/v2/windows/creation_flags.hpp>"),
            std::string::npos);
        // 两构造分支末尾的 initializer（capture_output + 非 capture 各一处）。
        // clang-format 会把 process_creation_flags<CREATE_NO_WINDOW>{} 拆两行，
        // 故分两段断言各自 count == 2（避免对整串字面量跨行敏感）。
        const auto  flag_open = std::string("process_creation_flags<");
        const auto  flag_close = std::string("CREATE_NO_WINDOW>{}");
        std::size_t open_count = 0;
        std::size_t close_count = 0;
        for (std::size_t pos = 0;
             (pos = source.find(flag_open, pos)) != std::string::npos;
             open_count += 1, pos += flag_open.size())
        {
        }
        for (std::size_t pos = 0;
             (pos = source.find(flag_close, pos)) != std::string::npos;
             close_count += 1, pos += flag_close.size())
        {
        }
        EXPECT_EQ(open_count, 2u);
        EXPECT_EQ(close_count, 2u);
    }

    TEST(
        DasMaaPiAgentRuntimeStart,
        StartsSingleAgentWithIdentifierArgCwdAndPiEnv)
    {
        FakeMaaApiBoundary  fake;
        FakeProcessRunner   runner;
        AgentRuntimeService service(fake, runner);

        auto result = service.Start(StartRequest(), TestContext());

        ASSERT_EQ(result.status, "succeeded");
        ASSERT_TRUE(result.session_id.has_value());
        EXPECT_EQ(result.outputs.agent_session_id, result.session_id);
        EXPECT_EQ(result.outputs.running_agent_count, 1);
        ASSERT_EQ(result.agent.size(), 1u);
        EXPECT_EQ(result.agent[0].state, "running");
        EXPECT_EQ(result.agent[0].identifier, "agent-client-id");
        ASSERT_EQ(runner.launches.size(), 1u);
        EXPECT_EQ(
            Das::Utils::PathToGenericUtf8(runner.launches[0].executable),
            "python");
        EXPECT_EQ(
            Das::Utils::PathToGenericUtf8(runner.launches[0].working_directory),
            "C:/maa/project");
        EXPECT_EQ(
            runner.launches[0].arguments,
            (std::vector<std::string>{"./agent/main.py", "agent-client-id"}));
        EXPECT_NE(
            FindEnv(runner.launches[0].environment, "PI_CLIENT_NAME"),
            nullptr);
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
        FakeMaaApiBoundary  fake;
        FakeProcessRunner   runner;
        AgentRuntimeService service(fake, runner);

        auto result = service.Start(StartRequest(), TestContext());

        ASSERT_EQ(result.status, "succeeded");
        ASSERT_EQ(runner.launches.size(), 1u);
        EXPECT_EQ(FindEnv(runner.launches[0].environment, "PATH"), nullptr);
        EXPECT_NE(FindEnv(runner.launches[0].environment, "PI_TRACE"), nullptr);
    }

    TEST(
        DasMaaPiAgentRuntimeStart,
        ResolvesRelativeChildExecFromInterfaceDirectory)
    {
        FakeMaaApiBoundary  fake;
        FakeProcessRunner   runner;
        AgentRuntimeService service(fake, runner);

        auto result = service.Start(
            StartRequest({AgentSpecDto{.child_exec = "bin/agent.exe"}}),
            TestContext());

        ASSERT_EQ(result.status, "succeeded");
        ASSERT_EQ(runner.launches.size(), 1u);
        EXPECT_EQ(
            Das::Utils::PathToGenericUtf8(runner.launches[0].executable),
            "C:/maa/project/bin/agent.exe");
    }

    TEST(DasMaaPiAgentRuntimeStart, RejectsEmptyChildExecBeforeLaunch)
    {
        FakeMaaApiBoundary  fake;
        FakeProcessRunner   runner;
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
        FakeProcessRunner   runner;
        AgentRuntimeService service(fake, runner);

        auto result = service.Start(StartRequest(), TestContext());

        EXPECT_EQ(result.status, "failed");
        ASSERT_EQ(runner.processes.size(), 1u);
        EXPECT_GT(runner.processes[0]->terminate_calls, 0);
        EXPECT_GT(runner.processes[0]->wait_calls, 0);
        EXPECT_FALSE(result.session_id.has_value());
    }

    TEST(
        DasMaaPiAgentRuntimeStart,
        SinkRegistrationFailureTerminatesLaunchedChild)
    {
        FakeMaaApiBoundary fake;
        fake.register_agent_client_tasker_sink_result =
            MaaApiResult::Failure(9, "tasker sink failed");
        FakeProcessRunner   runner;
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
        auto                request = StartRequest();
        request.options.max_output_tail_bytes = 4;

        auto started = service.Start(request, TestContext());
        ASSERT_EQ(started.status, "succeeded");

        auto status = service.Status(*started.session_id);

        ASSERT_EQ(status.status, "succeeded");
        ASSERT_EQ(status.agent.size(), 1u);
        EXPECT_EQ(status.agent[0].stdout_tail, "6789");
        EXPECT_EQ(status.agent[0].stderr_tail, "ghij");
    }

    TEST(DasMaaPiAgentRuntimeStatus, ReportsExitedProcessFromSessionTable)
    {
        FakeMaaApiBoundary  fake;
        FakeProcessRunner   runner;
        AgentRuntimeService service(fake, runner);

        auto started = service.Start(StartRequest(), TestContext());
        ASSERT_EQ(started.status, "succeeded");
        ASSERT_EQ(runner.processes.size(), 1u);
        runner.processes[0]->running = false;
        runner.processes[0]->exit_code = 42;

        auto status = service.Status(*started.session_id);

        ASSERT_EQ(status.status, "succeeded");
        ASSERT_EQ(status.agent.size(), 1u);
        EXPECT_EQ(status.agent[0].state, "exited");
        ASSERT_TRUE(status.agent[0].exit_code.has_value());
        EXPECT_EQ(*status.agent[0].exit_code, 42);
    }

    TEST(DasMaaPiAgentRuntimeStop, TimeoutKillsProcessAndRecordsFinalState)
    {
        FakeMaaApiBoundary fake;
        FakeProcessRunner  runner;
        runner.next_wait_failures = 1;
        AgentRuntimeService service(fake, runner);
        auto                request = StartRequest();
        request.options.stop_timeout_ms = 1;

        auto started = service.Start(request, TestContext());
        ASSERT_EQ(started.status, "succeeded");

        auto stopped = service.Stop(*started.session_id, request.options);

        ASSERT_EQ(stopped.status, "succeeded");
        ASSERT_EQ(runner.processes.size(), 1u);
        EXPECT_EQ(runner.processes[0]->terminate_calls, 1);
        EXPECT_TRUE(stopped.signals.timed_out);
        ASSERT_EQ(stopped.agent.size(), 1u);
        EXPECT_EQ(stopped.agent[0].state, "stopped");
        EXPECT_TRUE(HasCall(fake.calls, "DisconnectAgentClient:1"));
        EXPECT_TRUE(HasCall(fake.calls, "DestroyAgentClient:1"));
    }

    TEST(
        DasMaaPiAgentRuntimeCleanup,
        ServiceDestructionStopsActiveSessionsInCleanupOrder)
    {
        FakeMaaApiBoundary fake;
        FakeProcessRunner  runner;
        runner.next_wait_failures = 1;
        runner.call_log = &fake.calls;

        {
            AgentRuntimeService service(fake, runner);
            auto                request = StartRequest();
            request.options.stop_timeout_ms = 1;

            auto started = service.Start(request, TestContext());

            ASSERT_EQ(started.status, "succeeded");
            ASSERT_TRUE(started.session_id.has_value());
            ASSERT_EQ(runner.processes.size(), 1u);
        }

        ASSERT_EQ(runner.processes.size(), 1u);
        EXPECT_EQ(runner.processes[0]->wait_calls, 2);
        EXPECT_EQ(runner.processes[0]->terminate_calls, 1);

        const auto disconnect =
            CallIndex(fake.calls, "DisconnectAgentClient:1");
        const auto destroy = CallIndex(fake.calls, "DestroyAgentClient:1");
        const auto first_wait = CallIndex(fake.calls, "WaitForExit");
        const auto terminate = CallIndex(fake.calls, "Terminate");
        const auto second_wait = static_cast<std::size_t>(
            std::find(
                fake.calls.begin() + first_wait + 1,
                fake.calls.end(),
                "WaitForExit")
            - fake.calls.begin());

        ASSERT_LT(second_wait, fake.calls.size());
        EXPECT_LT(disconnect, destroy);
        EXPECT_LT(destroy, first_wait);
        EXPECT_LT(first_wait, terminate);
        EXPECT_LT(terminate, second_wait);
    }

    TEST(
        DasMaaPiAgentRuntimeProcess,
        RealRunnerUsesBoostProcessV2AndPerChildEnvironment)
    {
        const auto source_path =
            std::filesystem::path(DAS_PATH_LITERAL(__FILE__))
                .parent_path()
                .parent_path()
            / DAS_PATH_LITERAL("src")
            / DAS_PATH_LITERAL("AgentProcessRunner.cpp");
        const std::ifstream input(source_path);
        ASSERT_TRUE(input.is_open()) << Das::Utils::PathToUtf8(source_path);

        std::ostringstream buffer;
        buffer << input.rdbuf();
        const auto source = buffer.str();

        EXPECT_NE(
            source.find("boost::process::v2::process"),
            std::string::npos);
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

    // BuildProcessEnvironment 是 spawn 子进程的环境装配纯函数：拉继承进程
    // 环境 + 应用 PI_* 前缀过滤 + 注入 PYTHONIOENCODING/PYTHONUTF8。这些单测
    // 调 test-only overload 注入 fixture 继承 env，避免依赖测试进程真继承 env
    // （boost::process::v2::environment::current() 难控）。
    namespace
    {
        bool ContainsEnv(
            const std::vector<std::string>& env,
            std::string_view                expected)
        {
            return std::find(env.begin(), env.end(), expected) != env.end();
        }

        std::vector<std::pair<std::string, std::string>> EmptyInheritedEnv()
        {
            return {};
        }
    } // namespace

    TEST(DasMaaPiBuildEnvPython, EmptyPiEnvInjectsPythonUtf8)
    {
        const auto env = Detail::BuildProcessEnvironment(
            std::vector<PiEnvVarDto>{},
            EmptyInheritedEnv());
        EXPECT_TRUE(ContainsEnv(env, "PYTHONIOENCODING=utf-8"));
        EXPECT_TRUE(ContainsEnv(env, "PYTHONUTF8=1"));
    }

    TEST(DasMaaPiBuildEnvPython, OverridesInheritedPythonUtf8)
    {
        // 继承 env 含 PYTHONUTF8=0（系统抑制 UTF-8 mode），注入必须覆盖。
        std::vector<std::pair<std::string, std::string>> inherited{
            {"PYTHONUTF8", "0"}};
        const auto env = Detail::BuildProcessEnvironment(
            std::vector<PiEnvVarDto>{},
            inherited);
        EXPECT_TRUE(ContainsEnv(env, "PYTHONUTF8=1"));
    }

    TEST(DasMaaPiBuildEnvPython, OverridesInheritedPythonIoencoding)
    {
        // 继承 env 含 PYTHONIOENCODING=latin-1，注入必须覆盖对齐 MXU
        // maa_agent.rs:360-361 的 .env() 覆盖语义（operator[] 非
        // try_emplace）。
        std::vector<std::pair<std::string, std::string>> inherited{
            {"PYTHONIOENCODING", "latin-1"}};
        const auto env = Detail::BuildProcessEnvironment(
            std::vector<PiEnvVarDto>{},
            inherited);
        EXPECT_TRUE(ContainsEnv(env, "PYTHONIOENCODING=utf-8"));
    }

    TEST(DasMaaPiBuildEnvPython, PiFilterKeepsPiAndRejectsNonPi)
    {
        std::vector<PiEnvVarDto> pi_env{
            PiEnvVarDto{.key = "PI_TEST", .value = "x"},
            PiEnvVarDto{.key = "NON_PI", .value = "y"},
        };
        const auto env =
            Detail::BuildProcessEnvironment(pi_env, EmptyInheritedEnv());
        EXPECT_TRUE(ContainsEnv(env, "PI_TEST=x"));
        EXPECT_FALSE(ContainsEnv(env, "NON_PI=y"));
    }

    TEST(DasMaaPiBuildEnvPython, PythonInjectionSurvivesPiPrefixedKeyShadow)
    {
        // PI_PYTHONIOENCODING 是 PI_* 前缀但与 PYTHON* 不同——验证 PYTHON*
        // 注入在 PI_* filter 之后，最终 PYTHONIOENCODING=utf-8 生效。
        std::vector<PiEnvVarDto> pi_env{PiEnvVarDto{
            .key = "PI_PYTHONIOENCODING",
            .value = "should_not_apply"}};
        const auto               env =
            Detail::BuildProcessEnvironment(pi_env, EmptyInheritedEnv());
        EXPECT_TRUE(ContainsEnv(env, "PYTHONIOENCODING=utf-8"));
        EXPECT_TRUE(ContainsEnv(env, "PI_PYTHONIOENCODING=should_not_apply"));
    }
} // namespace
