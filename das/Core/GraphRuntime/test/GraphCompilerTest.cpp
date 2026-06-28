#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>
#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/GraphCompiler.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>
#include <utility>

namespace
{
    using namespace Das::Core::GraphRuntime;
    using namespace Das::Core::GraphRuntime::Dto;
    using namespace Das::Core::ForeignInterfaceHost;

    // ---------------------------------------------------------------
    // Helpers to build synthetic manifest definitions (yyjson::value)
    // via JSON string parsing — avoids mutable yyjson array API issues
    // ---------------------------------------------------------------

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

    yyjson::value MakeEmptyDefinition() { return MakeDefinition({}, {}); }

    yyjson::value MakeDefinitionNoPorts()
    {
        return Das::Utils::MakeYyjsonObject();
    }

    // ---------------------------------------------------------------
    // Helpers to build GraphDocumentDto fixtures
    // ---------------------------------------------------------------

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

    GraphNodeDto MakeEntryRefNode(const std::string& node_id, int64_t entry_id)
    {
        GraphNodeDto node;
        node.node_id = node_id;
        node.target.target_kind = "entryRef";
        EntryRefDto entry_ref;
        entry_ref.kind = "entryRef";
        entry_ref.entry_id = entry_id;
        node.target.entry_ref = entry_ref;
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

    GraphEdgeDto MakeSignalEdge(
        const std::string& edge_id,
        const std::string& src_node,
        const std::string& src_port,
        const std::string& tgt_node,
        const std::string& tgt_port)
    {
        GraphEdgeDto edge =
            MakeEdge(edge_id, src_node, src_port, tgt_node, tgt_port);
        edge.edge_type = "signal";
        return edge;
    }

    GraphPortDefinitionDto MakeGraphInput(
        const std::string& port_id,
        const std::string& port_type,
        const std::string& default_json)
    {
        GraphPortDefinitionDto port;
        port.port_id = port_id;
        port.port_type = port_type;
        auto parsed = Das::Utils::ParseYyjsonFromString(default_json);
        if (parsed.has_value())
        {
            port.default_value = std::move(*parsed);
        }
        return port;
    }

    // CyclicEdgeGraph diagnostic kind serialised as a string of its enum int.
    bool HasCyclicEdgeGraphDiagnostic(const Dto::CompiledGraphPlanDto& plan)
    {
        const std::string cyclic_kind = std::to_string(
            static_cast<int>(CompileDiagnosticKind::CyclicEdgeGraph));
        for (const auto& diag : plan.diagnostics)
        {
            auto obj = diag.as_object();
            if (!obj.has_value())
            {
                continue;
            }
            auto kind = (*obj)[std::string_view("kind")].as_string();
            if (kind.has_value() && *kind == cyclic_kind)
            {
                return true;
            }
        }
        return false;
    }

    // ---------------------------------------------------------------
    // Stub TaskComponentFactoryManager for testing
    // Overrides EnumerateDefinitions to return injected test data
    // ---------------------------------------------------------------

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

} // namespace

// ===================================================================
// Test Suite 1: ManifestReadingTest
// ===================================================================

TEST(ManifestReadingTest, ReadsInputPorts)
{
    StubFactoryManager mgr;
    auto               definition =
        MakeDefinition({{"in1", "int"}, {"in2", "string"}}, {{"out1", "bool"}});
    mgr.AddDefinition("11111111-1111-1111-1111-111111111111", definition);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    auto ports = compiler.ReadManifest("11111111-1111-1111-1111-111111111111");
    ASSERT_EQ(ports.inputs.size(), 2u);
    EXPECT_EQ(ports.inputs[0].port_id, "in1");
    EXPECT_EQ(ports.inputs[0].port_type, "int");
    EXPECT_EQ(ports.inputs[1].port_id, "in2");
    EXPECT_EQ(ports.inputs[1].port_type, "string");
    ASSERT_EQ(ports.outputs.size(), 1u);
    EXPECT_EQ(ports.outputs[0].port_id, "out1");
    EXPECT_EQ(ports.outputs[0].port_type, "bool");
}

TEST(ManifestReadingTest, ReadsOutputPorts)
{
    StubFactoryManager mgr;
    auto definition = MakeDefinition({}, {{"out1", "bool"}, {"out2", "image"}});
    mgr.AddDefinition("22222222-2222-2222-2222-222222222222", definition);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    auto ports = compiler.ReadManifest("22222222-2222-2222-2222-222222222222");
    EXPECT_EQ(ports.inputs.size(), 0u);
    ASSERT_EQ(ports.outputs.size(), 2u);
    EXPECT_EQ(ports.outputs[0].port_id, "out1");
    EXPECT_EQ(ports.outputs[0].port_type, "bool");
    EXPECT_EQ(ports.outputs[1].port_id, "out2");
    EXPECT_EQ(ports.outputs[1].port_type, "image");
}

TEST(ManifestReadingTest, ReadsEmptyManifest)
{
    StubFactoryManager mgr;
    mgr.AddDefinition(
        "33333333-3333-3333-3333-333333333333",
        MakeEmptyDefinition());

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    auto ports = compiler.ReadManifest("33333333-3333-3333-3333-333333333333");
    EXPECT_EQ(ports.inputs.size(), 0u);
    EXPECT_EQ(ports.outputs.size(), 0u);
}

TEST(ManifestReadingTest, ReadsMissingFields)
{
    StubFactoryManager mgr;
    mgr.AddDefinition(
        "44444444-4444-4444-4444-444444444444",
        MakeDefinitionNoPorts());

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    auto ports = compiler.ReadManifest("44444444-4444-4444-4444-444444444444");
    EXPECT_EQ(ports.inputs.size(), 0u);
    EXPECT_EQ(ports.outputs.size(), 0u);
}

// ===================================================================
// Test Suite 2: EdgePortExistenceTest
// ===================================================================

TEST(EdgePortExistenceTest, ValidEdgeBothPortsExist)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "int"}});
    auto               node_b_def = MakeDefinition({{"in1", "int"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_a_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_b_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("nodeA", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("nodeB", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.edges.push_back(MakeEdge("e1", "nodeA", "out1", "nodeB", "in1"));

    auto diagnostics = compiler.ValidateEdgePorts(doc);
    EXPECT_TRUE(diagnostics.empty());
}

TEST(EdgePortExistenceTest, SourcePortNotFound)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "int"}});
    auto               node_b_def = MakeDefinition({{"in1", "int"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_a_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_b_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("nodeA", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("nodeB", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.edges.push_back(MakeEdge("e1", "nodeA", "nonexistent", "nodeB", "in1"));

    auto diagnostics = compiler.ValidateEdgePorts(doc);
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_EQ(diagnostics[0].kind, CompileDiagnosticKind::PortNotFound);
    EXPECT_EQ(diagnostics[0].node_id, "nodeA");
    EXPECT_EQ(diagnostics[0].port_id, "nonexistent");
    EXPECT_EQ(
        diagnostics[0].direction,
        CompileEdgeDiagnostic::PortDirection::Output);
}

TEST(EdgePortExistenceTest, TargetPortNotFound)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "int"}});
    auto               node_b_def = MakeDefinition({{"in1", "int"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_a_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_b_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("nodeA", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("nodeB", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.edges.push_back(
        MakeEdge("e1", "nodeA", "out1", "nodeB", "nonexistent"));

    auto diagnostics = compiler.ValidateEdgePorts(doc);
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_EQ(diagnostics[0].kind, CompileDiagnosticKind::PortNotFound);
    EXPECT_EQ(diagnostics[0].node_id, "nodeB");
    EXPECT_EQ(diagnostics[0].port_id, "nonexistent");
    EXPECT_EQ(
        diagnostics[0].direction,
        CompileEdgeDiagnostic::PortDirection::Input);
}

TEST(EdgePortExistenceTest, NodeNotFound)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "int"}});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_a_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("nodeA", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.edges.push_back(MakeEdge("e1", "nodeA", "out1", "missingNode", "in1"));

    auto diagnostics = compiler.ValidateEdgePorts(doc);
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_EQ(diagnostics[0].kind, CompileDiagnosticKind::NodeNotFound);
}

TEST(EdgePortExistenceTest, NoEdgesGraph)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "int"}});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_a_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("nodeA", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));

    auto diagnostics = compiler.ValidateEdgePorts(doc);
    EXPECT_TRUE(diagnostics.empty());
}

// ===================================================================
// Test Suite 3: PortTypeCompatibilityTest
// ===================================================================

TEST(PortTypeCompatibilityTest, SameTypeCompatible)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "int"}});
    auto               node_b_def = MakeDefinition({{"in1", "int"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_a_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_b_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("nodeA", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("nodeB", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.edges.push_back(MakeEdge("e1", "nodeA", "out1", "nodeB", "in1"));

    auto diagnostics = compiler.ValidateEdgePorts(doc);
    EXPECT_TRUE(diagnostics.empty());
}

TEST(PortTypeCompatibilityTest, ImageToImageCompatible)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "image"}});
    auto               node_b_def = MakeDefinition({{"in1", "image"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_a_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_b_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("nodeA", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("nodeB", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.edges.push_back(MakeEdge("e1", "nodeA", "out1", "nodeB", "in1"));

    auto diagnostics = compiler.ValidateEdgePorts(doc);
    EXPECT_TRUE(diagnostics.empty());
}

TEST(PortTypeCompatibilityTest, TypeMismatch)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "image"}});
    auto               node_b_def = MakeDefinition({{"in1", "string"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_a_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_b_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("nodeA", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("nodeB", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.edges.push_back(MakeEdge("e1", "nodeA", "out1", "nodeB", "in1"));

    auto diagnostics = compiler.ValidateEdgePorts(doc);
    ASSERT_FALSE(diagnostics.empty());
    bool found_type_mismatch = false;
    for (const auto& d : diagnostics)
    {
        if (d.kind == CompileDiagnosticKind::TypeMismatch)
        {
            found_type_mismatch = true;
            EXPECT_EQ(d.expected_type, "string");
            EXPECT_EQ(d.actual_type, "image");
            break;
        }
    }
    EXPECT_TRUE(found_type_mismatch);
}

TEST(PortTypeCompatibilityTest, BaseToComponentCompatible)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "base"}});
    auto               node_b_def = MakeDefinition({{"in1", "component"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_a_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_b_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("nodeA", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("nodeB", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.edges.push_back(MakeEdge("e1", "nodeA", "out1", "nodeB", "in1"));

    auto diagnostics = compiler.ValidateEdgePorts(doc);
    EXPECT_TRUE(diagnostics.empty());
}

TEST(PortTypeCompatibilityTest, ComponentToBaseCompatible)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "component"}});
    auto               node_b_def = MakeDefinition({{"in1", "base"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_a_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_b_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("nodeA", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("nodeB", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.edges.push_back(MakeEdge("e1", "nodeA", "out1", "nodeB", "in1"));

    auto diagnostics = compiler.ValidateEdgePorts(doc);
    EXPECT_TRUE(diagnostics.empty());
}

TEST(PortTypeCompatibilityTest, UnknownTypeWarning)
{
    StubFactoryManager mgr;
    auto node_a_def = MakeDefinition({}, {{"out1", "unknown_type"}});
    auto node_b_def = MakeDefinition({{"in1", "unknown_type"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_a_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_b_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("nodeA", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("nodeB", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.edges.push_back(MakeEdge("e1", "nodeA", "out1", "nodeB", "in1"));

    auto diagnostics = compiler.ValidateEdgePorts(doc);
    bool found_unknown = false;
    for (const auto& d : diagnostics)
    {
        if (d.kind == CompileDiagnosticKind::UnknownPortType)
        {
            found_unknown = true;
            break;
        }
    }
    EXPECT_TRUE(found_unknown);
}

// ===================================================================
// Test Suite 4: ComponentRefTargetResolutionTest
// ===================================================================

TEST(ComponentRefTargetResolutionTest, ResolveComponentRefNode)
{
    StubFactoryManager mgr;
    auto               node_def =
        MakeDefinition({{"data_in", "image"}}, {{"data_out", "image"}});
    mgr.AddDefinition("CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC", node_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("node1", "CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC"));
    doc.edges.push_back(
        MakeEdge("e1", "node1", "data_out", "node1", "data_in"));

    auto diagnostics = compiler.ValidateEdgePorts(doc);
    EXPECT_TRUE(diagnostics.empty());
}

TEST(ComponentRefTargetResolutionTest, UnresolvableComponentGuid)
{
    StubFactoryManager mgr;

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("node1", "CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC"));
    doc.nodes.push_back(
        MakeComponentNode("node2", "DDDDDDDD-DDDD-DDDD-DDDD-DDDDDDDDDDDD"));
    doc.edges.push_back(MakeEdge("e1", "node1", "out1", "node2", "in1"));

    auto diagnostics = compiler.ValidateEdgePorts(doc);
    ASSERT_FALSE(diagnostics.empty());
    bool found_unresolvable = false;
    for (const auto& d : diagnostics)
    {
        if (d.kind == CompileDiagnosticKind::UnresolvableComponentGuid)
        {
            found_unresolvable = true;
            break;
        }
    }
    EXPECT_TRUE(found_unresolvable);
}

// ===================================================================
// Test Suite 5: ToposortTest
// ===================================================================

TEST(ToposortTest, LinearChain)
{
    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("A", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("B", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.nodes.push_back(
        MakeComponentNode("C", "CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC"));
    doc.edges.push_back(MakeEdge("e1", "A", "out", "B", "in"));
    doc.edges.push_back(MakeEdge("e2", "B", "out", "C", "in"));

    GraphCompiler compiler;
    auto          order = compiler.ComputeExecutionOrder(doc);

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], "A");
    EXPECT_EQ(order[1], "B");
    EXPECT_EQ(order[2], "C");
}

TEST(ToposortTest, Diamond)
{
    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("A", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("B", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.nodes.push_back(
        MakeComponentNode("C", "CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC"));
    doc.nodes.push_back(
        MakeComponentNode("D", "DDDDDDDD-DDDD-DDDD-DDDD-DDDDDDDDDDDD"));
    doc.edges.push_back(MakeEdge("e1", "A", "out1", "B", "in"));
    doc.edges.push_back(MakeEdge("e2", "A", "out2", "C", "in"));
    doc.edges.push_back(MakeEdge("e3", "B", "out", "D", "in1"));
    doc.edges.push_back(MakeEdge("e4", "C", "out", "D", "in2"));

    GraphCompiler compiler;
    auto          order = compiler.ComputeExecutionOrder(doc);

    ASSERT_EQ(order.size(), 4u);
    EXPECT_EQ(order.front(), "A");
    EXPECT_EQ(order.back(), "D");
    // B and C must appear between A and D (any order)
    auto b_pos = std::find(order.begin(), order.end(), "B");
    auto c_pos = std::find(order.begin(), order.end(), "C");
    EXPECT_NE(b_pos, order.end());
    EXPECT_NE(c_pos, order.end());
}

TEST(ToposortTest, SingleNode)
{
    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("node1", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));

    GraphCompiler compiler;
    auto          order = compiler.ComputeExecutionOrder(doc);

    ASSERT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], "node1");
}

TEST(ToposortTest, DisconnectedGraph)
{
    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("A", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("B", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.nodes.push_back(
        MakeComponentNode("C", "CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC"));
    // No edges — all nodes are disconnected

    GraphCompiler compiler;
    auto          order = compiler.ComputeExecutionOrder(doc);

    ASSERT_EQ(order.size(), 3u);
    // All three nodes must be present (order doesn't matter)
    EXPECT_NE(std::find(order.begin(), order.end(), "A"), order.end());
    EXPECT_NE(std::find(order.begin(), order.end(), "B"), order.end());
    EXPECT_NE(std::find(order.begin(), order.end(), "C"), order.end());
}

TEST(ToposortTest, CycleDetection)
{
    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("A", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("B", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.nodes.push_back(
        MakeComponentNode("C", "CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC"));
    doc.edges.push_back(MakeEdge("e1", "A", "out", "B", "in"));
    doc.edges.push_back(MakeEdge("e2", "B", "out", "C", "in"));
    doc.edges.push_back(MakeEdge("e3", "C", "out", "A", "in"));

    GraphCompiler compiler;
    auto          order = compiler.ComputeExecutionOrder(doc);

    EXPECT_TRUE(order.empty());
}

TEST(ToposortTest, SelfLoopCycle)
{
    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("A", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.edges.push_back(MakeEdge("e1", "A", "out", "A", "in"));

    GraphCompiler compiler;
    auto          order = compiler.ComputeExecutionOrder(doc);

    EXPECT_TRUE(order.empty());
}

// ===================================================================
// Test Suite 6: PortBindingPlanTest
// ===================================================================

TEST(PortBindingPlanTest, SingleEdgeBinding)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "image"}});
    auto               node_b_def = MakeDefinition({{"in1", "image"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_a_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_b_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("A", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("B", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.edges.push_back(MakeEdge("e1", "A", "out1", "B", "in1"));

    auto plan = compiler.GeneratePortBindingPlan(doc);
    ASSERT_EQ(plan.bindings.size(), 1u);
    EXPECT_EQ(plan.bindings[0].source_node_id, "A");
    EXPECT_EQ(plan.bindings[0].source_port_id, "out1");
    EXPECT_EQ(plan.bindings[0].target_node_id, "B");
    EXPECT_EQ(plan.bindings[0].target_port_id, "in1");
    EXPECT_EQ(plan.bindings[0].expected_type, "image");
}

TEST(PortBindingPlanTest, MultiEdgeBindings)
{
    StubFactoryManager mgr;
    auto node_def = MakeDefinition({{"in1", "int"}}, {{"out1", "int"}});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_def);
    mgr.AddDefinition("CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC", node_def);
    mgr.AddDefinition("DDDDDDDD-DDDD-DDDD-DDDD-DDDDDDDDDDDD", node_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("A", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("B", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.nodes.push_back(
        MakeComponentNode("C", "CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC"));
    doc.nodes.push_back(
        MakeComponentNode("D", "DDDDDDDD-DDDD-DDDD-DDDD-DDDDDDDDDDDD"));
    doc.edges.push_back(MakeEdge("e1", "A", "out1", "B", "in1"));
    doc.edges.push_back(MakeEdge("e2", "A", "out1", "C", "in1"));
    doc.edges.push_back(MakeEdge("e3", "B", "out1", "D", "in1"));
    doc.edges.push_back(MakeEdge("e4", "C", "out1", "D", "in1"));

    auto plan = compiler.GeneratePortBindingPlan(doc);
    EXPECT_EQ(plan.bindings.size(), 4u);
}

TEST(PortBindingPlanTest, ExpectedTypeFromSource)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "image"}});
    auto               node_b_def = MakeDefinition({{"in1", "image"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_a_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_b_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("A", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("B", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.edges.push_back(MakeEdge("e1", "A", "out1", "B", "in1"));

    auto plan = compiler.GeneratePortBindingPlan(doc);
    ASSERT_EQ(plan.bindings.size(), 1u);
    // expected_type must come from source port type
    EXPECT_EQ(plan.bindings[0].expected_type, "image");
}

TEST(PortBindingPlanTest, NoEdgesGraph)
{
    StubFactoryManager mgr;
    auto node_def = MakeDefinition({{"in1", "int"}}, {{"out1", "int"}});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("A", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));

    auto plan = compiler.GeneratePortBindingPlan(doc);
    EXPECT_TRUE(plan.bindings.empty());
}

// ===================================================================
// Test Suite 7: CyclicEntryRefTest
// ===================================================================

TEST(CyclicEntryRefTest, NoCycle)
{
    GraphDocumentDto doc;
    doc.document_id = "doc_001";
    doc.nodes.push_back(MakeEntryRefNode("sub1", 42));

    GraphCompiler compiler;
    auto          diagnostics = compiler.DetectCyclicEntryRefs(doc);
    EXPECT_TRUE(diagnostics.empty());
}

TEST(CyclicEntryRefTest, SelfReferenceCycle)
{
    GraphDocumentDto doc;
    doc.document_id = "doc_001";
    doc.fingerprint = "fp_abc123";
    auto node = MakeEntryRefNode("sub1", 42);
    // Self-reference: the entryRef's source_fingerprint matches the
    // current document's fingerprint
    node.target.entry_ref->source_fingerprint = doc.fingerprint;
    doc.nodes.push_back(node);

    GraphCompiler compiler;
    auto          diagnostics = compiler.DetectCyclicEntryRefs(doc);
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_EQ(diagnostics[0].kind, CompileDiagnosticKind::CyclicEntryRef);
}

TEST(CyclicEntryRefTest, MultipleEntryRefsNoCycle)
{
    GraphDocumentDto doc;
    doc.document_id = "doc_001";
    doc.nodes.push_back(MakeEntryRefNode("sub1", 100));
    doc.nodes.push_back(MakeEntryRefNode("sub2", 200));

    GraphCompiler compiler;
    auto          diagnostics = compiler.DetectCyclicEntryRefs(doc);
    EXPECT_TRUE(diagnostics.empty());
}

// ===================================================================
// Test Suite 8: CompileOrchestrationTest
// ===================================================================

TEST(CompileOrchestrationTest, CompileSuccessPath)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "int"}});
    auto               node_b_def = MakeDefinition({{"in1", "int"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_a_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_b_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.document_id = "test_doc";
    doc.version = 1;
    doc.fingerprint = "abc123";
    doc.nodes.push_back(
        MakeComponentNode("A", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("B", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.edges.push_back(MakeEdge("e1", "A", "out1", "B", "in1"));

    auto plan = compiler.Compile(doc);
    EXPECT_FALSE(plan.execution_order.empty());
    EXPECT_FALSE(plan.binding_plan.bindings.empty());
    EXPECT_EQ(plan.document_id, "test_doc");
    EXPECT_EQ(plan.source_revision, 1);
    EXPECT_EQ(plan.source_fingerprint, "abc123");
}

TEST(CompileOrchestrationTest, CompileWithErrors)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "image"}});
    auto               node_b_def = MakeDefinition({{"in1", "string"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_a_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_b_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.document_id = "test_doc";
    doc.version = 1;
    doc.fingerprint = "abc123";
    doc.nodes.push_back(
        MakeComponentNode("A", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("B", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.edges.push_back(MakeEdge("e1", "A", "out1", "B", "in1"));

    auto plan = compiler.Compile(doc);
    // Best-effort: still produces execution_order and binding_plan
    EXPECT_FALSE(plan.execution_order.empty());
    EXPECT_FALSE(plan.binding_plan.bindings.empty());
    // Diagnostics contain the type mismatch
    EXPECT_FALSE(plan.diagnostics.empty());
}

TEST(CompileOrchestrationTest, CompiledFingerprint)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "int"}});
    auto               node_b_def = MakeDefinition({{"in1", "int"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_a_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_b_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.document_id = "test_doc";
    doc.version = 1;
    doc.fingerprint = "abc123";
    doc.nodes.push_back(
        MakeComponentNode("A", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("B", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.edges.push_back(MakeEdge("e1", "A", "out1", "B", "in1"));

    auto plan = compiler.Compile(doc);
    EXPECT_FALSE(plan.compiled_fingerprint.empty());
    EXPECT_NE(plan.compiled_fingerprint, plan.source_fingerprint);
}

// ===================================================================
// Test Suite 9: SignalAwareCompileTest (DAS-60 Stage 2)
// ===================================================================

TEST(SignalAwareCompileTest, SignalChainProducesDeterministicOrder)
{
    StubFactoryManager mgr;
    auto node_def = MakeDefinition({{"in", "signal"}}, {{"out", "signal"}});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_def);
    mgr.AddDefinition("CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC", node_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("A", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("B", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.nodes.push_back(
        MakeComponentNode("C", "CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC"));
    doc.edges.push_back(MakeSignalEdge("e1", "A", "out", "B", "in"));
    doc.edges.push_back(MakeSignalEdge("e2", "B", "out", "C", "in"));

    auto topo = compiler.ComputeTopology(doc);
    ASSERT_EQ(topo.execution_order.size(), 3u);
    EXPECT_EQ(topo.execution_order[0], "A");
    EXPECT_EQ(topo.execution_order[1], "B");
    EXPECT_EQ(topo.execution_order[2], "C");
    ASSERT_EQ(topo.signal_routes.size(), 2u);
    EXPECT_EQ(topo.signal_routes[0].source_port_id, "out");
    EXPECT_EQ(topo.signal_routes[0].target_node_id, "B");
    EXPECT_EQ(topo.signal_routes[1].target_node_id, "C");
    EXPECT_TRUE(topo.back_edges.empty());
    EXPECT_FALSE(topo.has_data_cycle);
}

TEST(SignalAwareCompileTest, BranchSignalRoutesCaptureBothBranches)
{
    StubFactoryManager mgr;
    auto               branch_def =
        MakeDefinition({}, {{"true", "signal"}, {"false", "signal"}});
    auto sink_def = MakeDefinition({{"in", "signal"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", branch_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", sink_def);
    mgr.AddDefinition("CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC", sink_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("branch", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(MakeComponentNode(
        "trueBranch",
        "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.nodes.push_back(MakeComponentNode(
        "falseBranch",
        "CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC"));
    doc.edges.push_back(
        MakeSignalEdge("e1", "branch", "true", "trueBranch", "in"));
    doc.edges.push_back(
        MakeSignalEdge("e2", "branch", "false", "falseBranch", "in"));

    auto topo = compiler.ComputeTopology(doc);
    // branch ordered before both branches (deterministic, document order)
    ASSERT_EQ(topo.execution_order.size(), 3u);
    EXPECT_EQ(topo.execution_order.front(), "branch");
    EXPECT_EQ(topo.execution_order[1], "trueBranch");
    EXPECT_EQ(topo.execution_order[2], "falseBranch");

    // Each branch edge becomes a route keyed by its source signal port.
    ASSERT_EQ(topo.signal_routes.size(), 2u);
    EXPECT_EQ(topo.signal_routes[0].source_port_id, "true");
    EXPECT_EQ(topo.signal_routes[0].target_node_id, "trueBranch");
    EXPECT_EQ(topo.signal_routes[1].source_port_id, "false");
    EXPECT_EQ(topo.signal_routes[1].target_node_id, "falseBranch");
    for (const auto& route : topo.signal_routes)
    {
        auto cond = route.activation_condition.as_object();
        ASSERT_TRUE(cond.has_value());
        EXPECT_TRUE(cond->contains(std::string_view("signal")));
    }
    EXPECT_TRUE(topo.back_edges.empty());
}

TEST(SignalAwareCompileTest, ForWhileBackEdgeCompilesWithoutCycle)
{
    StubFactoryManager mgr;
    auto               loop_head_def =
        MakeDefinition({{"loop_in", "signal"}}, {{"continue", "signal"}});
    auto body_def = MakeDefinition({{"in", "signal"}}, {{"done", "signal"}});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", loop_head_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", body_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("for", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("body", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    // forward: for.continue -> body.in ; back-edge: body.done -> for.loop_in
    doc.edges.push_back(MakeSignalEdge("e1", "for", "continue", "body", "in"));
    doc.edges.push_back(MakeSignalEdge("e2", "body", "done", "for", "loop_in"));

    auto topo = compiler.ComputeTopology(doc);
    // Back-edge tolerated: both nodes still ordered, no data cycle.
    ASSERT_EQ(topo.execution_order.size(), 2u);
    EXPECT_EQ(topo.execution_order[0], "for");
    EXPECT_EQ(topo.execution_order[1], "body");
    EXPECT_FALSE(topo.has_data_cycle);

    // The back-edge is recorded with the loop head as its target.
    ASSERT_EQ(topo.back_edges.size(), 1u);
    EXPECT_EQ(topo.back_edges[0].edge_id, "e2");
    EXPECT_EQ(topo.back_edges[0].source_node_id, "body");
    EXPECT_EQ(topo.back_edges[0].target_node_id, "for");
    EXPECT_EQ(topo.back_edges[0].loop_head_node_id, "for");

    // Back-edge still carries a signal route (the loop-back signal).
    ASSERT_EQ(topo.signal_routes.size(), 2u);
}

TEST(SignalAwareCompileTest, DataCycleStillRejectedAsCyclicEdgeGraph)
{
    StubFactoryManager mgr;
    auto node_def = MakeDefinition({{"in", "int"}}, {{"out", "int"}});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_def);
    mgr.AddDefinition("CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC", node_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("A", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("B", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.nodes.push_back(
        MakeComponentNode("C", "CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC"));
    // All data edges (default edge_type) forming a cycle.
    doc.edges.push_back(MakeEdge("e1", "A", "out", "B", "in"));
    doc.edges.push_back(MakeEdge("e2", "B", "out", "C", "in"));
    doc.edges.push_back(MakeEdge("e3", "C", "out", "A", "in"));

    auto plan = compiler.Compile(doc);
    EXPECT_TRUE(plan.execution_order.empty());
    EXPECT_TRUE(HasCyclicEdgeGraphDiagnostic(plan));
    EXPECT_TRUE(plan.back_edges.empty());
}

TEST(SignalAwareCompileTest, GraphInputsBroadcastBoundToMatchingPorts)
{
    StubFactoryManager mgr;
    // Node declares an input whose port_id matches a graph_input ("threshold")
    // and one that does not ("private").
    auto node_def =
        MakeDefinition({{"threshold", "int"}, {"private", "int"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("N", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.graph_inputs.push_back(MakeGraphInput("threshold", "int", "42"));

    auto plan = compiler.GeneratePortBindingPlan(doc);
    // Only the matching input port receives a broadcast binding.
    ASSERT_EQ(plan.bindings.size(), 1u);
    const auto& binding = plan.bindings[0];
    EXPECT_EQ(binding.source_node_id, "$graph_input");
    EXPECT_EQ(binding.source_port_id, "threshold");
    EXPECT_EQ(binding.target_node_id, "N");
    EXPECT_EQ(binding.target_port_id, "threshold");
    EXPECT_EQ(binding.expected_type, "int");
    EXPECT_EQ(binding.default_value.write(), std::string("42"));
}

TEST(SignalAwareCompileTest, GraphInputsBroadcastToOneToManyNodes)
{
    StubFactoryManager mgr;
    auto               node_def = MakeDefinition({{"threshold", "int"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("N1", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("N2", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.graph_inputs.push_back(MakeGraphInput("threshold", "int", "7"));

    auto plan = compiler.GeneratePortBindingPlan(doc);
    // One graph input broadcast to two nodes (one-to-many).
    ASSERT_EQ(plan.bindings.size(), 2u);
    EXPECT_EQ(plan.bindings[0].target_node_id, "N1");
    EXPECT_EQ(plan.bindings[1].target_node_id, "N2");
    for (const auto& binding : plan.bindings)
    {
        EXPECT_EQ(binding.source_node_id, "$graph_input");
        EXPECT_EQ(binding.default_value.write(), std::string("7"));
    }
}
