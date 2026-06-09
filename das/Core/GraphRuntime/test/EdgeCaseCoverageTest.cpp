#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>
#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphAuthoring.h>
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

    // ---- Component GUIDs ----

    const std::string kCompPassThrough = "B0000000-0000-0000-0000-000000000001";
    const std::string kCompIntOut = "B0000000-0000-0000-0000-000000000002";
    const std::string kCompStringOut = "B0000000-0000-0000-0000-000000000003";
    const std::string kCompFail = "B0000000-0000-0000-0000-000000000004";

    // ---- Node IDs ----

    const std::string kNodeA = "20000000-0000-0000-0000-000000000001";
    const std::string kNodeB = "20000000-0000-0000-0000-000000000002";
    const std::string kNodeC = "20000000-0000-0000-0000-000000000003";
    const std::string kNodeD = "20000000-0000-0000-0000-000000000004";
    const std::string kNodeE = "20000000-0000-0000-0000-000000000005";

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

    // ---- Stub Factory Manager ----

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

    // ---- Document / node / edge helpers ----

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

    GraphDocumentDto MakeEmptyDocument()
    {
        GraphDocumentDto doc;
        doc.document_id = "edge-case-test";
        doc.version = 1;
        doc.fingerprint = "fp_edge_v1";
        return doc;
    }

    GraphDocumentDto MakeSingleNodeDocument(
        const std::string& node_id,
        const std::string& component_guid)
    {
        auto doc = MakeEmptyDocument();
        doc.nodes.push_back(MakeComponentNode(node_id, component_guid));
        return doc;
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

    CompiledGraphPlanDto MakeSingleSnapshotPlan(
        const std::string&                         node_id,
        const std::string&                         component_guid,
        const std::vector<GraphPortDefinitionDto>& ports = {})
    {
        CompiledGraphPlanDto plan;
        plan.source_fingerprint = "fp_v1";
        plan.compiled_fingerprint = "compiled_fp_v1";
        plan.node_snapshots.push_back(
            MakeSnapshot(node_id, component_guid, ports));
        plan.execution_order = {node_id};
        return plan;
    }

    // ---- PortMap helpers ----

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

    // ---- Authoring helpers ----

    GraphNodeDto MakeAuthoringNode(const std::string& node_id)
    {
        GraphNodeDto node;
        node.node_id = node_id;
        node.target.target_kind = "componentRef";
        node.target.component_ref =
            ComponentRefDto{"componentRef", kCompPassThrough, ""};
        return node;
    }

    yyjson::value MakeSettingsValue(const std::string& key, double value)
    {
        auto payload = Das::Utils::MakeYyjsonObject();
        auto obj = *payload.as_object();
        obj[std::string_view(key)] = value;
        return payload;
    }

} // namespace

// ===========================================================================
// Test fixture: EdgeCaseCoverageTest
// ===========================================================================

class EdgeCaseCoverageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        stub_mgr_ = std::make_unique<StubFactoryManager>();

        // Pass-through: in="value_in", out="value_out"
        stub_mgr_->AddDefinition(
            kCompPassThrough,
            MakeDefinition({{"value_in", "int"}}, {{"value_out", "int"}}));

        // Int-only output
        stub_mgr_->AddDefinition(
            kCompIntOut,
            MakeDefinition({{"value_in", "int"}}, {{"value_out", "int"}}));

        // String output
        stub_mgr_->AddDefinition(
            kCompStringOut,
            MakeDefinition(
                {{"value_in", "string"}},
                {{"value_out", "string"}}));

        // Fail component
        stub_mgr_->AddDefinition(
            kCompFail,
            MakeDefinition({{"value_in", "int"}}, {{"value_out", "int"}}));

        token_ =
            Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();
    }

    CompiledGraphPlanDto CompileDocument(const GraphDocumentDto& doc)
    {
        GraphCompiler compiler;
        compiler.SetFactoryManager(stub_mgr_.get());
        return compiler.Compile(doc);
    }

    std::vector<CompileEdgeDiagnostic> ValidateEdgePorts(
        const GraphDocumentDto& doc)
    {
        GraphCompiler compiler;
        compiler.SetFactoryManager(stub_mgr_.get());
        return compiler.ValidateEdgePorts(doc);
    }

    std::vector<std::string> ComputeExecutionOrder(const GraphDocumentDto& doc)
    {
        GraphCompiler compiler;
        return compiler.ComputeExecutionOrder(doc);
    }

    std::unique_ptr<StubFactoryManager> stub_mgr_;
    DAS::DasPtr<IDasStopToken>          token_;
};