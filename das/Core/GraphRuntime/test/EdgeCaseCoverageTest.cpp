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

    DasResult ExecutePlan(
        const CompiledGraphPlanDto& plan,
        ComponentResolver           resolver,
        IDasStopToken*              stop_token = nullptr)
    {
        GraphRuntime rt;
        return rt.Run(
            plan,
            plan.source_fingerprint,
            stop_token ? stop_token : token_.Get(),
            std::move(resolver));
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

// ===========================================================================
// Group 1: Empty / Trivial Graphs
// ===========================================================================

// Test 1: Empty graph (zero nodes) compiles — execution_order is empty
TEST_F(EdgeCaseCoverageTest, EmptyGraph_CompilesWithEmptyExecutionOrder)
{
    GraphDocumentDto doc = MakeEmptyDocument();
    // nodes and edges are empty by default

    auto plan = CompileDocument(doc);
    EXPECT_TRUE(plan.execution_order.empty());
    EXPECT_TRUE(plan.binding_plan.bindings.empty());
    EXPECT_EQ(plan.source_fingerprint, "fp_edge_v1");
    // Compile succeeds (best-effort) — empty graph is valid
    EXPECT_TRUE(plan.node_snapshots.empty());
}

// Test 2: Empty graph Run() returns DAS_S_OK — no crash
TEST_F(EdgeCaseCoverageTest, EmptyGraph_RunReturnsOk)
{
    CompiledGraphPlanDto empty_plan;
    empty_plan.source_fingerprint = "fp_empty";
    empty_plan.compiled_fingerprint = "compiled_fp_empty";
    // No snapshots, no execution_order, no bindings

    int  call_count = 0;
    auto resolver = [&](const std::string&,
                        IDasStopToken*,
                        IDasPortMap*,
                        IDasPortMap**) -> DasResult
    {
        ++call_count;
        return DAS_S_OK;
    };

    GraphRuntime rt;
    auto         hr = rt.Run(empty_plan, "fp_empty", token_.Get(), resolver);
    EXPECT_EQ(hr, DAS_S_OK);
    EXPECT_EQ(call_count, 0); // No nodes to execute
}

// Test 3: Single-node graph without edges compiles and executes
TEST_F(EdgeCaseCoverageTest, SingleNodeNoEdges_Executes)
{
    // Compile validates the graph structure
    auto doc = MakeSingleNodeDocument(kNodeA, kCompPassThrough);
    auto compile_plan = CompileDocument(doc);
    EXPECT_EQ(compile_plan.execution_order.size(), 1u);
    EXPECT_TRUE(compile_plan.binding_plan.bindings.empty());

    // Build a complete plan manually for execution (Compile does not
    // generate full node_snapshots needed by GraphRuntime::Run)
    CompiledGraphPlanDto plan;
    plan.source_fingerprint = "fp_v1";
    plan.compiled_fingerprint = "compiled_fp_v1";
    plan.node_snapshots.push_back(MakeSnapshot(
        kNodeA,
        kCompPassThrough,
        {MakePortDef("value_in"), MakePortDef("value_out")}));
    plan.execution_order = {kNodeA};

    bool executed = false;
    auto resolver = [&](const std::string&,
                        IDasStopToken*,
                        IDasPortMap*  input_map,
                        IDasPortMap** pp_out_map) -> DasResult
    {
        DasResult hr = CreateIDasPortMap(pp_out_map);
        if (DAS_S_OK != hr)
            return hr;

        executed = true;
        // Node has no upstream data — input_map may be null or empty
        int64_t val = 0;
        if (input_map)
        {
            val = PortMapGetInt(input_map, "value_in");
        }
        PortMapSetInt(*pp_out_map, "value_out", val + 1);
        return DAS_S_OK;
    };

    auto hr = ExecutePlan(plan, resolver);
    EXPECT_EQ(hr, DAS_S_OK);
    EXPECT_TRUE(executed);
}

// ===========================================================================
// Group 2: Disconnected / Multi-node No Edges
// ===========================================================================

// Test 4: Multiple disconnected nodes all execute independently
TEST_F(EdgeCaseCoverageTest, MultiNodeNoEdges_AllExecuteIndependently)
{
    auto doc = MakeEmptyDocument();
    doc.nodes.push_back(MakeComponentNode(kNodeA, kCompPassThrough));
    doc.nodes.push_back(MakeComponentNode(kNodeB, kCompPassThrough));
    doc.nodes.push_back(MakeComponentNode(kNodeC, kCompPassThrough));
    doc.nodes.push_back(MakeComponentNode(kNodeD, kCompPassThrough));
    doc.nodes.push_back(MakeComponentNode(kNodeE, kCompPassThrough));
    // No edges — all disconnected

    // Verify compile produces correct topological order
    auto compile_plan = CompileDocument(doc);
    EXPECT_EQ(compile_plan.execution_order.size(), 5u);
    EXPECT_TRUE(compile_plan.binding_plan.bindings.empty());

    const auto& order = compile_plan.execution_order;
    EXPECT_NE(std::find(order.begin(), order.end(), kNodeA), order.end());
    EXPECT_NE(std::find(order.begin(), order.end(), kNodeB), order.end());
    EXPECT_NE(std::find(order.begin(), order.end(), kNodeC), order.end());
    EXPECT_NE(std::find(order.begin(), order.end(), kNodeD), order.end());
    EXPECT_NE(std::find(order.begin(), order.end(), kNodeE), order.end());

    // Build a complete plan manually for execution
    CompiledGraphPlanDto plan;
    plan.source_fingerprint = "fp_v1";
    plan.compiled_fingerprint = "compiled_fp_v1";
    for (const auto& nid : compile_plan.execution_order)
    {
        plan.node_snapshots.push_back(MakeSnapshot(
            nid,
            kCompPassThrough,
            {MakePortDef("value_in"), MakePortDef("value_out")}));
    }
    plan.execution_order = compile_plan.execution_order;

    int  nodes_executed = 0;
    auto resolver = [&](const std::string&,
                        IDasStopToken*,
                        IDasPortMap*,
                        IDasPortMap** pp_out_map) -> DasResult
    {
        DasResult hr = CreateIDasPortMap(pp_out_map);
        if (DAS_S_OK != hr)
            return hr;
        ++nodes_executed;
        PortMapSetInt(*pp_out_map, "value_out", 1);
        return DAS_S_OK;
    };

    auto hr = ExecutePlan(plan, resolver);
    EXPECT_EQ(hr, DAS_S_OK);
    EXPECT_EQ(nodes_executed, 5);
}

// ===========================================================================
// Group 3: Port Validation (v44/v45)
// ===========================================================================

// Test 5: Invalid source port_id → compile error with diagnostic
TEST_F(EdgeCaseCoverageTest, InvalidSourcePort_CompileError)
{
    auto doc = MakeEmptyDocument();
    doc.nodes.push_back(MakeComponentNode(kNodeA, kCompPassThrough));
    doc.nodes.push_back(MakeComponentNode(kNodeB, kCompPassThrough));
    // kCompPassThrough has outputs: "value_out" — reference nonexistent port
    doc.edges.push_back(
        MakeEdge("e1", kNodeA, "nonexistent_port", kNodeB, "value_in"));

    auto diagnostics = ValidateEdgePorts(doc);
    ASSERT_FALSE(diagnostics.empty());

    bool found_source_error = false;
    for (const auto& d : diagnostics)
    {
        if (d.kind == CompileDiagnosticKind::PortNotFound && d.node_id == kNodeA
            && d.port_id == "nonexistent_port")
        {
            found_source_error = true;
            EXPECT_EQ(
                d.direction,
                CompileEdgeDiagnostic::PortDirection::Output);
            break;
        }
    }
    EXPECT_TRUE(found_source_error)
        << "Expected PortNotFound diagnostic for source node";
}

// Test 6: Invalid target port_id → compile error with diagnostic
TEST_F(EdgeCaseCoverageTest, InvalidTargetPort_CompileError)
{
    auto doc = MakeEmptyDocument();
    doc.nodes.push_back(MakeComponentNode(kNodeA, kCompPassThrough));
    doc.nodes.push_back(MakeComponentNode(kNodeB, kCompPassThrough));
    // kCompPassThrough has inputs: "value_in" — reference nonexistent port
    doc.edges.push_back(
        MakeEdge("e1", kNodeA, "value_out", kNodeB, "nonexistent_port"));

    auto diagnostics = ValidateEdgePorts(doc);
    ASSERT_FALSE(diagnostics.empty());

    bool found_target_error = false;
    for (const auto& d : diagnostics)
    {
        if (d.kind == CompileDiagnosticKind::PortNotFound && d.node_id == kNodeB
            && d.port_id == "nonexistent_port")
        {
            found_target_error = true;
            EXPECT_EQ(d.direction, CompileEdgeDiagnostic::PortDirection::Input);
            break;
        }
    }
    EXPECT_TRUE(found_target_error)
        << "Expected PortNotFound diagnostic for target node";
}

// Test 7: Port type mismatch (INT→STRING) → compile error
TEST_F(EdgeCaseCoverageTest, PortTypeMismatch_IntToString)
{
    auto doc = MakeEmptyDocument();
    doc.nodes.push_back(MakeComponentNode(kNodeA, kCompIntOut));
    doc.nodes.push_back(MakeComponentNode(kNodeB, kCompStringOut));
    // kCompIntOut outputs int, kCompStringOut inputs string → mismatch
    doc.edges.push_back(
        MakeEdge("e1", kNodeA, "value_out", kNodeB, "value_in"));

    auto diagnostics = ValidateEdgePorts(doc);
    ASSERT_FALSE(diagnostics.empty());

    bool found_type_mismatch = false;
    for (const auto& d : diagnostics)
    {
        if (d.kind == CompileDiagnosticKind::TypeMismatch)
        {
            found_type_mismatch = true;
            EXPECT_EQ(d.actual_type, "int");
            EXPECT_EQ(d.expected_type, "string");
            break;
        }
    }
    EXPECT_TRUE(found_type_mismatch)
        << "Expected TypeMismatch diagnostic for INT→STRING edge";
}

// ===========================================================================
// Group 4: Cycles
// ===========================================================================

// Test 8: Circular dependency (A→B→C→A) → toposort returns empty
TEST_F(EdgeCaseCoverageTest, CircularDependency_Triangular)
{
    auto doc = MakeEmptyDocument();
    doc.nodes.push_back(MakeComponentNode(kNodeA, kCompPassThrough));
    doc.nodes.push_back(MakeComponentNode(kNodeB, kCompPassThrough));
    doc.nodes.push_back(MakeComponentNode(kNodeC, kCompPassThrough));
    doc.edges.push_back(
        MakeEdge("e1", kNodeA, "value_out", kNodeB, "value_in"));
    doc.edges.push_back(
        MakeEdge("e2", kNodeB, "value_out", kNodeC, "value_in"));
    doc.edges.push_back(
        MakeEdge("e3", kNodeC, "value_out", kNodeA, "value_in"));

    auto order = ComputeExecutionOrder(doc);
    EXPECT_TRUE(order.empty())
        << "Circular dependency should produce empty execution order";
}

// Test 9: Self-referencing edge (A→A) → toposort returns empty
TEST_F(EdgeCaseCoverageTest, SelfReferencingEdge_CompileError)
{
    auto doc = MakeEmptyDocument();
    doc.nodes.push_back(MakeComponentNode(kNodeA, kCompPassThrough));
    doc.edges.push_back(
        MakeEdge("e1", kNodeA, "value_out", kNodeA, "value_in"));

    auto order = ComputeExecutionOrder(doc);
    EXPECT_TRUE(order.empty())
        << "Self-referencing edge should produce empty execution order";
}

// Test 10: Duplicate node_id → ApplySettingsChange rejects
TEST_F(EdgeCaseCoverageTest, DuplicateNodeId_AuthoringRejects)
{
    auto doc = MakeEmptyDocument();
    doc.nodes.push_back(MakeAuthoringNode("duplicate_id"));
    doc.nodes.push_back(MakeAuthoringNode("duplicate_id"));

    // Compile with duplicate nodes — GraphCompiler does not enforce uniqueness
    // but ApplySettingsChange does when adding nodes
    auto doc2 = MakeEmptyDocument();
    {
        GraphAuthoringChange change{AddNodeChange{MakeAuthoringNode("dup_id")}};
        EXPECT_TRUE(ApplySettingsChange(doc2, change).Ok());
    }
    {
        GraphAuthoringChange change{AddNodeChange{MakeAuthoringNode("dup_id")}};
        auto                 result = ApplySettingsChange(doc2, change);
        EXPECT_FALSE(result.Ok());
        EXPECT_EQ(result.error_kind, AuthoringErrorKind::DuplicateNodeId);
    }
}

// ===========================================================================
// Group 5: Error Handling / Null Safety
// ===========================================================================

// Test 11: Malformed JSON → ParseYyjsonFromString returns error gracefully
TEST_F(EdgeCaseCoverageTest, MalformedJson_ParsedGracefully)
{
    // Test that yyjson parsing handles malformed input without crash
    auto result = Das::Utils::ParseYyjsonFromString("{this is not valid json");
    EXPECT_FALSE(result.has_value());

    // Verify empty string doesn't crash
    auto result2 = Das::Utils::ParseYyjsonFromString("");
    EXPECT_FALSE(result2.has_value());

    // Verify valid JSON succeeds for contrast
    auto result3 = Das::Utils::ParseYyjsonFromString(R"({"key": "value"})");
    EXPECT_TRUE(result3.has_value());
}

// Test 12: Null stop token → Run handles gracefully (no crash)
TEST_F(EdgeCaseCoverageTest, NullStopToken_RunHandled)
{
    auto plan = MakeSingleSnapshotPlan(
        kNodeA,
        kCompPassThrough,
        {MakePortDef("value_in"), MakePortDef("value_out")});

    bool executed = false;
    auto resolver = [&](const std::string&,
                        IDasStopToken*,
                        IDasPortMap*,
                        IDasPortMap** pp_out_map) -> DasResult
    {
        DasResult hr = CreateIDasPortMap(pp_out_map);
        if (DAS_S_OK != hr)
            return hr;
        executed = true;
        PortMapSetInt(*pp_out_map, "value_out", 42);
        return DAS_S_OK;
    };

    // Pass nullptr stop_token — GraphRuntime should handle this
    GraphRuntime rt;
    auto         hr = rt.Run(plan, "fp_v1", nullptr, resolver);
    // Either succeeds (treating nullptr as no-cancel) or returns error
    // The key assertion: NO CRASH
    EXPECT_TRUE(hr == DAS_S_OK || DAS::IsFailed(hr));
}

// Test 13: Run with stop token cancelled → immediate rejection
TEST_F(EdgeCaseCoverageTest, PreCancelledStopToken_StopsExecution)
{
    auto plan = MakeSingleSnapshotPlan(
        kNodeA,
        kCompPassThrough,
        {MakePortDef("value_out")});

    int  call_count = 0;
    auto resolver = [&](const std::string&,
                        IDasStopToken*,
                        IDasPortMap*,
                        IDasPortMap**) -> DasResult
    {
        ++call_count;
        return DAS_S_OK;
    };

    auto cancelled_token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();
    static_cast<MockStopToken*>(cancelled_token.Get())->cancelled.store(true);

    GraphRuntime rt;
    auto         hr = rt.Run(plan, "fp_v1", cancelled_token.Get(), resolver);
    EXPECT_NE(hr, DAS_S_OK);
    EXPECT_EQ(call_count, 0);
}

// Test 14: Multiple validation errors aggregated in diagnostics
TEST_F(EdgeCaseCoverageTest, MultipleValidationErrors_Aggregated)
{
    auto doc = MakeEmptyDocument();
    doc.nodes.push_back(MakeComponentNode(kNodeA, kCompPassThrough));
    doc.nodes.push_back(MakeComponentNode(kNodeB, kCompStringOut));
    doc.nodes.push_back(MakeComponentNode(kNodeC, kCompPassThrough));

    // Error 1: Invalid source port on edge 1
    doc.edges.push_back(MakeEdge("e1", kNodeA, "bad_port", kNodeB, "value_in"));
    // Error 2: Type mismatch on edge 2 (int → string)
    doc.edges.push_back(
        MakeEdge("e2", kNodeA, "value_out", kNodeB, "value_in"));
    // Error 3: Invalid target port on edge 3
    doc.edges.push_back(
        MakeEdge("e3", kNodeA, "value_out", kNodeC, "bad_port"));

    auto diagnostics = ValidateEdgePorts(doc);
    // Should have at least 3 separate diagnostics
    EXPECT_GE(diagnostics.size(), 3u)
        << "Expected >= 3 diagnostics, got " << diagnostics.size();

    // Verify they are distinct issues
    int port_not_found_count = 0;
    int type_mismatch_count = 0;
    for (const auto& d : diagnostics)
    {
        if (d.kind == CompileDiagnosticKind::PortNotFound)
            ++port_not_found_count;
        if (d.kind == CompileDiagnosticKind::TypeMismatch)
            ++type_mismatch_count;
    }
    EXPECT_GE(port_not_found_count, 2)
        << "Expected >= 2 PortNotFound diagnostics";
    EXPECT_GE(type_mismatch_count, 1)
        << "Expected >= 1 TypeMismatch diagnostic";
}

// ===========================================================================
// Group 6: Authoring Lifecycle
// ===========================================================================

// Test 15: Authoring apply → compile → apply → stale fingerprint
TEST_F(EdgeCaseCoverageTest, Authoring_ApplyCompileApply_Stale)
{
    // Step 1: Create document with initial settings
    auto doc = MakeEmptyDocument();
    doc.nodes.push_back(MakeComponentNode(kNodeA, kCompPassThrough));
    doc.fingerprint = "fp_v1";

    // Step 2: Compile with fp_v1
    auto plan = CompileDocument(doc);
    EXPECT_EQ(plan.source_fingerprint, "fp_v1");

    // Step 3: Author settings change → fingerprint would change
    // (In real system, fingerprint changes when document changes)
    auto                 new_settings = MakeSettingsValue("threshold", 0.9);
    GraphAuthoringChange change{
        UpdateNodeConfigChange{kNodeA, std::move(new_settings)}};
    auto result = ApplySettingsChange(doc, change);
    EXPECT_TRUE(result.Ok());

    // Step 4: Run with old fingerprint → should detect staleness
    GraphRuntime rt;
    auto         hr = rt.Run(
        plan,
        "fp_v2_changed", // mismatched fingerprint
        token_.Get(),
        [](const std::string&,
           IDasStopToken*,
           IDasPortMap*,
           IDasPortMap** pp_out_map) -> DasResult
        { return CreateIDasPortMap(pp_out_map); });

    // ValidateFingerprint should fail — DAS_E_FAIL on mismatch
    EXPECT_NE(hr, DAS_S_OK) << "Stale fingerprint should be rejected";
}

// Test 16: Sequential ApplySettingsChange → last write wins
TEST_F(EdgeCaseCoverageTest, Authoring_ConcurrentApply_LastWriteWins)
{
    auto doc = MakeEmptyDocument();
    doc.nodes.push_back(MakeAuthoringNode("node_1"));

    // First apply: set threshold to 0.5
    {
        auto                 settings = MakeSettingsValue("threshold", 0.5);
        GraphAuthoringChange change{
            UpdateNodeConfigChange{"node_1", std::move(settings)}};
        EXPECT_TRUE(ApplySettingsChange(doc, change).Ok());
    }

    // Second apply: set threshold to 0.9 — overwrites
    {
        auto                 settings = MakeSettingsValue("threshold", 0.9);
        GraphAuthoringChange change{
            UpdateNodeConfigChange{"node_1", std::move(settings)}};
        EXPECT_TRUE(ApplySettingsChange(doc, change).Ok());
    }

    // Verify the second value (0.9) is present, not the first (0.5)
    auto json_str = std::string(doc.nodes[0].settings.write());
    EXPECT_TRUE(json_str.find("0.9") != std::string::npos)
        << "Expected last-write-wins with value 0.9, got: " << json_str;
    EXPECT_TRUE(json_str.find("threshold") != std::string::npos)
        << "Expected 'threshold' key in settings, got: " << json_str;
}

// ===========================================================================
// Group 7: Integration
// ===========================================================================

// Test 17: Disconnected subgraphs execute independently without data leaks
TEST_F(EdgeCaseCoverageTest, DisconnectedSubgraph_ExecutesWithoutInput)
{
    // Group 1: A→B, Group 2: C→D — no edges between groups
    auto doc = MakeEmptyDocument();
    doc.nodes.push_back(MakeComponentNode(kNodeA, kCompPassThrough));
    doc.nodes.push_back(MakeComponentNode(kNodeB, kCompPassThrough));
    doc.nodes.push_back(MakeComponentNode(kNodeC, kCompPassThrough));
    doc.nodes.push_back(MakeComponentNode(kNodeD, kCompPassThrough));

    // Group 1 edge
    doc.edges.push_back(
        MakeEdge("e1", kNodeA, "value_out", kNodeB, "value_in"));
    // Group 2 edge
    doc.edges.push_back(
        MakeEdge("e2", kNodeC, "value_out", kNodeD, "value_in"));

    // Verify compile produces correct order
    auto compile_plan = CompileDocument(doc);
    EXPECT_EQ(compile_plan.execution_order.size(), 4u);
    EXPECT_EQ(compile_plan.binding_plan.bindings.size(), 2u);

    const auto& order = compile_plan.execution_order;
    auto        a_pos = std::find(order.begin(), order.end(), kNodeA);
    auto        b_pos = std::find(order.begin(), order.end(), kNodeB);
    auto        c_pos = std::find(order.begin(), order.end(), kNodeC);
    auto        d_pos = std::find(order.begin(), order.end(), kNodeD);

    ASSERT_NE(a_pos, order.end());
    ASSERT_NE(b_pos, order.end());
    ASSERT_NE(c_pos, order.end());
    ASSERT_NE(d_pos, order.end());

    EXPECT_LT(a_pos, b_pos); // A before B
    EXPECT_LT(c_pos, d_pos); // C before D

    // Build a complete plan manually for execution
    CompiledGraphPlanDto plan;
    plan.source_fingerprint = "fp_v1";
    plan.compiled_fingerprint = "compiled_fp_v1";
    for (const auto& nid : compile_plan.execution_order)
    {
        plan.node_snapshots.push_back(MakeSnapshot(
            nid,
            kCompPassThrough,
            {MakePortDef("value_in"), MakePortDef("value_out")}));
    }
    plan.execution_order = compile_plan.execution_order;
    plan.binding_plan = compile_plan.binding_plan;

    // Execute: verify all 4 nodes run with no data cross-contamination
    std::map<std::string, int64_t> node_outputs;
    int                            node_idx = 0;

    auto resolver = [&](const std::string& component_guid,
                        IDasStopToken*,
                        IDasPortMap*  input_map,
                        IDasPortMap** pp_out_map) -> DasResult
    {
        DasResult hr = CreateIDasPortMap(pp_out_map);
        if (DAS_S_OK != hr)
            return hr;

        std::string current_node = plan.execution_order[node_idx++];

        int64_t val = 0;
        if (input_map && PortMapHas(input_map, "value_in"))
        {
            val = PortMapGetInt(input_map, "value_in");
        }
        else
        {
            // Source node — seed with unique value per group
            if (current_node == kNodeA)
                val = 100;
            else if (current_node == kNodeC)
                val = 200;
        }
        node_outputs[current_node] = val;
        PortMapSetInt(*pp_out_map, "value_out", val);
        return DAS_S_OK;
    };

    auto hr = ExecutePlan(plan, resolver);
    EXPECT_EQ(hr, DAS_S_OK);

    // Verify group 1: A=100, B receives 100
    EXPECT_EQ(node_outputs[kNodeA], 100);
    EXPECT_EQ(node_outputs[kNodeB], 100);

    // Verify group 2: C=200, D receives 200
    EXPECT_EQ(node_outputs[kNodeC], 200);
    EXPECT_EQ(node_outputs[kNodeD], 200);
}

// Test 18: Fingerprint mismatch → Run rejects with DAS_E_FAIL
TEST_F(EdgeCaseCoverageTest, FingerprintMismatch_RunRejects)
{
    auto plan = MakeSingleSnapshotPlan(
        kNodeA,
        kCompPassThrough,
        {MakePortDef("value_out")});
    plan.source_fingerprint = "fp_original";

    int  call_count = 0;
    auto resolver = [&](const std::string&,
                        IDasStopToken*,
                        IDasPortMap*,
                        IDasPortMap** pp_out_map) -> DasResult
    {
        ++call_count;
        return CreateIDasPortMap(pp_out_map);
    };

    // Run with mismatched fingerprint
    GraphRuntime rt;
    auto         hr = rt.Run(plan, "fp_changed", token_.Get(), resolver);
    EXPECT_NE(hr, DAS_S_OK) << "Fingerprint mismatch should cause Run to fail";
    EXPECT_EQ(call_count, 0)
        << "No nodes should execute when fingerprint is stale";

    // Verify GetLastErrorMessage is descriptive
    const auto& msg = rt.GetLastErrorMessage();
    EXPECT_FALSE(msg.empty());
}
