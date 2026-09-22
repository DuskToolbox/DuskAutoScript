#include "../src/MaapiRunTaskComponent.h"
#include "FakeMaaApiBoundary.h"

#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphRuntime.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentCreator.Implements.hpp>
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>

namespace
{
    using namespace Das;
    using namespace Das::Core::GraphRuntime;
    using namespace Das::Core::GraphRuntime::Dto;
    using namespace Das::Plugins::DasMaaPi;
    using namespace Das::Plugins::DasMaaPi::Test;

    using IDasTaskComponent = PluginInterface::IDasTaskComponent;

    // ---- Constants ----

    const std::string MAAPI_RUN_TASK_GUID =
        "69F20008-0000-4000-8000-000000000001";
    const std::string NODE_ID = "30000000-0000-0000-0000-000000000001";

    // 图输入广播绑定哨兵（与 GraphRuntime/DoAdapter 的 BROADCAST_SOURCE
    // 一致）：source 不是真实节点，default_value 直接物化到目标输入口。
    constexpr std::string_view GRAPH_INPUT_BROADCAST_SOURCE = "$graph_input";

    // ---- Mock StopToken ----

    class MockStopToken final
        : public PluginInterface::DasStopTokenImplBase<MockStopToken>
    {
    public:
        DasResult DAS_STD_CALL StopRequested(bool* canStop) override
        {
            if (!canStop)
            {
                return DAS_E_INVALID_POINTER;
            }
            *canStop = false;
            return DAS_S_OK;
        }
    };

    // ---- ScopedBoundaryHook ----

    class ScopedBoundaryHook final
    {
    public:
        explicit ScopedBoundaryHook(FakeMaaApiBoundary& boundary)
        {
            SetMaaApiBoundaryForTest(&boundary);
        }

        ~ScopedBoundaryHook() { SetMaaApiBoundaryForTest(nullptr); }
    };

    // ---- MaapiMockHost ----
    // maapi run 组件为被测对象本体（图内直接消费 executionInput 端口喂线
    // 的 envelope——引用闭包注入链的产物物化形态，无中间人组件）。

    class MaapiMockHost final
        : public PluginInterface::DasTaskComponentCreatorImplBase<MaapiMockHost>
    {
    public:
        DasResult CreateTaskComponent(
            const DasGuid&      component_guid,
            IDasTaskComponent** pp_out_component) override
        {
            if (!pp_out_component)
            {
                return DAS_E_INVALID_POINTER;
            }

            auto guid_str = DasGuidToString(component_guid);

            if (guid_str.GetUtf8() == MAAPI_RUN_TASK_GUID)
            {
                auto* component = new MaapiRunTaskComponent();
                component->AddRef();
                *pp_out_component = component;
                return DAS_S_OK;
            }

            *pp_out_component = nullptr;
            return DAS_E_NOT_FOUND;
        }

        DasResult CreateAuthoringSession(
            const DasGuid&                              task_guid,
            Das::ExportInterface::IDasJson*             p_context_json,
            PluginInterface::IDasTaskAuthoringSession** pp_out_session) override
        {
            if (!pp_out_session)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_session = nullptr;
            return DAS_E_NO_IMPLEMENTATION;
        }
    };

    // ---- Plan builders ----

    GraphPortDefinitionDto MakePortDef(
        const std::string& port_id,
        const std::string& port_type = "string")
    {
        GraphPortDefinitionDto p;
        p.port_id = port_id;
        p.port_type = port_type;
        return p;
    }

    // envelope fixture 直接构造 ExecutionEnvelopeDto 并序列化——cpp_yyjson
    // 的 default_caster 经 PFR 对每个字段调 obj[key]，缺 key（如
    // focusTranslationsPath/schedule 即便为空也必须显式存在）在 cast 前抛
    // std::out_of_range，手搓 JSON 字符串极易漏键。
    yyjson::value MakeEnvelopeValue()
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
        task.pipeline_override = Utils::MakeYyjsonObject();
        envelope.maapi.tasks.push_back(std::move(task));

        return Utils::CloneYyjsonValue(yyjson::object(envelope));
    }

    // 编译产物串：被引 maapi 条目的 compile 段 executionInput 形态=
    // envelope 原文（maapi.run 的产物通道——无 {fingerprint, plan} 外壳）。
    std::string MakeEnvelopeArtifactText()
    {
        const auto serialized = Utils::SerializeYyjsonValue(
            Utils::CloneYyjsonValue(MakeEnvelopeValue()));
        return serialized.value_or("{}");
    }

    CompiledGraphPlanDto MakeSingleMaapiRunNodePlan(
        const std::string& artifact_text)
    {
        CompiledGraphPlanDto plan;
        plan.source_fingerprint = "fp_e2e";

        CompiledNodeSnapshotDto snap;
        snap.node_id = NODE_ID;
        snap.component_guid = MAAPI_RUN_TASK_GUID;
        snap.resolved_ports = {
            MakePortDef("executionInput", "object"),
            MakePortDef("status", "string"),
            MakePortDef("completedTasks", "array<string>"),
            MakePortDef("stopped", "bool"),
            MakePortDef("diagnostics", "array<object>"),
        };
        plan.node_snapshots.push_back(std::move(snap));
        plan.execution_order.push_back(NODE_ID);

        // 图输入广播绑定：编译产物串以字符串标量物化到 executionInput
        // 输入口（GraphRuntime 只对标量做广播注入，对象/数组会被跳过）。
        // yyjson::value 的 lvalue std::string 路径存的是视图（悬垂），
        // 必须走 rvalue 拷贝路径让其持有字节。
        PortBindingDto broadcast;
        broadcast.source_node_id = std::string{GRAPH_INPUT_BROADCAST_SOURCE};
        broadcast.source_port_id = "artifact";
        broadcast.target_node_id = NODE_ID;
        broadcast.target_port_id = "executionInput";
        broadcast.default_value = yyjson::value(std::string{artifact_text});
        plan.binding_plan.bindings.push_back(std::move(broadcast));

        return plan;
    }

} // namespace

// ===================================================================
// Test 1: MaapiRunNodeExecutesInjectedEnvelopeInGraphRuntime
//
// tracer 端到端链路验收（引用闭包注入链形态）：
//   GraphRuntime::RunWithHost → maapi run 组件节点（无中间人组件）→
//   $graph_input 广播物化 executionInput（被引条目编译产物=envelope
//   原文）→ MaapiRunTaskComponent::Do → 共享执行内核 → MaaRuntime::Run
//   （FakeMaaApiBoundary 观测）
// ===================================================================

TEST(MaapiGraphE2ETest, MaapiRunNodeExecutesInjectedEnvelopeInGraphRuntime)
{
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    auto plan = MakeSingleMaapiRunNodePlan(MakeEnvelopeArtifactText());

    auto host = MaapiMockHost::Make();
    auto token = PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    GraphRuntime rt;
    auto         hr = rt.RunWithHost(plan, "fp_e2e", token.Get(), host.Get());

    EXPECT_EQ(hr, DAS_S_OK);

    // 组件被真实驱动：maapi run 组件经 executionInput 消费注入的 envelope
    // 并到达 MaaRuntime 的任务投递。
    EXPECT_TRUE(fake.ContainsSubstring("PostTask:StartE2E:"));
    EXPECT_TRUE(fake.Contains("CreateTasker"));
}

// ===================================================================
// Test 2: MaapiRunNodePropagatesChildFailureToGraph
//
// MaaFW 任务失败时组件 Do 返回失败码——图执行走失败接管路径（运行期返
// 回非成功码），boundary 可观测到已尝试投递。调用中介退役后无「子失败
// 收进 status 端口、自身返回成功」的中间层：节点即组件，失败直达。
// ===================================================================

TEST(MaapiGraphE2ETest, MaapiRunNodePropagatesChildFailureToGraph)
{
    FakeMaaApiBoundary fake;
    fake.wait_status_by_entry["StartE2E"] = MaaTaskStatus::Failed;
    const ScopedBoundaryHook hook(fake);

    auto plan = MakeSingleMaapiRunNodePlan(MakeEnvelopeArtifactText());

    auto host = MaapiMockHost::Make();
    auto token = PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    GraphRuntime rt;
    auto         hr = rt.RunWithHost(plan, "fp_e2e", token.Get(), host.Get());

    EXPECT_NE(hr, DAS_S_OK);
    EXPECT_TRUE(fake.ContainsSubstring("PostTask:StartE2E:"));
}
