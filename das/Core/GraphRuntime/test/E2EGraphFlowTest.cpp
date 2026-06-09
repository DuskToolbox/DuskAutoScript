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
#include <gtest/gtest.h>

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

    const std::string kCompPassThrough = "A0000000-0000-0000-0000-000000000001";
    const std::string kCompDouble = "A0000000-0000-0000-0000-000000000002";
    const std::string kCompSum = "A0000000-0000-0000-0000-000000000003";
    const std::string kCompFail = "A0000000-0000-0000-0000-000000000004";

    const std::string kNodeA = "10000000-0000-0000-0000-000000000001";
    const std::string kNodeB = "10000000-0000-0000-0000-000000000002";
    const std::string kNodeC = "10000000-0000-0000-0000-000000000003";
    const std::string kNodeD = "10000000-0000-0000-0000-000000000004";

    // ---- Mock StopToken ----

    class MockStopToken final
        : public Das::PluginInterface::DasStopTokenImplBase<MockStopToken>
    {
    public:
        std::atomic<bool> cancelled{false};

        DasResult DAS_STD_CALL StopRequested(bool* canStop) override
        {
            if (!canStop)
                return DAS_E_INVALID_POINTER;
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
            DasGuid guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
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
                json += ",";
            json += R"({"id":")" + inputs[i].first + R"(","type":")"
                    + inputs[i].second + R"("})";
        }
        json += R"(],"outputs":[)";
        for (size_t i = 0; i < outputs.size(); ++i)
        {
            if (i > 0)
                json += ",";
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
        const std::string& component_guid = kCompPassThrough)
    {
        CompiledGraphPlanDto plan;
        plan.source_fingerprint = "fp_v1";
        plan.compiled_fingerprint = "compiled_fp_v1";

        for (int i = 0; i < num_nodes; ++i)
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "10000000-0000-0000-0000-%012d", i + 1);
            std::string nid(buf);

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
            kCompPassThrough,
            MakeDefinition({{"value_in", "int"}}, {{"value_out", "int"}}));

        // Double: reads "value_in", writes "value_out" (= value_in * 2)
        stub_mgr_->AddDefinition(
            kCompDouble,
            MakeDefinition({{"value_in", "int"}}, {{"value_out", "int"}}));

        // Sum: reads "value_in", writes "value_out" (= sum of value_in)
        stub_mgr_->AddDefinition(
            kCompSum,
            MakeDefinition({{"value_in", "int"}}, {{"value_out", "int"}}));

        // Fail: reads "value_in", always returns DAS_E_FAIL from Do()
        stub_mgr_->AddDefinition(
            kCompFail,
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
