#include "../../DasFlowControl/src/FlowControlTaskComponents.h"
#include "../src/MaapiRunTaskComponent.h"
#include "FakeMaaApiBoundary.h"

#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphRuntime.h>
#include <das/Core/TaskScheduler/RepositoryInvokeDtos.h>
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

    namespace RepositoryInvokeDto =
        Das::Core::TaskScheduler::RepositoryInvoke::Dto;

    using IDasTaskComponent = PluginInterface::IDasTaskComponent;

    // ---- Constants ----

    const std::string MAAPI_RUN_TASK_GUID =
        "69F20008-0000-4000-8000-000000000001";
    const std::string REPOSITORY_INVOKE_GUID =
        "68F10007-0000-4000-8000-000000000007";
    const std::string REPOSITORY_INVOKE_KIND = "das.flow.invokeRepository";
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

    // ---- MockTaskComponentCreator ----
    // RepositoryInvoke 节点用真实 FlowControl 组件实现（host 注入本 mock，
    // 供其创建子组件）；maapi run 组件为被测对象本体。

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

            if (guid_str.GetUtf8() == REPOSITORY_INVOKE_GUID)
            {
                auto* component = new DasFlowControlTaskComponent(
                    REPOSITORY_INVOKE_KIND,
                    DasPtr<PluginInterface::IDasTaskComponentCreator>(this));
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

    // 编译快照：RepositoryInvoke 组件的 compiledSnapshot 输入口契约载荷
    // （唯一编译器的 executionInput 产物内嵌其中）。
    std::string MakeSnapshotJson()
    {
        RepositoryInvokeDto::ChildExecutionSnapshotDto snapshot;
        snapshot.version = 1;
        snapshot.source_entry_id = 42;
        snapshot.source_revision = 7;
        snapshot.plugin_guid = std::string(PLUGIN_GUID_TEXT);
        snapshot.task_type_guid = std::string(TASK_GUID_TEXT);
        snapshot.component_guid = MAAPI_RUN_TASK_GUID;
        snapshot.execution_input = MakeEnvelopeValue();

        const auto serialized = Utils::SerializeYyjsonValue(
            Utils::CloneYyjsonValue(yyjson::object(snapshot)));
        return serialized.value_or("{}");
    }

    CompiledGraphPlanDto MakeSingleRepositoryInvokeNodePlan(
        const std::string& snapshot_json)
    {
        CompiledGraphPlanDto plan;
        plan.source_fingerprint = "fp_e2e";

        CompiledNodeSnapshotDto snap;
        snap.node_id = NODE_ID;
        snap.component_guid = REPOSITORY_INVOKE_GUID;
        snap.resolved_ports = {
            MakePortDef("compiledSnapshot", "object"),
            MakePortDef("status", "string"),
            MakePortDef("diagnostic", "string"),
            MakePortDef("completedTasks", "array<string>"),
            MakePortDef("stopped", "bool"),
            MakePortDef("diagnostics", "array<object>"),
        };
        plan.node_snapshots.push_back(std::move(snap));
        plan.execution_order.push_back(NODE_ID);

        // 图输入广播绑定：快照 JSON 以字符串标量物化到 compiledSnapshot
        // 输入口（GraphRuntime 只对标量做广播注入，对象/数组会被跳过）。
        // yyjson::value 的 lvalue std::string 路径存的是视图（悬垂），
        // 必须走 rvalue 拷贝路径让其持有字节。
        PortBindingDto broadcast;
        broadcast.source_node_id = std::string{GRAPH_INPUT_BROADCAST_SOURCE};
        broadcast.source_port_id = "snapshot";
        broadcast.target_node_id = NODE_ID;
        broadcast.target_port_id = "compiledSnapshot";
        broadcast.default_value = yyjson::value(std::string{snapshot_json});
        plan.binding_plan.bindings.push_back(std::move(broadcast));

        return plan;
    }

} // namespace

// ===================================================================
// Test 1: RepositoryInvokeNodeExecutesMaapiChildInGraphRuntime
//
// tracer 端到端链路验收：
//   GraphRuntime::RunWithHost → 真实 RepositoryInvoke 组件（读
//   compiledSnapshot）→ host CreateTaskComponent(maapi run) →
//   SetPortString("executionInput", ...) → MaapiRunTaskComponent::Do
//   → 共享执行内核 → MaaRuntime::Run（FakeMaaApiBoundary 观测）
// ===================================================================

TEST(MaapiGraphE2ETest, RepositoryInvokeNodeExecutesMaapiChildInGraphRuntime)
{
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    auto plan = MakeSingleRepositoryInvokeNodePlan(MakeSnapshotJson());

    auto host = MaapiMockHost::Make();
    auto token = PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    GraphRuntime rt;
    auto         hr = rt.RunWithHost(plan, "fp_e2e", token.Get(), host.Get());

    EXPECT_EQ(hr, DAS_S_OK);

    // 子组件被真实驱动：maapi run 组件经 executionInput 消费快照内的
    // envelope 并到达 MaaRuntime 的任务投递。
    EXPECT_TRUE(fake.ContainsSubstring("PostTask:StartE2E:"));
    EXPECT_TRUE(fake.Contains("CreateTasker"));
}

// ===================================================================
// Test 2: RepositoryInvokeNodeToleratesMaapiChildFailure
//
// MaaFW 任务失败时子组件 Do 返回失败码，RepositoryInvoke 组件把子失败
// 收进 status=failed 结果口（自身 Do 成功）——图执行完成，boundary 可
// 观测到已尝试投递。
// ===================================================================

TEST(MaapiGraphE2ETest, RepositoryInvokeNodeToleratesMaapiChildFailure)
{
    FakeMaaApiBoundary fake;
    fake.wait_status_by_entry["StartE2E"] = MaaTaskStatus::Failed;
    const ScopedBoundaryHook hook(fake);

    auto plan = MakeSingleRepositoryInvokeNodePlan(MakeSnapshotJson());

    auto host = MaapiMockHost::Make();
    auto token = PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    GraphRuntime rt;
    auto         hr = rt.RunWithHost(plan, "fp_e2e", token.Get(), host.Get());

    EXPECT_EQ(hr, DAS_S_OK);
    EXPECT_TRUE(fake.ContainsSubstring("PostTask:StartE2E:"));
}
