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
        // v26 fixture 的控制器是 Adb：PI v2 协议下 address 只能来自任务设置
        // 携带的设备选择（adbDevice），这里 pin FakeMaaApiBoundary 默认设备
        // 的 address，让执行引擎经 boundary.FindAdbDevices 匹配六字段。
        input.adb_device = "127.0.0.1:5555";
        return input;
    }

} // namespace

TEST(MaaPiExecutionEngine, EngineCompilesAndExecutesSuccessfully)
{
    FakeMaaApiBoundary       fake;
    FakeProcessRunner        runner;
    const ScopedRuntimeHooks hooks(fake, runner);

    MaaPiExecutionEngine engine;
    auto output = engine.Execute(MakeDefaultInput(), fake, nullptr);

    EXPECT_EQ(output.das_result, DAS_S_OK);
    EXPECT_FALSE(output.completed_tasks.empty());
    EXPECT_EQ(output.completed_tasks.front(), "DailyFarm");
}

TEST(MaaPiExecutionEngine, EngineReturnsErrorOnMissingPiPath)
{
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

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
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

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
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

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
    FakeProcessRunner        runner;
    const ScopedRuntimeHooks hooks(fake, runner);

    MaaPiExecutionEngine engine;
    auto output = engine.Execute(MakeDefaultInput(), fake, nullptr);

    EXPECT_EQ(output.das_result, DAS_E_MAAPI_EXECUTION_FAILED);
}

TEST(MaaPiExecutionEngine, EngineRespectsStopToken)
{
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    RequestedStopToken stop;

    MaaPiExecutionEngine engine;
    auto output = engine.Execute(MakeDefaultInput(), fake, &stop);

    EXPECT_NE(output.das_result, DAS_S_OK);
}

// Adb 控制器 + 任务设置未携带 adbDevice：执行输入不完整，编译前显性失败
// （PI v2 协议的 controller.adb 不含 address，静默放行只会延后到
// MaaAdbControllerCreate 处报模糊错误）。
TEST(
    MaaPiExecutionEngine,
    AdbControllerWithoutDeviceSelectionFailsBeforeCompile)
{
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    auto input = MakeDefaultInput();
    input.adb_device.clear();

    MaaPiExecutionEngine engine;
    auto                 output = engine.Execute(input, fake, nullptr);

    EXPECT_EQ(output.das_result, DAS_E_MAAPI_ADB_DEVICE_MISSING);
    ASSERT_EQ(output.diagnostics.size(), 1u);
    EXPECT_EQ(output.diagnostics.front().code, "adb-device-missing");
    EXPECT_EQ(output.diagnostics.front().severity, "error");
    // 失败发生在编译前：boundary 不应收到任何控制器/任务调用
    EXPECT_FALSE(fake.Contains("CreateControllerFromParams"));
    EXPECT_FALSE(fake.Contains("CreateTasker"));
}

// 任务 pin 的设备 address 不在执行期扫描结果中：显性失败并指引用户检查
// 连接或重选，不静默 fallback。
TEST(MaaPiExecutionEngine, AdbControllerPinnedDeviceNotDiscoveredFails)
{
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    auto input = MakeDefaultInput();
    input.adb_device = "192.168.1.50:5555";

    MaaPiExecutionEngine engine;
    auto                 output = engine.Execute(input, fake, nullptr);

    EXPECT_EQ(output.das_result, DAS_E_MAAPI_ADB_DEVICE_MISSING);
    ASSERT_EQ(output.diagnostics.size(), 1u);
    EXPECT_EQ(output.diagnostics.front().code, "adb-device-not-found");
    EXPECT_NE(
        output.error_message.find("192.168.1.50:5555"),
        std::string::npos);
    EXPECT_TRUE(fake.Contains("FindAdbDevices"));
    EXPECT_FALSE(fake.Contains("CreateControllerFromParams"));
}

// 设备匹配成功：boundary 扫描的 Toolkit 六字段注入 envelope，控制器以该
// address 创建（NormalizeControllerInitParams 的 cache 覆盖路径）。
TEST(MaaPiExecutionEngine, AdbControllerResolvedDeviceInjectsToolkitFields)
{
    FakeMaaApiBoundary       fake;
    FakeProcessRunner        runner;
    const ScopedRuntimeHooks hooks(fake, runner);

    MaaPiExecutionEngine engine;
    auto output = engine.Execute(MakeDefaultInput(), fake, nullptr);

    EXPECT_EQ(output.das_result, DAS_S_OK);
    ASSERT_TRUE(fake.last_controller_init_params.has_value());
    ASSERT_TRUE(fake.last_controller_init_params->adb.has_value());
    EXPECT_EQ(fake.last_controller_init_params->adb->address, "127.0.0.1:5555");
    EXPECT_EQ(
        fake.last_controller_init_params->adb->adb_path,
        "C:/adb/adb.exe");
    EXPECT_EQ(fake.last_controller_init_params->adb->screencap_method, 0x0FULL);
    EXPECT_EQ(fake.last_controller_init_params->adb->input_method, 0x03ULL);
}
