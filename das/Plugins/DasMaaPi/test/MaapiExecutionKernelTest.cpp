#include "../src/MaapiExecutionKernel.h"
#include "FakeMaaApiBoundary.h"

#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace
{
    using namespace Das;
    using namespace Das::Plugins::DasMaaPi;
    using namespace Das::Plugins::DasMaaPi::Test;

    class NeverStopToken final
        : public PluginInterface::DasStopTokenImplBase<NeverStopToken>
    {
    public:
        DasResult StopRequested(bool* p_out_stop_requested) override
        {
            if (p_out_stop_requested == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_out_stop_requested = false;
            return DAS_S_OK;
        }
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

    class ScopedBoundaryHook final
    {
    public:
        explicit ScopedBoundaryHook(FakeMaaApiBoundary& boundary)
        {
            SetMaaApiBoundaryForTest(&boundary);
        }

        ~ScopedBoundaryHook() { SetMaaApiBoundaryForTest(nullptr); }
    };

    // envelope fixture 直接构造 ExecutionEnvelopeDto 并序列化——cpp_yyjson
    // 的 default_caster 经 PFR 对每个字段调 obj[key]，缺 key（如
    // focusTranslationsPath/schedule 即便为空也必须显式存在）在 cast 前抛
    // std::out_of_range，手搓 JSON 字符串极易漏键。
    yyjson::value MakeEnvelopeValue(
        std::string pipeline_override_json = R"({"Stage":{"value":"one"}})")
    {
        ExecutionEnvelopeDto envelope;
        envelope.maapi.interface_directory = "C:/maa";
        envelope.maapi.controller_name = "Android";
        envelope.maapi.controller.name = "Android";
        envelope.maapi.controller.type = "Adb";
        envelope.maapi.controller.address = "127.0.0.1:5555";

        AdbInitParams adb;
        adb.address = "127.0.0.1:5555";
        ControllerInitParams controller_init_params;
        controller_init_params.controller_type = "Adb";
        controller_init_params.adb = std::move(adb);
        envelope.maapi.controller_init_params =
            std::move(controller_init_params);

        envelope.maapi.resource_name = "Official";
        envelope.maapi.resource_paths = {"C:/maa/resource"};
        // 与 FakeMaaApiBoundary::GetResourceHash 的默认返回一致，避免
        // resource hash mismatch 警告诊断混入断言面。
        envelope.maapi.resource_hash = "hash-expected";

        MaaTaskExecutionDto task;
        task.task_name = "E2ETestTask";
        task.entry = "StartE2E";
        auto parsed_override =
            Utils::ParseYyjsonFromString(std::move(pipeline_override_json));
        task.pipeline_override = parsed_override ? std::move(*parsed_override)
                                                 : Utils::MakeYyjsonObject();
        envelope.maapi.tasks.push_back(std::move(task));

        return Utils::CloneYyjsonValue(yyjson::object(envelope));
    }
} // namespace

// 合法 envelope 经内核执行：MaaRuntime::Run 被驱动，boundary 收到
// PostTask:StartE2E:<pipeline_override>，返回 runtime 的 das_result。
TEST(DasMaaPiExecutionKernel, ExecutesEnvelopeThroughRuntime)
{
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);
    NeverStopToken           stop;

    MaaRuntimeResult result;
    const auto       hr =
        ExecuteEnvelope(MakeEnvelopeValue(), fake, &stop, {}, {}, result);

    EXPECT_EQ(hr, DAS_S_OK);
    EXPECT_EQ(result.das_result, DAS_S_OK);
    ASSERT_EQ(result.completed_tasks.size(), 1u);
    EXPECT_EQ(result.completed_tasks.front(), "E2ETestTask");
    EXPECT_TRUE(fake.ContainsSubstring("PostTask:StartE2E:"));
    EXPECT_TRUE(fake.Contains("CreateTasker"));
}

// envelope JSON 非法/缺键：返回 ParseExecutionEnvelope 的失败码，不触碰
// boundary。
TEST(DasMaaPiExecutionKernel, ReturnsParseFailureWithoutTouchingBoundary)
{
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);
    NeverStopToken           stop;

    auto invalid = Utils::ParseYyjsonFromString(R"({"version":1})");
    ASSERT_TRUE(invalid.has_value());

    MaaRuntimeResult result;
    const auto hr = ExecuteEnvelope(*invalid, fake, &stop, {}, {}, result);

    EXPECT_EQ(hr, DAS_E_INVALID_ARGUMENT);
    EXPECT_TRUE(fake.calls.empty());
}

// port_map 与 inputs 均非空时 MergePortMapInputs 语义生效：graph_port_id
// 映射进 pipeline_override 的 pi_param_name 键。
TEST(DasMaaPiExecutionKernel, MergesPortMapInputsIntoPipelineOverride)
{
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);
    NeverStopToken           stop;

    const std::map<std::string, std::string> port_map{{"graph_stage", "Stage"}};
    const std::map<std::string, yyjson::value> inputs{
        {"graph_stage", yyjson::value(std::string{"1-1"})}};

    MaaRuntimeResult result;
    const auto       hr = ExecuteEnvelope(
        MakeEnvelopeValue(),
        fake,
        &stop,
        port_map,
        inputs,
        result);

    EXPECT_EQ(hr, DAS_S_OK);
    EXPECT_TRUE(fake.ContainsSubstring(R"(PostTask:StartE2E:{"Stage":"1-1"})"));
}

// port_map 与 inputs 任一为空表：空操作语义，envelope 原样执行。
TEST(DasMaaPiExecutionKernel, EmptyMapsLeaveEnvelopeUntouched)
{
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);
    NeverStopToken           stop;

    const std::map<std::string, yyjson::value> inputs{
        {"graph_stage", yyjson::value(std::string{"1-1"})}};

    MaaRuntimeResult result;
    const auto       hr =
        ExecuteEnvelope(MakeEnvelopeValue(), fake, &stop, {}, inputs, result);

    EXPECT_EQ(hr, DAS_S_OK);
    EXPECT_TRUE(fake.ContainsSubstring(
        R"(PostTask:StartE2E:{"Stage":{"value":"one"}})"));
}

// stop_token 已停：提前返回 DAS_E_TIMEOUT，不执行。
TEST(DasMaaPiExecutionKernel, ReturnsTimeoutWhenStopAlreadyRequested)
{
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);
    RequestedStopToken       stop;

    MaaRuntimeResult result;
    const auto       hr =
        ExecuteEnvelope(MakeEnvelopeValue(), fake, &stop, {}, {}, result);

    EXPECT_EQ(hr, DAS_E_TIMEOUT);
    EXPECT_TRUE(fake.calls.empty());
}
