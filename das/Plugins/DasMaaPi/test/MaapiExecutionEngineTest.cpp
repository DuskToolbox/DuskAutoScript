#include "../src/AgentProcessRunner.h"
#include "FakeMaaApiBoundary.h"

#include <das/Plugins/DasMaaPi/MaaPiExecutionEngine.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Plugins/DasMaaPi/PiParser.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace
{
    using namespace Das;
    using namespace Das::Plugins::DasMaaPi;
    using namespace Das::Plugins::DasMaaPi::AgentRuntime;
    using namespace Das::Plugins::DasMaaPi::Test;

    std::filesystem::path FixturePath(std::string_view name)
    {
        return std::filesystem::current_path() / "DasMaaPi" / "test"
               / "fixtures" / std::filesystem::path{name};
    }

    struct FakeProcessState
    {
        bool                   running = true;
        int                    wait_calls = 0;
        std::optional<int32_t> exit_code;
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
                .pid = 6001,
                .exit_code = state_->exit_code,
                .stdout_tail = {},
                .stderr_tail = {}};
        }

        bool WaitForExit(std::chrono::milliseconds) override
        {
            ++state_->wait_calls;
            state_->running = false;
            state_->exit_code = 0;
            return true;
        }

        void Terminate() override { state_->running = false; }

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
            auto state = std::make_shared<FakeProcessState>();
            processes.push_back(state);
            return AgentProcessLaunchResult::Success(
                std::make_unique<FakeProcess>(std::move(state)));
        }

        std::vector<AgentProcessLaunchRequest>         launches;
        std::vector<std::shared_ptr<FakeProcessState>> processes;
    };

    class ScopedRuntimeHooks final
    {
    public:
        ScopedRuntimeHooks(FakeMaaApiBoundary& fake, FakeProcessRunner& runner)
        {
            SetMaaApiBoundaryForTest(&fake);
            SetAgentProcessRunnerForTest(&runner);
        }

        ~ScopedRuntimeHooks()
        {
            SetAgentProcessRunnerForTest(nullptr);
            SetMaaApiBoundaryForTest(nullptr);
        }
    };

    class ScopedBoundaryHook final
    {
    public:
        explicit ScopedBoundaryHook(FakeMaaApiBoundary& boundary)
        {
            SetMaaApiBoundaryForTest(&boundary);
        }

        ~ScopedBoundaryHook() { SetMaaApiBoundaryForTest(nullptr); }
    };

    class RequestedStopToken final
        : public PluginInterface::DasStopTokenImplBase<RequestedStopToken>
    {
    public:
        DasResult StopRequested(bool* p_out_stop_requested) override
        {
            if (p_out_stop_requested == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_out_stop_requested = true;
            return DAS_S_OK;
        }
    };

    EngineInput MakeDefaultInput()
    {
        EngineInput input;
        input.pi_path = FixturePath("interface_v26_jsonc.jsonc").string();
        input.task_name = "DailyFarm";
        input.options = Das::Utils::MakeYyjsonObject();
        return input;
    }

} // namespace

TEST(MaaPiExecutionEngine, EngineCompilesAndExecutesSuccessfully)
{
    FakeMaaApiBoundary fake;
    FakeProcessRunner  runner;
    ScopedRuntimeHooks hooks(fake, runner);

    MaaPiExecutionEngine engine;
    auto output = engine.Execute(MakeDefaultInput(), fake, nullptr);

    EXPECT_EQ(output.das_result, DAS_S_OK);
    EXPECT_FALSE(output.completed_tasks.empty());
    EXPECT_EQ(output.completed_tasks.front(), "DailyFarm");
}

TEST(MaaPiExecutionEngine, EngineReturnsErrorOnMissingPiPath)
{
    FakeMaaApiBoundary fake;
    ScopedBoundaryHook hook(fake);

    EngineInput input;
    input.pi_path = "/nonexistent/path/interface.json";
    input.task_name = "DailyFarm";
    input.options = Das::Utils::MakeYyjsonObject();

    MaaPiExecutionEngine engine;
    auto                 output = engine.Execute(input, fake, nullptr);

    EXPECT_TRUE(
        output.das_result == DAS_E_MAAPI_PI_PARSE_FAILED
        || output.das_result == DAS_E_MAAPI_PI_MISSING);
}

TEST(MaaPiExecutionEngine, EngineReturnsErrorOnEmptyPiPath)
{
    FakeMaaApiBoundary fake;
    ScopedBoundaryHook hook(fake);

    EngineInput input;
    input.pi_path = "";
    input.task_name = "DailyFarm";
    input.options = Das::Utils::MakeYyjsonObject();

    MaaPiExecutionEngine engine;
    auto                 output = engine.Execute(input, fake, nullptr);

    EXPECT_EQ(output.das_result, DAS_E_MAAPI_PI_MISSING);
}

TEST(MaaPiExecutionEngine, EngineReturnsErrorOnTaskNotFound)
{
    FakeMaaApiBoundary fake;
    ScopedBoundaryHook hook(fake);

    auto input = MakeDefaultInput();
    input.task_name = "NonExistentTask";

    MaaPiExecutionEngine engine;
    auto                 output = engine.Execute(input, fake, nullptr);

    EXPECT_EQ(output.das_result, DAS_E_MAAPI_TASK_MISSING);
    EXPECT_NE(output.error_message.find("NonExistentTask"), std::string::npos);
}

TEST(MaaPiExecutionEngine, EngineMapsExecutionFailureToErrorCode)
{
    FakeMaaApiBoundary fake;
    fake.wait_status_by_entry["StartDaily"] = MaaTaskStatus::Failed;
    FakeProcessRunner  runner;
    ScopedRuntimeHooks hooks(fake, runner);

    MaaPiExecutionEngine engine;
    auto output = engine.Execute(MakeDefaultInput(), fake, nullptr);

    EXPECT_EQ(output.das_result, DAS_E_MAAPI_EXECUTION_FAILED);
}

TEST(MaaPiExecutionEngine, EngineRespectsStopToken)
{
    FakeMaaApiBoundary fake;
    ScopedBoundaryHook hook(fake);

    RequestedStopToken stop;

    MaaPiExecutionEngine engine;
    auto output = engine.Execute(MakeDefaultInput(), fake, &stop);

    EXPECT_NE(output.das_result, DAS_S_OK);
}
