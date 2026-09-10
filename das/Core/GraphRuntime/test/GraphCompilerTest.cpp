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

    // UnbackedDataCycle diagnostic kind serialised as a string of its enum
    // int; true when at least one such diagnostic carries the guidance
    // message text.
    bool HasUnbackedDataCycleDiagnostic(
        const Dto::CompiledGraphPlanDto& plan,
        const std::string&               message_fragment = {})
    {
        const std::string unbacked_kind = std::to_string(
            static_cast<int>(CompileDiagnosticKind::UnbackedDataCycle));
        for (const auto& diag : plan.diagnostics)
        {
            auto obj = diag.as_object();
            if (!obj.has_value())
            {
                continue;
            }
            auto kind = (*obj)[std::string_view("kind")].as_string();
            if (!kind.has_value() || *kind != unbacked_kind)
            {
                continue;
            }
            if (message_fragment.empty())
            {
                return true;
            }
            auto message = (*obj)[std::string_view("message")].as_string();
            if (message.has_value()
                && message->find(message_fragment) != std::string_view::npos)
            {
                return true;
            }
        }
        return false;
    }

    // LoopBodyDataLeak diagnostic kind serialised as a string of its enum
    // int; true when at least one such diagnostic exists (optionally scoped
    // to a node_id).
    bool HasLoopBodyDataLeakDiagnostic(
        const Dto::CompiledGraphPlanDto& plan,
        const std::string&               node_id = {})
    {
        const std::string leak_kind = std::to_string(
            static_cast<int>(CompileDiagnosticKind::LoopBodyDataLeak));
        for (const auto& diag : plan.diagnostics)
        {
            auto obj = diag.as_object();
            if (!obj.has_value())
            {
                continue;
            }
            auto kind = (*obj)[std::string_view("kind")].as_string();
            if (!kind.has_value() || *kind != leak_kind)
            {
                continue;
            }
            if (node_id.empty())
            {
                return true;
            }
            auto node = (*obj)[std::string_view("node_id")].as_string();
            if (node.has_value() && *node == node_id)
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

// ---------------------------------------------------------------------
// 数字等价类兼容矩阵（IsTypeCompatible 直测）——Core 权威判定与 UI 侧
// GraphConnectionRules.CanConnect 等价表是同一张表的两侧，改一侧必改
// 另一侧，漂移即红。
// ---------------------------------------------------------------------
TEST(PortTypeCompatibilityTest, NumericEquivalenceMatrix)
{
    struct MatrixEntry
    {
        const char* source_type;
        const char* target_type;
        bool        expected_compatible;
    };
    const MatrixEntry matrix[] = {
        // 同词
        {"int", "int", true},
        {"int64", "int64", true},
        {"float", "float", true},
        {"double", "double", true},
        {"bool", "bool", true},
        {"string", "string", true},
        {"object", "object", true},
        // 整数族内互连
        {"int", "int64", true},
        {"int64", "int", true},
        // 浮点族内互连（double→float 收窄按裁决放行：运行期值恒以
        // double 承载，族内换词不构成语义截断）
        {"float", "double", true},
        {"double", "float", true},
        // 整数 → 浮点：无损宽化放行
        {"int", "double", true},
        {"int", "float", true},
        {"int64", "double", true},
        {"int64", "float", true},
        // 浮点 → 整数：精度截断不静默做，拒绝
        {"double", "int", false},
        {"double", "int64", false},
        {"float", "int", false},
        {"float", "int64", false},
        // bool/string/object 与数字族互拒（fail-closed，bool↔number 坑源）
        {"bool", "int", false},
        {"int", "bool", false},
        {"bool", "double", false},
        {"double", "bool", false},
        {"string", "int", false},
        {"int", "string", false},
        {"object", "int", false},
        {"int", "object", false}};

    for (const auto& entry : matrix)
    {
        SCOPED_TRACE(
            std::string(entry.source_type) + " -> " + entry.target_type);
        EXPECT_EQ(
            GraphCompiler::IsTypeCompatible(
                entry.source_type,
                entry.target_type),
            entry.expected_compatible);
    }
}

// ---------------------------------------------------------------------
// 边校验间接测矩阵方向性：整数→浮点连线零诊断；浮点→整数连线
// TypeMismatch（expected=目标声明类型，actual=源类型）。
// ---------------------------------------------------------------------
TEST(PortTypeCompatibilityTest, IntToDoubleEdgePassesTypeCheck)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "int"}});
    auto               node_b_def = MakeDefinition({{"in1", "double"}}, {});
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

TEST(PortTypeCompatibilityTest, DoubleToIntEdgeRejectedAsTypeMismatch)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "double"}});
    auto               node_b_def = MakeDefinition({{"in1", "int64"}}, {});
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
            EXPECT_EQ(d.expected_type, "int64");
            EXPECT_EQ(d.actual_type, "double");
            break;
        }
    }
    EXPECT_TRUE(found_type_mismatch);
}

TEST(PortTypeCompatibilityTest, BoolNumberEdgeRejectedAsTypeMismatch)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "int"}});
    auto               node_b_def = MakeDefinition({{"in1", "bool"}}, {});
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
            break;
        }
    }
    EXPECT_TRUE(found_type_mismatch);
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

// 数据环在排序层被容留（回边剔除出 Kahn 后仍可排序），拒绝发生在
// Compile() 的承载检查（UnbackedDataCycle + 清空 execution_order）——
// 「排序层直接空」的旧语义已随数据回边合法化退役。
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

    EXPECT_FALSE(order.empty());

    auto plan = compiler.Compile(doc);
    EXPECT_TRUE(plan.execution_order.empty());
    EXPECT_TRUE(HasUnbackedDataCycleDiagnostic(plan));
}

TEST(ToposortTest, SelfLoopCycle)
{
    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("A", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.edges.push_back(MakeEdge("e1", "A", "out", "A", "in"));

    GraphCompiler compiler;
    auto          order = compiler.ComputeExecutionOrder(doc);

    EXPECT_FALSE(order.empty());

    auto plan = compiler.Compile(doc);
    EXPECT_TRUE(plan.execution_order.empty());
    EXPECT_TRUE(HasUnbackedDataCycleDiagnostic(plan));
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
    // broadcast 绑定的目标口类型来自边界推导的声明类型（与源侧同源）。
    EXPECT_EQ(broadcast->target_type, "int");
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

// ---------------------------------------------------------------------
// target_type 记录：binding 同时携带两端类型知识——expected_type 来自
// 源口声明、target_type 来自目标输入口声明。两端类型不同（整数→浮点）
// 时字段各自独立，供读值侧物化消费。
// ---------------------------------------------------------------------
TEST(PortBindingPlanTest, TargetTypeRecordedFromTargetManifestInputs)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "int"}});
    auto               node_b_def = MakeDefinition({{"in1", "double"}}, {});
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
    EXPECT_EQ(plan.bindings[0].expected_type, "int");
    EXPECT_EQ(plan.bindings[0].target_type, "double");
}

// 目标口不在 manifest inputs：解析不到留空，binding plan 不发明类型。
TEST(PortBindingPlanTest, TargetTypeEmptyWhenTargetPortUnresolvable)
{
    StubFactoryManager mgr;
    auto               node_a_def = MakeDefinition({}, {{"out1", "int"}});
    auto               node_b_def = MakeDefinition({{"in1", "double"}}, {});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_a_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_b_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("A", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("B", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    doc.edges.push_back(MakeEdge("e1", "A", "out1", "B", "in_unknown"));

    auto plan = compiler.GeneratePortBindingPlan(doc);
    // 边绑定 + B.in1 悬空推导的 broadcast 绑定（in_unknown 占走了入边）。
    ASSERT_EQ(plan.bindings.size(), 2u);
    const PortBindingDto* edge_binding = nullptr;
    for (const auto& binding : plan.bindings)
    {
        if (binding.target_port_id == "in_unknown")
        {
            edge_binding = &binding;
        }
    }
    ASSERT_NE(edge_binding, nullptr);
    EXPECT_EQ(edge_binding->expected_type, "int");
    EXPECT_EQ(edge_binding->target_type, "");
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
    EXPECT_TRUE(topo.loop_layers.empty());
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

    // The back-edge is recorded with the loop head as its target.
    ASSERT_EQ(topo.back_edges.size(), 1u);
    EXPECT_EQ(topo.back_edges[0].edge_id, "e2");
    EXPECT_EQ(topo.back_edges[0].source_node_id, "body");
    EXPECT_EQ(topo.back_edges[0].target_node_id, "for");
    EXPECT_EQ(topo.back_edges[0].loop_head_node_id, "for");
    EXPECT_EQ(topo.back_edges[0].edge_kind, "signal");
    EXPECT_EQ(topo.back_edges[0].loop_id, "loop::for");

    // The cycle layer is recorded for the runtime's iteration reset set.
    ASSERT_EQ(topo.loop_layers.size(), 1u);
    EXPECT_EQ(topo.loop_layers[0].head_node_id, "for");
    EXPECT_EQ(topo.loop_layers[0].tier, 0);
    ASSERT_EQ(topo.loop_layers[0].members.size(), 2u);
    EXPECT_EQ(topo.loop_layers[0].members[0], "for");
    EXPECT_EQ(topo.loop_layers[0].members[1], "body");
    EXPECT_TRUE(topo.loop_layers[0].data_back_edge_sources.empty());

    // Back-edge still carries a signal route (the loop-back signal).
    ASSERT_EQ(topo.signal_routes.size(), 2u);
}

TEST(SignalAwareCompileTest, PureDataCycleRejectedAsUnbackedDataCycle)
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
    // 纯数据环（零顺序线）：第一圈都没有起点——fail-closed 拒绝并给出
    // 指引（用 while/for 承载或把完成线连回循环头），execution_order 清空。
    EXPECT_TRUE(plan.execution_order.empty());
    EXPECT_TRUE(HasUnbackedDataCycleDiagnostic(plan, "while"));
    EXPECT_TRUE(HasUnbackedDataCycleDiagnostic(plan, "signal"));
    // 数据环新语义不复用 Kahn 兜底诊断（那是拓扑真失败专用）。
    EXPECT_FALSE(HasCyclicEdgeGraphDiagnostic(plan));
}

TEST(
    SignalAwareCompileTest,
    HalfLoopWithoutSignalCircuitRejectedAsUnbackedDataCycle)
{
    StubFactoryManager mgr;
    auto node_def = MakeDefinition({{"in", "int"}}, {{"out", "int"}});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", node_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", node_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("A", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("B", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    // 半吊子环：顺序线前向 + 数据边回来——没有 signal 回路承载，跑一次
    // 即停，用户以为是循环实为静默错误，必须显性拒绝。
    doc.edges.push_back(MakeSignalEdge("e1", "A", "out", "B", "in"));
    doc.edges.push_back(MakeEdge("e2", "B", "out", "A", "in"));

    auto plan = compiler.Compile(doc);
    EXPECT_TRUE(plan.execution_order.empty());
    EXPECT_TRUE(HasUnbackedDataCycleDiagnostic(plan, "while"));
    EXPECT_FALSE(HasCyclicEdgeGraphDiagnostic(plan));
}

TEST(SignalAwareCompileTest, SignalCarriedDataBackEdgesPassUnbackedCheck)
{
    StubFactoryManager mgr;
    auto               head_def = MakeDefinition(
        {{"cond", "int"}, {"loop_in", "signal"}},
        {{"continue", "signal"}});
    auto tail_def = MakeDefinition(
        {{"in", "signal"}},
        {{"done", "signal"}, {"out", "int"}});
    mgr.AddDefinition("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA", head_def);
    mgr.AddDefinition("BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB", tail_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("head", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("tail", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    // 顺序回边（signal）承载迭代 + 数据回边回传循环状态——合法形态，
    // 放行且保序（数据回边不触发承载拒绝）。
    doc.edges.push_back(MakeSignalEdge("e1", "head", "continue", "tail", "in"));
    doc.edges.push_back(
        MakeSignalEdge("e2", "tail", "done", "head", "loop_in"));
    doc.edges.push_back(MakeEdge("e3", "tail", "out", "head", "cond"));

    auto plan = compiler.Compile(doc);
    EXPECT_FALSE(plan.execution_order.empty());
    ASSERT_EQ(plan.execution_order.size(), 2u);
    EXPECT_EQ(plan.execution_order[0], "head");
    EXPECT_EQ(plan.execution_order[1], "tail");
    EXPECT_FALSE(HasUnbackedDataCycleDiagnostic(plan));
    EXPECT_FALSE(HasCyclicEdgeGraphDiagnostic(plan));

    ASSERT_EQ(plan.back_edges.size(), 2u);
    ASSERT_EQ(plan.loop_layers.size(), 1u);
    EXPECT_EQ(plan.loop_layers[0].head_node_id, "head");
    ASSERT_EQ(plan.loop_layers[0].data_back_edge_sources.size(), 1u);
    EXPECT_EQ(plan.loop_layers[0].data_back_edge_sources[0].node_id, "tail");
    EXPECT_EQ(plan.loop_layers[0].data_back_edge_sources[0].port_id, "out");
}

// ---------------------------------------------------------------------
// 嵌套循环分层：外层 while（W1）内含内层 while（W2），内层体 A 数据回边
// 喂自身。整图互相可达坍缩成一个大 SCC，必须按信号回边头拆成自然环两
// 层：外层 members ⊇ 内层全员（含闭包生产者 P1）、tier 按嵌套深度、每
// 条回边归属自己的层（内层数据回边归内层、外层 signal 回边归外层）。
// ---------------------------------------------------------------------
TEST(SignalAwareCompileTest, NestedWhileLoopsRecordTwoTiersAndOwnership)
{
    GraphCompiler compiler;

    GraphDocumentDto doc;
    for (const char* id : {"W1", "B", "W2", "A", "C1", "P1"})
    {
        doc.nodes.push_back(
            MakeComponentNode(id, "00000000-0000-0000-0000-000000000001"));
    }
    // 顺序线：外层头→外层体→内层头→内层体；内层回边回内层头，内层退
    // 出线连外层尾，外层回边回外层头（break 无作用域：连哪算哪）。
    doc.edges.push_back(MakeSignalEdge("e_s1", "W1", "continue", "B", "in"));
    doc.edges.push_back(MakeSignalEdge("e_s2", "B", "out", "W2", "in"));
    doc.edges.push_back(MakeSignalEdge("e_s3", "W2", "continue", "A", "in"));
    doc.edges.push_back(MakeSignalEdge("e_s4", "A", "done", "W2", "loop_in"));
    doc.edges.push_back(MakeSignalEdge("e_s5", "A", "break", "C1", "in"));
    doc.edges.push_back(MakeSignalEdge("e_s6", "C1", "done", "W1", "loop_in"));
    // 内层体数据回边喂自身；内层→外层体数据边（两层均环内成员，非泄
    // 漏形状）；无顺序线生产者 P1 的消费者在内层（闭包入最内层）。
    doc.edges.push_back(MakeEdge("e_d1", "A", "out", "A", "state"));
    doc.edges.push_back(MakeEdge("e_d2", "A", "out", "C1", "value"));
    doc.edges.push_back(MakeEdge("e_d3", "P1", "out", "A", "extra"));

    auto topo = compiler.ComputeTopology(doc);

    ASSERT_EQ(topo.execution_order.size(), 6u);
    ASSERT_EQ(topo.loop_layers.size(), 2u);
    const Dto::LoopLayerDto* outer = nullptr;
    const Dto::LoopLayerDto* inner = nullptr;
    for (const auto& layer : topo.loop_layers)
    {
        if (layer.head_node_id == "W1")
        {
            outer = &layer;
        }
        else if (layer.head_node_id == "W2")
        {
            inner = &layer;
        }
    }
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(inner, nullptr);

    EXPECT_EQ(outer->tier, 0);
    EXPECT_EQ(inner->tier, 1);

    // 外层环成员含内环全员（含闭包成员）；内层成员为自身自然环 + 闭包。
    std::unordered_set<std::string> outer_members(
        outer->members.begin(),
        outer->members.end());
    ASSERT_EQ(outer->members.size(), 6u);
    for (const auto& member : inner->members)
    {
        EXPECT_EQ(outer_members.count(member), 1u)
            << "outer layer must contain every inner member: " << member;
    }
    ASSERT_EQ(inner->members.size(), 3u);
    EXPECT_EQ(inner->members[0], "W2");
    EXPECT_EQ(inner->members[1], "A");
    EXPECT_EQ(inner->members[2], "P1");

    // 回边归属：内层 signal 回边与数据回边归内层、外层 signal 回边归外层。
    const Dto::BackEdgeDto* inner_back = nullptr;
    const Dto::BackEdgeDto* outer_back = nullptr;
    const Dto::BackEdgeDto* data_back = nullptr;
    for (const auto& be : topo.back_edges)
    {
        if (be.edge_id == "e_s4")
        {
            inner_back = &be;
        }
        else if (be.edge_id == "e_s6")
        {
            outer_back = &be;
        }
        else if (be.edge_id == "e_d1")
        {
            data_back = &be;
        }
    }
    ASSERT_NE(inner_back, nullptr);
    ASSERT_NE(outer_back, nullptr);
    ASSERT_NE(data_back, nullptr);
    EXPECT_EQ(inner_back->edge_kind, "signal");
    EXPECT_EQ(inner_back->loop_id, "loop::W2");
    EXPECT_EQ(outer_back->edge_kind, "signal");
    EXPECT_EQ(outer_back->loop_id, "loop::W1");
    EXPECT_EQ(data_back->edge_kind, "data");
    EXPECT_EQ(data_back->loop_id, "loop::W2");

    // 数据回边源口只记在内层（外层自身的环承载通道为空——内层的源口归
    // 内层清单，外层发火时按嵌套关系清内层清单）。
    ASSERT_EQ(inner->data_back_edge_sources.size(), 1u);
    EXPECT_EQ(inner->data_back_edge_sources[0].node_id, "A");
    EXPECT_EQ(inner->data_back_edge_sources[0].port_id, "out");
    EXPECT_TRUE(outer->data_back_edge_sources.empty());

    // 内层→外层体的数据边不出泄漏提示；嵌套承载形态无承载拒绝。
    EXPECT_TRUE(topo.diagnostics.empty());
}

// ---------------------------------------------------------------------
// 内环节点的数据回边直连外层头时归属外层：该通道携带外层自己的循环
// 状态，记进内层清单会被「外层发火清严格内层源口」误清，外层头重启
// 后读不到上圈回边值。
// ---------------------------------------------------------------------
TEST(SignalAwareCompileTest, DataBackEdgeToOuterHeadOwnedByOuterLayer)
{
    GraphCompiler compiler;

    GraphDocumentDto doc;
    for (const char* id : {"W1", "B", "W2", "A", "C1"})
    {
        doc.nodes.push_back(
            MakeComponentNode(id, "00000000-0000-0000-0000-000000000001"));
    }
    doc.edges.push_back(MakeSignalEdge("e_s1", "W1", "continue", "B", "in"));
    doc.edges.push_back(MakeSignalEdge("e_s2", "B", "out", "W2", "in"));
    doc.edges.push_back(MakeSignalEdge("e_s3", "W2", "continue", "A", "in"));
    doc.edges.push_back(MakeSignalEdge("e_s4", "A", "done", "W2", "loop_in"));
    doc.edges.push_back(MakeSignalEdge("e_s5", "A", "break", "C1", "in"));
    doc.edges.push_back(MakeSignalEdge("e_s6", "C1", "done", "W1", "loop_in"));
    // 内层体自身数据回边（归内层）+ 内层体直连外层头的数据回边（归外层）。
    doc.edges.push_back(MakeEdge("e_d1", "A", "out", "A", "state"));
    doc.edges.push_back(MakeEdge("e_d2", "A", "count", "W1", "cond"));

    auto topo = compiler.ComputeTopology(doc);

    ASSERT_EQ(topo.loop_layers.size(), 2u);
    const Dto::LoopLayerDto* outer = nullptr;
    const Dto::LoopLayerDto* inner = nullptr;
    for (const auto& layer : topo.loop_layers)
    {
        if (layer.head_node_id == "W1")
        {
            outer = &layer;
        }
        else if (layer.head_node_id == "W2")
        {
            inner = &layer;
        }
    }
    ASSERT_NE(outer, nullptr);
    ASSERT_NE(inner, nullptr);

    const Dto::BackEdgeDto* inner_data_back = nullptr;
    const Dto::BackEdgeDto* outer_data_back = nullptr;
    for (const auto& be : topo.back_edges)
    {
        if (be.edge_id == "e_d1")
        {
            inner_data_back = &be;
        }
        else if (be.edge_id == "e_d2")
        {
            outer_data_back = &be;
        }
    }
    ASSERT_NE(inner_data_back, nullptr);
    ASSERT_NE(outer_data_back, nullptr);

    // 直连外层头的数据回边记档在外层（loop_head_node_id 与源口清单），
    // 层内自环通道维持归内层。
    EXPECT_EQ(outer_data_back->edge_kind, "data");
    EXPECT_EQ(outer_data_back->loop_head_node_id, "W1");
    EXPECT_EQ(inner_data_back->edge_kind, "data");
    EXPECT_EQ(inner_data_back->loop_head_node_id, "W2");

    ASSERT_EQ(outer->data_back_edge_sources.size(), 1u);
    EXPECT_EQ(outer->data_back_edge_sources[0].node_id, "A");
    EXPECT_EQ(outer->data_back_edge_sources[0].port_id, "count");
    ASSERT_EQ(inner->data_back_edge_sources.size(), 1u);
    EXPECT_EQ(inner->data_back_edge_sources[0].node_id, "A");
    EXPECT_EQ(inner->data_back_edge_sources[0].port_id, "out");

    EXPECT_TRUE(topo.diagnostics.empty());
}

// ---------------------------------------------------------------------
// 闭包边界：无顺序线生产者的归属由消费者位置精确决定——P1 的消费者在
// 环内 → 并入环成员每圈重算；P2 的消费者全在环外 → 不入任何层（启动
// 时只算一次）。
// ---------------------------------------------------------------------
TEST(SignalAwareCompileTest, ProducerClosureJoinsOnlyItsConsumerLoop)
{
    GraphCompiler compiler;

    GraphDocumentDto doc;
    for (const char* id : {"W", "A", "C", "P1", "P2", "Q"})
    {
        doc.nodes.push_back(
            MakeComponentNode(id, "00000000-0000-0000-0000-000000000001"));
    }
    doc.edges.push_back(MakeSignalEdge("e_s1", "W", "continue", "A", "in"));
    doc.edges.push_back(MakeSignalEdge("e_s2", "A", "done", "C", "in"));
    doc.edges.push_back(MakeSignalEdge("e_s3", "C", "done", "W", "loop_in"));
    doc.edges.push_back(MakeEdge("e_d1", "P1", "out", "A", "extra"));
    doc.edges.push_back(MakeEdge("e_d2", "P2", "out", "Q", "in"));

    auto topo = compiler.ComputeTopology(doc);

    ASSERT_EQ(topo.execution_order.size(), 6u);
    ASSERT_EQ(topo.loop_layers.size(), 1u);
    EXPECT_EQ(topo.loop_layers[0].head_node_id, "W");
    EXPECT_EQ(topo.loop_layers[0].tier, 0);
    ASSERT_EQ(topo.loop_layers[0].members.size(), 4u);
    EXPECT_EQ(topo.loop_layers[0].members[0], "W");
    EXPECT_EQ(topo.loop_layers[0].members[1], "A");
    EXPECT_EQ(topo.loop_layers[0].members[2], "C");
    EXPECT_EQ(topo.loop_layers[0].members[3], "P1");
    // P2/Q 不属于任何层：P2 启动时只算一次，Q 不参与环重置。
    for (const auto& layer : topo.loop_layers)
    {
        for (const auto& member : layer.members)
        {
            EXPECT_NE(member, "P2");
            EXPECT_NE(member, "Q");
        }
    }
}

// ---------------------------------------------------------------------
// 泄漏 lint：环成员的数据边直连环外节点（不属于任何层成员、非回边）→
// LoopBodyDataLeak warning——该口在跳过的圈次不刷新，环外读者读到上一
// 活跃圈的值。提示不阻断编译：execution_order 保持非空。
// ---------------------------------------------------------------------
TEST(SignalAwareCompileTest, LoopBodyDataLeakWarnsWithoutBlocking)
{
    GraphCompiler compiler;

    GraphDocumentDto doc;
    for (const char* id : {"W", "A", "C", "X"})
    {
        doc.nodes.push_back(
            MakeComponentNode(id, "00000000-0000-0000-0000-000000000001"));
    }
    doc.edges.push_back(MakeSignalEdge("e_s1", "W", "continue", "A", "in"));
    doc.edges.push_back(MakeSignalEdge("e_s2", "A", "done", "C", "in"));
    doc.edges.push_back(MakeSignalEdge("e_s3", "C", "done", "W", "loop_in"));
    doc.edges.push_back(MakeEdge("e_d1", "A", "out", "X", "in"));

    auto plan = compiler.Compile(doc);

    // warning 不阻断：全部节点仍进 execution_order，也无承载拒绝。
    ASSERT_EQ(plan.execution_order.size(), 4u);
    EXPECT_FALSE(HasUnbackedDataCycleDiagnostic(plan));
    EXPECT_FALSE(HasCyclicEdgeGraphDiagnostic(plan));
    EXPECT_TRUE(HasLoopBodyDataLeakDiagnostic(plan, "A"));
}

// 会话路径钉测：authoring 会话编译只绑 port_surface_provider（镜像
// GraphAuthoringSessionCompile 的注入形态），规范口名 signal 边落在不声明
// signal 口的组件上零 PortNotFound（无论此前是否已刷过噪声，豁免后的
// 终态由本用例固化）。
TEST(SignalAwareCompileTest, CanonicalSignalPortsSkipPortNotFoundViaProvider)
{
    GraphCompiler compiler;
    compiler.SetPortSurfaceProvider(
        [](const std::string& component_guid)
            -> std::optional<GraphCompiler::ComponentPortSurface>
        {
            if (component_guid != "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"
                && component_guid != "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB")
            {
                return std::nullopt;
            }
            GraphCompiler::ComponentPortSurface surface;
            Dto::GraphPortDefinitionDto         input;
            input.port_id = "value";
            input.port_type = "int";
            surface.inputs.push_back(std::move(input));
            Dto::GraphPortDefinitionDto output;
            output.port_id = "out";
            output.port_type = "int";
            surface.outputs.push_back(std::move(output));
            return surface;
        });

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("first", "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
    doc.nodes.push_back(
        MakeComponentNode("second", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));
    // 两条规范口名 signal 边：两端组件都不声明 signal_in/signal_out。
    doc.edges.push_back(
        MakeSignalEdge("e1", "first", "signal_out", "second", "signal_in"));
    doc.edges.push_back(
        MakeSignalEdge("e2", "second", "signal_out", "first", "signal_in"));

    auto              plan = compiler.Compile(doc);
    const std::string port_not_found_kind =
        std::to_string(static_cast<int>(CompileDiagnosticKind::PortNotFound));
    for (const auto& diag : plan.diagnostics)
    {
        auto obj = diag.as_object();
        ASSERT_TRUE(obj.has_value());
        auto kind = (*obj)[std::string_view("kind")].as_string();
        EXPECT_NE(kind.value_or(""), port_not_found_kind)
            << "canonical signal ports must be exempt from PortNotFound";
    }
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
