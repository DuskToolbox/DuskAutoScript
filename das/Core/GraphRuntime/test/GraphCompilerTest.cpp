#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>
#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/GraphCompiler.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

    GraphPortDefinitionDto MakePortDefinition(
        const std::string& port_id,
        const std::string& port_type,
        const std::string& default_json = {})
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

    // 会话 catalog 注入通道替身：component_guid → 声明端口面（无 factory
    // manager 的生产 authoring 编译路径形态）。
    GraphCompiler::ComponentPortSurfaceProvider MakePortSurfaceProvider(
        const std::unordered_map<
            std::string,
            std::pair<
                std::vector<std::pair<std::string, std::string>>,
                std::vector<std::pair<std::string, std::string>>>>& surfaces)
    {
        return [&surfaces](const std::string& component_guid)
                   -> std::optional<GraphCompiler::ComponentPortSurface>
        {
            auto it = surfaces.find(component_guid);
            if (it == surfaces.end())
            {
                return std::nullopt;
            }
            GraphCompiler::ComponentPortSurface surface;
            for (const auto& [port_id, port_type] : it->second.first)
            {
                surface.inputs.push_back(
                    MakePortDefinition(port_id, port_type));
            }
            for (const auto& [port_id, port_type] : it->second.second)
            {
                surface.outputs.push_back(
                    MakePortDefinition(port_id, port_type));
            }
            return surface;
        };
    }

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

// 注入端口面（无 factory manager）时边校验照常出诊断：authoring 会话编译
// 路径只绑定 provider，提前返回会静默吞掉全部边诊断。
TEST(EdgePortExistenceTest, ProviderSurfaceValidatesEdgesWithoutFactoryManager)
{
    std::unordered_map<
        std::string,
        std::pair<
            std::vector<std::pair<std::string, std::string>>,
            std::vector<std::pair<std::string, std::string>>>>
        surfaces;
    surfaces["AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"] = {{}, {{"out1", "int"}}};
    surfaces["BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"] = {{{"in1", "int"}}, {}};

    GraphCompiler compiler;
    compiler.SetPortSurfaceProvider(MakePortSurfaceProvider(surfaces));

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
    // 四条点对点边 + 一条推导 broadcast（A.in1 悬空——B/C/D 的 in1 均有
    // 入边，只有链头的 A 缺值来源）。
    ASSERT_EQ(plan.bindings.size(), 5u);
    const PortBindingDto* broadcast = nullptr;
    for (const auto& binding : plan.bindings)
    {
        if (binding.source_node_id == "$graph_input")
        {
            broadcast = &binding;
        }
    }
    ASSERT_NE(broadcast, nullptr);
    EXPECT_EQ(broadcast->source_port_id, "in1");
    EXPECT_EQ(broadcast->target_node_id, "A");
    EXPECT_EQ(broadcast->target_port_id, "in1");
    EXPECT_EQ(broadcast->expected_type, "int");
    EXPECT_TRUE(broadcast->default_value.is_null());
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
    // 无边图：唯一的绑定是悬空声明输入 in1 的 $graph_input broadcast
    // （声明通道不携带默认值 → default 通道 null，运行时按口未设处理）。
    ASSERT_EQ(plan.bindings.size(), 1u);
    EXPECT_EQ(plan.bindings[0].source_node_id, "$graph_input");
    EXPECT_EQ(plan.bindings[0].source_port_id, "in1");
    EXPECT_EQ(plan.bindings[0].target_node_id, "A");
    EXPECT_EQ(plan.bindings[0].target_port_id, "in1");
    EXPECT_EQ(plan.bindings[0].expected_type, "int");
    EXPECT_TRUE(plan.bindings[0].default_value.is_null());
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

TEST(SignalAwareCompileTest, DanglingInputsBroadcastBoundToMatchingPorts)
{
    StubFactoryManager mgr;
    // 节点声明两个输入口，均无入边——都推导为边界入参（$graph_input
    // 绑定；声明通道不携带默认值 → default 通道 null，运行时按口未设
    // 处理）。
    auto node_def =
        MakeDefinition({{"threshold", "int"}, {"private", "int"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("N", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));

    auto plan = compiler.GeneratePortBindingPlan(doc);
    ASSERT_EQ(plan.bindings.size(), 2u);
    const PortBindingDto* threshold_binding = nullptr;
    const PortBindingDto* private_binding = nullptr;
    for (const auto& binding : plan.bindings)
    {
        if (binding.target_port_id == "threshold")
        {
            threshold_binding = &binding;
        }
        if (binding.target_port_id == "private")
        {
            private_binding = &binding;
        }
    }
    ASSERT_NE(threshold_binding, nullptr);
    EXPECT_EQ(threshold_binding->source_node_id, "$graph_input");
    EXPECT_EQ(threshold_binding->source_port_id, "threshold");
    EXPECT_EQ(threshold_binding->target_node_id, "N");
    EXPECT_EQ(threshold_binding->target_port_id, "threshold");
    EXPECT_EQ(threshold_binding->expected_type, "int");
    EXPECT_TRUE(threshold_binding->default_value.is_null());
    ASSERT_NE(private_binding, nullptr);
    EXPECT_EQ(private_binding->source_node_id, "$graph_input");
    EXPECT_TRUE(private_binding->default_value.is_null());
}

TEST(SignalAwareCompileTest, DanglingInputsBroadcastToOneToManyNodes)
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

    auto plan = compiler.GeneratePortBindingPlan(doc);
    // 同名悬空输入广播到两个节点（one-to-many by-name 合并）。
    ASSERT_EQ(plan.bindings.size(), 2u);
    EXPECT_EQ(plan.bindings[0].target_node_id, "N1");
    EXPECT_EQ(plan.bindings[1].target_node_id, "N2");
    for (const auto& binding : plan.bindings)
    {
        EXPECT_EQ(binding.source_node_id, "$graph_input");
        EXPECT_EQ(binding.source_port_id, "threshold");
        EXPECT_TRUE(binding.default_value.is_null());
    }
}

// A fromsequence authoring doc is a linear signal-chain graph tagged with
// "fromsequence". The tag is a pure authoring/UI hint — the compiler must
// produce identical topology with or without it (compiler ignores tags;
// execution_order == chain order == node array order).
TEST(SignalAwareCompileTest, FromSequenceLinearChainCompilesInArrayOrder)
{
    StubFactoryManager mgr;
    auto task_def = MakeDefinition({{"in", "signal"}}, {{"out", "signal"}});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", task_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", task_def);
    mgr.AddDefinition("CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC", task_def);

    auto build_doc = [&]()
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(
            MakeComponentNode("t1", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
        doc.nodes.push_back(
            MakeComponentNode("t2", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
        doc.nodes.push_back(
            MakeComponentNode("t3", "CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC"));
        doc.edges.push_back(MakeSignalEdge("e1", "t1", "out", "t2", "in"));
        doc.edges.push_back(MakeSignalEdge("e2", "t2", "out", "t3", "in"));
        return doc;
    };

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    auto plan_plain = compiler.Compile(build_doc());

    auto doc_tagged = build_doc();
    doc_tagged.tags = {"fromsequence"};
    auto plan_tagged = compiler.Compile(doc_tagged);

    // Linear chain → array order, regardless of the tag.
    ASSERT_EQ(plan_plain.execution_order.size(), 3u);
    EXPECT_EQ(plan_plain.execution_order[0], "t1");
    EXPECT_EQ(plan_plain.execution_order[1], "t2");
    EXPECT_EQ(plan_plain.execution_order[2], "t3");
    EXPECT_EQ(plan_plain.execution_order, plan_tagged.execution_order);

    // Signal routes mirror the chain; no loops, no cycle diagnostic.
    EXPECT_EQ(plan_tagged.signal_routes.size(), 2u);
    EXPECT_TRUE(plan_tagged.back_edges.empty());
    EXPECT_FALSE(HasCyclicEdgeGraphDiagnostic(plan_tagged));
}

// ===================================================================
// Test Suite: Unknown target kinds compile to an empty component_guid —
// the runtime's Configure step rejects them (fail-closed).
// ===================================================================

TEST(CompileOrchestrationTest, CompileLeavesUnknownTargetKindGuidEmpty)
{
    GraphCompiler    compiler;
    GraphDocumentDto doc;
    GraphNodeDto     node;
    node.node_id = "unknown";
    node.target.target_kind = "entryRef";
    doc.nodes.push_back(std::move(node));

    auto plan = compiler.Compile(doc);
    ASSERT_EQ(plan.node_snapshots.size(), 1u);
    EXPECT_TRUE(plan.node_snapshots[0].component_guid.empty());
    EXPECT_TRUE(plan.node_snapshots[0].compiled_payload_json.is_null());
}

// ===================================================================
// Test Suite: RequiredFieldValidationTest
// 编译期 required 空值校验：会话层从 catalog 注入 required 清单
// （component_guid → 字段名集），Compile 对空值产出
// RequiredFieldMissing diagnostic，有值放行。
// ===================================================================

namespace
{
    using RequiredFieldMap =
        std::unordered_map<std::string, std::unordered_set<std::string>>;

    RequiredFieldMap MakeLaunchRequiredMap()
    {
        RequiredFieldMap required_fields;
        required_fields["95FD2FFA-88F2-4F85-A536-C708D25B641F"].insert(
            "exePath");
        return required_fields;
    }

    void SetNodeSettings(GraphNodeDto& node, std::string_view settings_json)
    {
        auto parsed =
            Das::Utils::ParseYyjsonFromString(std::string(settings_json));
        ASSERT_TRUE(parsed.has_value());
        node.settings = std::move(*parsed);
    }

    // RequiredFieldMissing diagnostic kind serialised as its enum int string.
    std::size_t CountRequiredFieldMissing(
        const Dto::CompiledGraphPlanDto& plan,
        std::string_view                 node_id = {})
    {
        const std::string wanted_kind = std::to_string(
            static_cast<int>(CompileDiagnosticKind::RequiredFieldMissing));
        std::size_t count = 0;
        for (const auto& diag : plan.diagnostics)
        {
            auto obj = diag.as_object();
            if (!obj.has_value())
            {
                continue;
            }
            auto kind = (*obj)[std::string_view("kind")].as_string();
            if (!kind.has_value() || *kind != wanted_kind)
            {
                continue;
            }
            if (!node_id.empty())
            {
                auto diag_node =
                    (*obj)[std::string_view("node_id")].as_string();
                if (!diag_node.has_value() || *diag_node != node_id)
                {
                    continue;
                }
            }
            ++count;
        }
        return count;
    }
} // namespace

TEST(RequiredFieldValidationTest, MissingRequiredFieldProducesDiagnostic)
{
    GraphCompiler compiler;

    GraphDocumentDto doc;
    doc.document_id = "required_missing";
    // settings 整体缺失（fresh addGraphNode 形态）。
    auto no_settings =
        MakeComponentNode("n_1", "95FD2FFA-88F2-4F85-A536-C708D25B641F");
    // settings 存在但键缺失。
    auto key_missing =
        MakeComponentNode("n_2", "95FD2FFA-88F2-4F85-A536-C708D25B641F");
    SetNodeSettings(key_missing, R"({"args":[]})");
    // 空串与纯空白串（与运行时 exe_path.empty() 判定对齐）。
    auto empty_string =
        MakeComponentNode("n_3", "95FD2FFA-88F2-4F85-A536-C708D25B641F");
    SetNodeSettings(empty_string, R"({"exePath":""})");
    auto whitespace_string =
        MakeComponentNode("n_4", "95FD2FFA-88F2-4F85-A536-C708D25B641F");
    SetNodeSettings(whitespace_string, R"({"exePath":"   "})");
    // null 值与"键缺失或 null 判空"的数值字段（非字符串键存在即满足）。
    auto null_value =
        MakeComponentNode("n_5", "95FD2FFA-88F2-4F85-A536-C708D25B641F");
    SetNodeSettings(null_value, R"({"exePath":null})");
    doc.nodes.push_back(std::move(no_settings));
    doc.nodes.push_back(std::move(key_missing));
    doc.nodes.push_back(std::move(empty_string));
    doc.nodes.push_back(std::move(whitespace_string));
    doc.nodes.push_back(std::move(null_value));

    auto plan = compiler.Compile(doc, MakeLaunchRequiredMap());
    EXPECT_EQ(CountRequiredFieldMissing(plan), 5u);
    for (std::string_view node_id : {"n_1", "n_2", "n_3", "n_4", "n_5"})
    {
        EXPECT_EQ(CountRequiredFieldMissing(plan, node_id), 1u)
            << "missing RequiredFieldMissing diagnostic for " << node_id;
    }
}

TEST(RequiredFieldValidationTest, SatisfiedRequiredFieldEmitsNoDiagnostic)
{
    GraphCompiler compiler;

    GraphDocumentDto doc;
    doc.document_id = "required_satisfied";
    auto configured =
        MakeComponentNode("n_1", "95FD2FFA-88F2-4F85-A536-C708D25B641F");
    SetNodeSettings(configured, R"({"exePath":"C:\\tool\\app.exe"})");
    // 组件不在 required 清单内：零诊断（含 settings 为空的节点）。
    auto other_component =
        MakeComponentNode("n_2", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA");
    doc.nodes.push_back(std::move(configured));
    doc.nodes.push_back(std::move(other_component));

    auto plan = compiler.Compile(doc, MakeLaunchRequiredMap());
    EXPECT_EQ(CountRequiredFieldMissing(plan), 0u);
    // 无清单注入（现有调用方形态）零诊断。
    auto plan_without_map = compiler.Compile(doc);
    EXPECT_EQ(CountRequiredFieldMissing(plan_without_map), 0u);
}

// ===================================================================
// Test Suite: BoundaryPortDerivationTest
// 边界端口推导（端口全自动推导，只吃声明面+边角色）：输入侧悬空判定
// （无入边的声明输入口→边界入参，最小集——值需有来源）；输出侧声明面
// 输出口全量导出（内部消费与对外导出不互斥，扇出合法）；同名口两侧
// by-name 合并。
// ===================================================================

TEST(BoundaryPortDerivationTest, DanglingPortsDeriveAsBoundaryPorts)
{
    StubFactoryManager mgr;
    auto definition_a = MakeDefinition({{"p1", "int"}}, {{"feed", "int"}});
    auto definition_b = MakeDefinition({{"eat", "int"}}, {{"p2", "string"}});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", definition_a);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", definition_b);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("n_1", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("n_2", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.edges.push_back(MakeEdge("e1", "n_1", "feed", "n_2", "eat"));

    auto boundary = compiler.DeriveBoundaryPorts(doc);

    // 输入侧悬空判定：eat 有入边（值有来源）非边界；p1 悬空 → 边界入参。
    ASSERT_EQ(boundary.inputs.size(), 1u);
    EXPECT_EQ(boundary.inputs[0].port_id, "p1");
    EXPECT_EQ(boundary.inputs[0].port_type, "int");
    ASSERT_EQ(boundary.inputs[0].node_ids.size(), 1u);
    EXPECT_EQ(boundary.inputs[0].node_ids[0], "n_1");
    EXPECT_TRUE(boundary.inputs[0].default_value.is_null());

    // 输出侧全量导出：feed 虽有出边（被内部消费）仍导出；p2 悬空同样
    // 导出——两条声明输出口都在边界清单里。
    ASSERT_EQ(boundary.outputs.size(), 2u);
    EXPECT_EQ(boundary.outputs[0].port_id, "feed");
    EXPECT_EQ(boundary.outputs[0].port_type, "int");
    ASSERT_EQ(boundary.outputs[0].node_ids.size(), 1u);
    EXPECT_EQ(boundary.outputs[0].node_ids[0], "n_1");
    EXPECT_EQ(boundary.outputs[1].port_id, "p2");
    EXPECT_EQ(boundary.outputs[1].port_type, "string");
    ASSERT_EQ(boundary.outputs[1].node_ids.size(), 1u);
    EXPECT_EQ(boundary.outputs[1].node_ids[0], "n_2");
}

TEST(BoundaryPortDerivationTest, SameNameDanglingInputsMergeByPortName)
{
    StubFactoryManager mgr;
    auto               definition = MakeDefinition({{"p1", "int"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", definition);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", definition);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("n_1", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("n_2", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));

    auto boundary = compiler.DeriveBoundaryPorts(doc);

    // 两个同名悬空输入口合并为一个边界口（by-name 广播语义），携带两个
    // 悬空位置。
    ASSERT_EQ(boundary.inputs.size(), 1u);
    EXPECT_EQ(boundary.inputs[0].port_id, "p1");
    ASSERT_EQ(boundary.inputs[0].node_ids.size(), 2u);
    EXPECT_EQ(boundary.inputs[0].node_ids[0], "n_1");
    EXPECT_EQ(boundary.inputs[0].node_ids[1], "n_2");
}

// manifest 声明端口面的悬空推导：未连线的声明输入口/输出口同样是边界口。
TEST(BoundaryPortDerivationTest, DeclaredManifestPortsDeriveWhenDangling)
{
    StubFactoryManager mgr;
    auto               definition =
        MakeDefinition({{"declared_in", "int"}}, {{"declared_out", "int"}});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", definition);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("n_1", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));

    auto boundary = compiler.DeriveBoundaryPorts(doc);

    ASSERT_EQ(boundary.inputs.size(), 1u);
    EXPECT_EQ(boundary.inputs[0].port_id, "declared_in");
    EXPECT_EQ(boundary.inputs[0].node_ids.size(), 1u);
    ASSERT_EQ(boundary.outputs.size(), 1u);
    EXPECT_EQ(boundary.outputs[0].port_id, "declared_out");
}

// ===================================================================
// 注入端口面的绑定类型解析：无 factory manager 时 expected_type 从会话
// catalog 注入的声明面解析（生产 authoring 编译路径形态）。
// ===================================================================

TEST(PortBindingPlanTest, ExpectedTypeResolvesFromInjectedPortSurface)
{
    std::unordered_map<
        std::string,
        std::pair<
            std::vector<std::pair<std::string, std::string>>,
            std::vector<std::pair<std::string, std::string>>>>
        surfaces;
    surfaces["AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"] = {
        {},
        {{"dyn_out", "image"}}};
    surfaces["BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"] = {{{"in1", "int"}}, {}};

    GraphCompiler compiler;
    compiler.SetPortSurfaceProvider(MakePortSurfaceProvider(surfaces));

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("A", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("B", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.edges.push_back(MakeEdge("e1", "A", "dyn_out", "B", "in1"));

    auto plan = compiler.GeneratePortBindingPlan(doc);
    ASSERT_EQ(plan.bindings.size(), 1u);
    EXPECT_EQ(plan.bindings[0].expected_type, "image");
}

// ===================================================================
// 推导悬空输入的 broadcast 集成：编译产物为悬空输入生成 $graph_input
// 哨兵绑定（default_value 取该口声明默认值——注入端口面通道可携带默认，
// manifest 通道不携带即为 null）；边界口清单写入编译产物。
// ===================================================================

TEST(SignalAwareCompileTest, DerivedDanglingInputsGenerateBroadcastBindings)
{
    GraphCompiler::ComponentPortSurface surface;
    surface.inputs.push_back(MakePortDefinition("p1", "int", "42"));
    surface.inputs.push_back(MakePortDefinition("declared_in", "int"));
    GraphCompiler compiler;
    compiler.SetPortSurfaceProvider(
        [&surface](const std::string& component_guid)
            -> std::optional<GraphCompiler::ComponentPortSurface>
        {
            if (component_guid != "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA")
            {
                return std::nullopt;
            }
            return surface;
        });

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("N", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));

    auto plan = compiler.Compile(doc);

    const PortBindingDto* p1_binding = nullptr;
    const PortBindingDto* declared_binding = nullptr;
    for (const auto& binding : plan.binding_plan.bindings)
    {
        if (binding.source_node_id != "$graph_input")
        {
            continue;
        }
        if (binding.target_port_id == "p1")
        {
            p1_binding = &binding;
        }
        if (binding.target_port_id == "declared_in")
        {
            declared_binding = &binding;
        }
    }
    ASSERT_NE(p1_binding, nullptr);
    EXPECT_EQ(p1_binding->source_port_id, "p1");
    EXPECT_EQ(p1_binding->target_node_id, "N");
    EXPECT_EQ(p1_binding->expected_type, "int");
    EXPECT_EQ(p1_binding->default_value.write(), std::string("42"));
    // 无声明默认的悬空输入同样推导（default 通道为 null，运行时按口未设）。
    ASSERT_NE(declared_binding, nullptr);
    EXPECT_TRUE(declared_binding->default_value.is_null());
}

TEST(CompileOrchestrationTest, CompileCarriesBoundaryPortsInPlan)
{
    StubFactoryManager mgr;
    auto               definition_a = MakeDefinition(
        {{"declared_in", "int"}, {"p1", "int"}},
        {{"p2", "string"}});
    auto definition_b = MakeDefinition({{"take", "string"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", definition_a);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", definition_b);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("n_1", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("n_2", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    // p2 被内部边消费（n_1.p2 → n_2.take）。
    doc.edges.push_back(MakeEdge("e1", "n_1", "p2", "n_2", "take"));

    auto plan = compiler.Compile(doc);

    // 悬空声明输入（declared_in/p1 均无入边）= 边界入参；take 有入边排除。
    ASSERT_EQ(plan.boundary_ports.inputs.size(), 2u);
    EXPECT_EQ(plan.boundary_ports.inputs[0].port_id, "declared_in");
    EXPECT_EQ(plan.boundary_ports.inputs[1].port_id, "p1");
    EXPECT_EQ(plan.boundary_ports.inputs[1].port_type, "int");
    ASSERT_EQ(plan.boundary_ports.inputs[1].node_ids.size(), 1u);
    EXPECT_EQ(plan.boundary_ports.inputs[1].node_ids[0], "n_1");

    // 输出侧全量导出：p2 虽被内部边消费仍进边界清单（执行侧导出依据）。
    ASSERT_EQ(plan.boundary_ports.outputs.size(), 1u);
    EXPECT_EQ(plan.boundary_ports.outputs[0].port_id, "p2");
    EXPECT_EQ(plan.boundary_ports.outputs[0].port_type, "string");
    EXPECT_EQ(plan.boundary_ports.outputs[0].node_ids[0], "n_1");
}
