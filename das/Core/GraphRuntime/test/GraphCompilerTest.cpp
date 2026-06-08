#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>
#include <das/Core/GraphRuntime/GraphCompiler.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

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
