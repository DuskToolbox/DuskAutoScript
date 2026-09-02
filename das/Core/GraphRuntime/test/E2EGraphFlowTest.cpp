#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>
#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/DoAdapter.h>
#include <das/Core/GraphRuntime/GraphCompiler.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/GraphRuntime/GraphRuntime.h>
#include <das/Core/GraphRuntime/PortFrame.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentHost.Implements.hpp>
#include <gtest/gtest.h>

#include "FlowControlErrorCodes.h"
#include "FlowControlTaskComponents.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace
{
    using namespace Das::Core::GraphRuntime;
    using namespace Das::Core::GraphRuntime::Dto;
    using namespace Das::Core::ForeignInterfaceHost;

    using IDasPortMap = Das::ExportInterface::IDasPortMap;
    using IDasReadOnlyPortMap = Das::ExportInterface::IDasReadOnlyPortMap;
    using IDasStopToken = Das::PluginInterface::IDasStopToken;

    // ---- GUID helpers ----

    const std::string COMP_PASS_THROUGH =
        "A0000000-0000-0000-0000-000000000001";
    const std::string COMP_DOUBLE = "A0000000-0000-0000-0000-000000000002";
    const std::string COMP_SUM = "A0000000-0000-0000-0000-000000000003";
    const std::string COMP_FAIL = "A0000000-0000-0000-0000-000000000004";

    const std::string NODE_A = "10000000-0000-0000-0000-000000000001";
    const std::string NODE_B = "10000000-0000-0000-0000-000000000002";
    const std::string NODE_C = "10000000-0000-0000-0000-000000000003";
    const std::string NODE_D = "10000000-0000-0000-0000-000000000004";

    // ---- Mock StopToken ----

    class MockStopToken final
        : public Das::PluginInterface::DasStopTokenImplBase<MockStopToken>
    {
    public:
        std::atomic<bool> cancelled{false};

        DasResult DAS_STD_CALL StopRequested(bool* canStop) override
        {
            if (!canStop)
            {
                return DAS_E_INVALID_POINTER;
            }
            *canStop = cancelled.load();
            return DAS_S_OK;
        }
    };

    // ---- Stub Factory Manager for GraphCompiler ----

    class StubFactoryManager : public TaskComponentFactoryManager
    {
    public:
        void AddDefinition(
            const std::string&   component_guid_str,
            const yyjson::value& definition)
        {
            const DasGuid guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                component_guid_str);
            definitions_.push_back(
                {guid, guid, guid, Das::Utils::CloneYyjsonValue(definition)});
        }

        std::vector<TaskComponentDefinitionInfo> EnumerateDefinitions()
            const override
        {
            return definitions_;
        }

    private:
        std::vector<TaskComponentDefinitionInfo> definitions_;
    };

    // ---- Document helpers ----

    ComponentRefDto MakeComponentRef(const std::string& component_guid)
    {
        ComponentRefDto ref;
        ref.kind = "componentRef";
        ref.component_guid = component_guid;
        return ref;
    }

    GraphNodeDto MakeComponentNode(
        const std::string& node_id,
        const std::string& component_guid)
    {
        GraphNodeDto node;
        node.node_id = node_id;
        node.target.target_kind = "componentRef";
        node.target.component_ref = MakeComponentRef(component_guid);
        return node;
    }

    GraphEdgeDto MakeEdge(
        const std::string& edge_id,
        const std::string& src_node,
        const std::string& src_port,
        const std::string& tgt_node,
        const std::string& tgt_port)
    {
        GraphEdgeDto edge;
        edge.edge_id = edge_id;
        edge.source_node_id = src_node;
        edge.source_port_id = src_port;
        edge.target_node_id = tgt_node;
        edge.target_port_id = tgt_port;
        return edge;
    }

    GraphPortDefinitionDto MakePortDef(
        const std::string& port_id,
        const std::string& port_type = "int")
    {
        GraphPortDefinitionDto p;
        p.port_id = port_id;
        p.port_type = port_type;
        return p;
    }

    yyjson::value MakeDefinition(
        const std::vector<std::pair<std::string, std::string>>& inputs,
        const std::vector<std::pair<std::string, std::string>>& outputs)
    {
        std::string json = R"({"inputs":[)";
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            if (i > 0)
            {
                json += ",";
            }
            json += R"({"id":")" + inputs[i].first + R"(","type":")"
                    + inputs[i].second + R"("})";
        }
        json += R"(],"outputs":[)";
        for (size_t i = 0; i < outputs.size(); ++i)
        {
            if (i > 0)
            {
                json += ",";
            }
            json += R"({"id":")" + outputs[i].first + R"(","type":")"
                    + outputs[i].second + R"("})";
        }
        json += R"(]})";
        auto result = Das::Utils::ParseYyjsonFromString(json);
        return result ? std::move(*result) : yyjson::value{};
    }

    // ---- Compiled plan helpers ----

    CompiledNodeSnapshotDto MakeSnapshot(
        const std::string&                         node_id,
        const std::string&                         component_guid,
        const std::vector<GraphPortDefinitionDto>& ports = {})
    {
        CompiledNodeSnapshotDto snap;
        snap.node_id = node_id;
        snap.component_guid = component_guid;
        snap.resolved_ports = ports;
        return snap;
    }

    PortBindingDto MakeBinding(
        const std::string& src_node,
        const std::string& src_port,
        const std::string& tgt_node,
        const std::string& tgt_port,
        const std::string& type = "int")
    {
        PortBindingDto b;
        b.source_node_id = src_node;
        b.source_port_id = src_port;
        b.target_node_id = tgt_node;
        b.target_port_id = tgt_port;
        b.expected_type = type;
        return b;
    }

    // Build a linear chain plan: node_0 -> node_1 -> ... -> node_{n-1}
    CompiledGraphPlanDto MakeLinearPlan(
        int                num_nodes,
        const std::string& component_guid = COMP_PASS_THROUGH)
    {
        CompiledGraphPlanDto plan;
        plan.source_fingerprint = "fp_v1";

        for (int i = 0; i < num_nodes; ++i)
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "10000000-0000-0000-0000-%012d", i + 1);
            const std::string nid(buf);

            auto snap = MakeSnapshot(
                nid,
                component_guid,
                {MakePortDef("value_in"), MakePortDef("value_out")});
            plan.node_snapshots.push_back(std::move(snap));
            plan.execution_order.push_back(nid);
        }

        for (int i = 0; i + 1 < num_nodes; ++i)
        {
            plan.binding_plan.bindings.push_back(MakeBinding(
                plan.execution_order[i],
                "value_out",
                plan.execution_order[i + 1],
                "value_in"));
        }

        return plan;
    }

    // ---- PortMap value helpers ----

    void PortMapSetInt(IDasPortMap* map, const std::string& key, int64_t value)
    {
        auto port_id = DasReadOnlyString::FromUtf8(key.c_str(), nullptr);
        map->SetInt(port_id.Get(), value);
    }

    bool PortMapHas(IDasReadOnlyPortMap* map, const std::string& key)
    {
        auto port_id = DasReadOnlyString::FromUtf8(key.c_str(), nullptr);
        bool has = false;
        map->Has(port_id.Get(), &has);
        return has;
    }

    int64_t PortMapGetInt(IDasReadOnlyPortMap* map, const std::string& key)
    {
        auto    port_id = DasReadOnlyString::FromUtf8(key.c_str(), nullptr);
        int64_t value = 0;
        map->GetInt(port_id.Get(), &value);
        return value;
    }

    Das::ExportInterface::DasVariantType PortMapGetType(
        IDasReadOnlyPortMap* map,
        const std::string&   key)
    {
        auto port_id = DasReadOnlyString::FromUtf8(key.c_str(), nullptr);
        Das::ExportInterface::DasVariantType type =
            Das::ExportInterface::DAS_VARIANT_TYPE_NULL;
        map->GetType(port_id.Get(), &type);
        return type;
    }

} // namespace

// =====================================================================
// Test fixture
// =====================================================================
class E2EGraphFlowTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Set up factory manager with standard test component definitions
        stub_mgr_ = std::make_unique<StubFactoryManager>();

        // Pass-through: reads "value_in", writes "value_out" (= value_in)
        stub_mgr_->AddDefinition(
            COMP_PASS_THROUGH,
            MakeDefinition({{"value_in", "int"}}, {{"value_out", "int"}}));

        // Double: reads "value_in", writes "value_out" (= value_in * 2)
        stub_mgr_->AddDefinition(
            COMP_DOUBLE,
            MakeDefinition({{"value_in", "int"}}, {{"value_out", "int"}}));

        // Sum: reads "value_in", writes "value_out" (= sum of value_in)
        stub_mgr_->AddDefinition(
            COMP_SUM,
            MakeDefinition({{"value_in", "int"}}, {{"value_out", "int"}}));

        // Fail: reads "value_in", always returns DAS_E_FAIL from Do()
        stub_mgr_->AddDefinition(
            COMP_FAIL,
            MakeDefinition({{"value_in", "int"}}, {{"value_out", "int"}}));

        token_ =
            Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();
    }

    void TearDown() override {}

    /// Compile a GraphDocumentDto through GraphCompiler with the stub manager
    CompiledGraphPlanDto CompileDocument(const GraphDocumentDto& doc)
    {
        GraphCompiler compiler;
        compiler.SetFactoryManager(stub_mgr_.get());
        auto plan = compiler.Compile(doc);
        return plan;
    }

    std::unique_ptr<StubFactoryManager> stub_mgr_;
    DAS::DasPtr<IDasStopToken>          token_;
};

// ---- 真实 launch 组件直驱（FlowControl 源码静态编入本测试目标） ----

constexpr std::string_view LAUNCH_COMPONENT_GUID_TEXT =
    "95FD2FFA-88F2-4F85-A536-C708D25B641F";
constexpr std::string_view LAUNCH_KIND_TEXT = "das.flow.launch";

class LaunchComponentHost final
    : public Das::PluginInterface::DasTaskComponentHostImplBase<
          LaunchComponentHost>
{
public:
    DasResult CreateTaskComponent(
        const DasGuid&                            component_guid,
        Das::PluginInterface::IDasTaskComponent** pp_out_component) override
    {
        if (pp_out_component == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_component = nullptr;
        if (component_guid
            != Das::Core::ForeignInterfaceHost::MakeDasGuid(
                std::string{LAUNCH_COMPONENT_GUID_TEXT}))
        {
            return DAS_E_NOT_FOUND;
        }

        auto* component = new Das::DasFlowControlTaskComponent(
            LAUNCH_KIND_TEXT,
            DAS::DasPtr<Das::PluginInterface::IDasTaskComponentHost>{});
        component->AddRef();
        *pp_out_component = component;
        return DAS_S_OK;
    }
};

// launch 执行失败（空配置）→ 图以专用错误码终止，错误文本携带节点 id
// 冒泡（GraphRuntime 现有失败接管路径，无 status=failed 继续跑）。
TEST_F(E2EGraphFlowTest, LaunchFailureTerminatesGraphWithErrorCode)
{
    CompiledGraphPlanDto plan;
    plan.source_fingerprint = "fp_launch_failure";

    CompiledNodeSnapshotDto launch_node;
    launch_node.node_id = "n_1";
    launch_node.component_guid = std::string{LAUNCH_COMPONENT_GUID_TEXT};
    plan.node_snapshots.push_back(std::move(launch_node));
    plan.execution_order.push_back("n_1");

    auto         host = LaunchComponentHost::Make();
    GraphRuntime runtime;
    const auto   hr = runtime.RunWithHost(
        plan,
        "fp_launch_failure",
        token_.Get(),
        host.Get());

    EXPECT_EQ(
        hr,
        Das::Plugins::DasFlowControl::DAS_E_FLOW_LAUNCH_EXE_PATH_MISSING);
    EXPECT_NE(runtime.GetLastErrorMessage().find("n_1"), std::string::npos)
        << runtime.GetLastErrorMessage();
}
