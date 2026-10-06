#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>
#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/GraphCompiler.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskAuthoringSession.Implements.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
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

    // LoopCarriedPortInvalid diagnostic kind（loopCarried 输出口无同名输入
    // 口配对）：kind 以枚举整数字符串序列化。
    bool HasLoopCarriedPortInvalidDiagnostic(
        const Dto::CompiledGraphPlanDto& plan,
        const std::string&               node_id = {})
    {
        const std::string invalid_kind = std::to_string(
            static_cast<int>(CompileDiagnosticKind::LoopCarriedPortInvalid));
        for (const auto& diag : plan.diagnostics)
        {
            auto obj = diag.as_object();
            if (!obj.has_value())
            {
                continue;
            }
            auto kind = (*obj)[std::string_view("kind")].as_string();
            if (!kind.has_value() || *kind != invalid_kind)
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
// 全类型词汇矩阵（D-20）：image/base/component 三词同词等价（族内仅自
// 身）、与既有九词及彼此跨族一律拒、未知词维持 permissive+warning——
// begin/end 签名类型域=全部类型的 Core 侧判定面。
// ---------------------------------------------------------------------
TEST(PortTypeCompatibilityTest, FullTypeVocabularyMatrix)
{
    struct MatrixEntry
    {
        const char* source_type;
        const char* target_type;
        bool        expected_compatible;
    };
    const MatrixEntry matrix[] = {
        // 同词等价
        {"image", "image", true},
        {"base", "base", true},
        {"component", "component", true},
        // 三词与标量词跨族互拒
        {"image", "string", false},
        {"string", "image", false},
        {"int", "image", false},
        {"image", "int", false},
        {"image", "bool", false},
        {"base", "int", false},
        {"int", "base", false},
        {"base", "bool", false},
        {"component", "string", false},
        // 三词彼此互拒（QI 互认与身份互通已按 UI 侧规则删除）
        {"base", "component", false},
        {"component", "base", false},
        {"image", "base", false},
        {"base", "image", false},
        {"image", "component", false},
        {"component", "image", false},
        // 未知词维持 permissive（UnknownPortType warning 路径不借道）
        {"custom_word", "int", true},
        {"int", "custom_word", true}};

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
    // 四条点对点边 + 一条推导 Boundary（A.in1 悬空——B/C/D 的 in1 均有
    // 入边，只有链头的 A 缺值来源）。
    ASSERT_EQ(plan.bindings.size(), 5u);
    const PortBindingDto* boundary_binding = nullptr;
    for (const auto& binding : plan.bindings)
    {
        if (binding.kind == PortBindingKind::Boundary)
        {
            boundary_binding = &binding;
        }
    }
    ASSERT_NE(boundary_binding, nullptr);
    // Boundary 记录无生产者位点（口 id 即目标口 id）。
    EXPECT_TRUE(boundary_binding->source_node_id.empty());
    EXPECT_TRUE(boundary_binding->source_port_id.empty());
    EXPECT_EQ(boundary_binding->target_node_id, "A");
    EXPECT_EQ(boundary_binding->target_port_id, "in1");
    EXPECT_EQ(boundary_binding->expected_type, "int");
    // Boundary 绑定的目标口类型来自边界推导的声明类型（与源侧同源）。
    EXPECT_EQ(boundary_binding->target_type, "int");
    EXPECT_TRUE(boundary_binding->default_value.is_null());
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
    // 边绑定 + B.in1 悬空推导的 Boundary 绑定（in_unknown 占走了入边）。
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
    // 无边图：唯一的绑定是悬空声明输入 in1 的 kind=Boundary 记录
    // （声明通道不携带默认值 → default 通道 null，运行时按口未设处理）。
    ASSERT_EQ(plan.bindings.size(), 1u);
    EXPECT_EQ(plan.bindings[0].kind, PortBindingKind::Boundary);
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
    EXPECT_EQ(inner_back->loop_head_node_id, "W2");
    EXPECT_EQ(outer_back->edge_kind, "signal");
    EXPECT_EQ(outer_back->loop_head_node_id, "W1");
    EXPECT_EQ(data_back->edge_kind, "data");
    EXPECT_EQ(data_back->loop_head_node_id, "W2");

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
// loopCarried 通道合成：成对声明的 index 口在编译产物中长出自环值路由
// （binding）与 data 回边记档（归属 for 自身层），内部状态口不投影为
// 图边界输入。
// ---------------------------------------------------------------------
TEST(SignalAwareCompileTest, LoopCarriedPairSynthesizesSelfLoopChannel)
{
    GraphCompiler compiler;

    StubFactoryManager factory_manager;
    factory_manager.AddDefinition(
        "00000000-0000-0000-0000-0000000000f1",
        *Das::Utils::ParseYyjsonFromString(R"({
            "inputs": [
                {"id": "index", "type": "int"},
                {"id": "start", "type": "int"},
                {"id": "end", "type": "int"}
            ],
            "outputs": [
                {"id": "continue", "type": "signal"},
                {"id": "break", "type": "signal"},
                {"id": "index", "type": "int", "loopCarried": true}
            ]
        })"));
    factory_manager.AddDefinition(
        "00000000-0000-0000-0000-0000000000b1",
        *Das::Utils::ParseYyjsonFromString(
            R"({"inputs": [{"id": "value", "type": "int"}],
                "outputs": [{"id": "signal_out", "type": "signal"}]})"));
    compiler.SetFactoryManager(&factory_manager);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("F", "00000000-0000-0000-0000-0000000000f1"));
    doc.nodes.push_back(
        MakeComponentNode("B", "00000000-0000-0000-0000-0000000000b1"));
    doc.edges.push_back(
        MakeSignalEdge("e_s1", "F", "continue", "B", "signal_in"));
    doc.edges.push_back(
        MakeSignalEdge("e_s2", "B", "signal_out", "F", "signal_in"));
    doc.edges.push_back(MakeEdge("e_d1", "F", "index", "B", "value"));

    auto plan = compiler.Compile(doc);

    EXPECT_FALSE(HasLoopCarriedPortInvalidDiagnostic(plan));

    // 合成自环 binding：F.index → F.index，类型按声明双侧填充。
    const Dto::PortBindingDto* self_binding = nullptr;
    for (const auto& binding : plan.binding_plan.bindings)
    {
        if (binding.source_node_id == "F" && binding.source_port_id == "index"
            && binding.target_node_id == "F"
            && binding.target_port_id == "index")
        {
            self_binding = &binding;
        }
    }
    ASSERT_NE(self_binding, nullptr);
    EXPECT_EQ(self_binding->expected_type, "int");
    EXPECT_EQ(self_binding->target_type, "int");

    // 合成 data 回边记档（归属 for 自身层），源口入该层清单。
    const Dto::BackEdgeDto* carried_back = nullptr;
    for (const auto& back_edge : plan.back_edges)
    {
        if (back_edge.edge_kind == "data" && back_edge.source_node_id == "F"
            && back_edge.target_node_id == "F")
        {
            carried_back = &back_edge;
        }
    }
    ASSERT_NE(carried_back, nullptr);
    EXPECT_EQ(carried_back->edge_id, "loopcarried::F::index");
    EXPECT_EQ(carried_back->loop_head_node_id, "F");

    ASSERT_EQ(plan.loop_layers.size(), 1u);
    ASSERT_EQ(plan.loop_layers[0].data_back_edge_sources.size(), 1u);
    EXPECT_EQ(plan.loop_layers[0].data_back_edge_sources[0].node_id, "F");
    EXPECT_EQ(plan.loop_layers[0].data_back_edge_sources[0].port_id, "index");

    // 内部状态口不出现在图边界输入面（start/end 悬空仍正常投影）。
    std::unordered_set<std::string> boundary_input_ids;
    for (const auto& boundary_input : plan.boundary_ports.inputs)
    {
        boundary_input_ids.insert(boundary_input.port_id);
    }
    EXPECT_EQ(boundary_input_ids.count("index"), 0u);
    EXPECT_EQ(boundary_input_ids.count("start"), 1u);
    EXPECT_EQ(boundary_input_ids.count("end"), 1u);
}

// ---------------------------------------------------------------------
// loopCarried 校验：输出口带 loopCarried 声明但无同名输入口配对时编译
// 诊断报错，且不合成任何自环通道。
// ---------------------------------------------------------------------
TEST(SignalAwareCompileTest, LoopCarriedOutputWithoutSameNameInputDiagnosed)
{
    GraphCompiler compiler;

    StubFactoryManager factory_manager;
    factory_manager.AddDefinition(
        "00000000-0000-0000-0000-0000000000f2",
        *Das::Utils::ParseYyjsonFromString(R"({
            "inputs": [{"id": "start", "type": "int"}],
            "outputs": [
                {"id": "count", "type": "int", "loopCarried": true}
            ]
        })"));
    compiler.SetFactoryManager(&factory_manager);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("N", "00000000-0000-0000-0000-0000000000f2"));

    auto plan = compiler.Compile(doc);

    EXPECT_TRUE(HasLoopCarriedPortInvalidDiagnostic(plan, "N"));

    // 无配对输入口 → 不合成自环 binding，也不记 data 回边。
    for (const auto& binding : plan.binding_plan.bindings)
    {
        EXPECT_FALSE(
            binding.source_node_id == "N" && binding.target_node_id == "N");
    }
    for (const auto& back_edge : plan.back_edges)
    {
        EXPECT_FALSE(
            back_edge.source_node_id == "N" && back_edge.target_node_id == "N");
    }
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

// ---------------------------------------------------------------------------
// Manifest 端口面清理后的三态语义（执行输入口删除、报告口保留）
// ---------------------------------------------------------------------------

// 执行输入口从声明面删除后，触发线落规范 signal_in 隐式豁免——豁免只认
// wire 层坐标名，不依赖组件声明任何信号口。
TEST(PortCleanedSurfaceTest, SignalEdgeIntoCleanedComponentCompiles)
{
    StubFactoryManager mgr;
    // 模拟口面清理后的执行组件形态：零声明输入口、报告输出口在位。
    auto cleaned_def = MakeDefinition(
        {},
        {{"completedTasks", "array<string>"},
         {"stopped", "bool"},
         {"diagnostics", "array<object>"},
         {"nextTime", "string"}});
    mgr.AddDefinition("CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC", cleaned_def);
    mgr.AddDefinition(
        "DDDDDDDD-DDDD-DDDD-DDDD-DDDDDDDDDDDD",
        MakeDefinition({}, {}));

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("trigger", "DDDDDDDD-DDDD-DDDD-DDDD-DDDDDDDDDDDD"));
    doc.nodes.push_back(
        MakeComponentNode("run", "CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC"));
    doc.edges.push_back(
        MakeSignalEdge("e_sig", "trigger", "signal_out", "run", "signal_in"));

    EXPECT_TRUE(compiler.ValidateEdgePorts(doc).empty());

    auto plan = compiler.Compile(doc);
    ASSERT_EQ(plan.execution_order.size(), 2u);
    const std::string port_not_found_kind =
        std::to_string(static_cast<int>(CompileDiagnosticKind::PortNotFound));
    for (const auto& diag : plan.diagnostics)
    {
        auto obj = diag.as_object();
        ASSERT_TRUE(obj.has_value());
        auto kind = (*obj)[std::string_view("kind")].as_string();
        EXPECT_NE(kind.value_or(""), port_not_found_kind)
            << "canonical signal_in must stay exempt after port removal";
    }
}

// 声明面收缩直达边界推导：已删输入口不再派生边界输入（无悬空可言），
// 报告输出口仍全量导出（报告口=动作契约静态，不随清理收缩）。
TEST(PortCleanedSurfaceTest, BoundaryFollowsShrunkDeclarationSurface)
{
    StubFactoryManager mgr;
    auto               cleaned_def = MakeDefinition(
        {},
        {{"completedTasks", "array<string>"},
         {"stopped", "bool"},
         {"diagnostics", "array<object>"},
         {"nextTime", "string"}});
    mgr.AddDefinition("CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC", cleaned_def);

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("run", "CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC"));

    auto plan = compiler.Compile(doc);
    EXPECT_TRUE(plan.boundary_ports.inputs.empty());
    bool next_time_exported = false;
    for (const auto& output : plan.boundary_ports.outputs)
    {
        if (output.port_id == "nextTime")
        {
            next_time_exported = true;
        }
    }
    EXPECT_TRUE(next_time_exported);
}

// 连到已删口的边（非规范口名）fail-closed 拒绝——豁免救不了已删的声明
// 口（数据口 executionInput 与遗留信号口名 in 同判）。
TEST(PortCleanedSurfaceTest, EdgeIntoRemovedDeclarationPortFailsClosed)
{
    StubFactoryManager mgr;
    auto               cleaned_def = MakeDefinition(
        {},
        {{"completedTasks", "array<string>"},
         {"stopped", "bool"},
         {"diagnostics", "array<object>"},
         {"nextTime", "string"}});
    mgr.AddDefinition("CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC", cleaned_def);
    mgr.AddDefinition(
        "DDDDDDDD-DDDD-DDDD-DDDD-DDDDDDDDDDDD",
        MakeDefinition({}, {{"out", "int"}}));

    GraphCompiler compiler;
    compiler.SetFactoryManager(&mgr);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("trigger", "DDDDDDDD-DDDD-DDDD-DDDD-DDDDDDDDDDDD"));
    doc.nodes.push_back(
        MakeComponentNode("run", "CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC"));
    doc.edges.push_back(
        MakeEdge("e_data", "trigger", "out", "run", "executionInput"));

    auto diagnostics = compiler.ValidateEdgePorts(doc);
    ASSERT_FALSE(diagnostics.empty());
    EXPECT_EQ(diagnostics[0].kind, CompileDiagnosticKind::PortNotFound);
    EXPECT_EQ(diagnostics[0].node_id, "run");
    EXPECT_EQ(diagnostics[0].port_id, "executionInput");

    GraphDocumentDto legacy_doc = doc;
    legacy_doc.edges.clear();
    legacy_doc.edges.push_back(
        MakeSignalEdge("e_legacy", "trigger", "signal_out", "run", "in"));
    auto legacy_diagnostics = compiler.ValidateEdgePorts(legacy_doc);
    ASSERT_FALSE(legacy_diagnostics.empty());
    EXPECT_EQ(legacy_diagnostics[0].kind, CompileDiagnosticKind::PortNotFound);
    EXPECT_EQ(legacy_diagnostics[0].node_id, "run");
    EXPECT_EQ(legacy_diagnostics[0].port_id, "in");
}

TEST(SignalAwareCompileTest, DanglingInputsBoundToBoundaryKind)
{
    StubFactoryManager mgr;
    // 节点声明两个输入口，均无入边——都推导为边界入参（kind=Boundary
    // 记录；声明通道不携带默认值 → default 通道 null，运行时按口未设
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
    EXPECT_EQ(threshold_binding->kind, PortBindingKind::Boundary);
    EXPECT_EQ(threshold_binding->target_node_id, "N");
    EXPECT_EQ(threshold_binding->target_port_id, "threshold");
    EXPECT_EQ(threshold_binding->expected_type, "int");
    EXPECT_TRUE(threshold_binding->default_value.is_null());
    ASSERT_NE(private_binding, nullptr);
    EXPECT_EQ(private_binding->kind, PortBindingKind::Boundary);
    EXPECT_TRUE(private_binding->default_value.is_null());
}

TEST(SignalAwareCompileTest, DanglingInputsBoundaryToOneToManyNodes)
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
    // 同名悬空输入扇出到两个节点（one-to-many by-name 合并——每消费者
    // 一条独立 Boundary 记录）。
    ASSERT_EQ(plan.bindings.size(), 2u);
    EXPECT_EQ(plan.bindings[0].target_node_id, "N1");
    EXPECT_EQ(plan.bindings[1].target_node_id, "N2");
    for (const auto& binding : plan.bindings)
    {
        EXPECT_EQ(binding.kind, PortBindingKind::Boundary);
        EXPECT_EQ(binding.target_port_id, "threshold");
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

// ---------------------------------------------------------------------
// begin 输出口导出排除（D-14）：begin 的同名 out 口是图内值边源，导出会
// 造成入参回显污染出参面——输出侧跳过 begin 节点的口；输入侧推导零改动
// （begin 的悬空 in 口照常是边界入参）。GUID 钉值独立于
// GraphBoundaryComponents.h 常量。
// ---------------------------------------------------------------------
constexpr std::string_view BOUNDARY_BEGIN_COMPONENT_GUID =
    "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E";
constexpr std::string_view BOUNDARY_END_COMPONENT_GUID =
    "E1B7C9D3-5F4A-4E2B-8D6C-7A9E1F3B5C2D";

TEST(BoundaryPortDerivationTest, BeginOutPortsExcludedFromBoundaryOutputs)
{
    std::unordered_map<
        std::string,
        std::pair<
            std::vector<std::pair<std::string, std::string>>,
            std::vector<std::pair<std::string, std::string>>>>
        surfaces;
    surfaces["BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"] = {
        {{"eat", "int"}},
        {{"result", "string"}}};

    GraphCompiler compiler;
    compiler.SetPortSurfaceProvider(MakePortSurfaceProvider(surfaces));
    // begin 文档口面经 nodeFaces 直读注入（不经 provider 任何路径）——
    // 口对双侧展开由推导层承担，输出侧排除见 D-14。
    GraphCompiler::NodeFacesMap node_faces;
    node_faces["n_begin"] = {
        {"左值", "int", "左值", "input"},
    };
    compiler.BindNodeFaces(&node_faces);

    GraphDocumentDto doc;
    doc.nodes.push_back(MakeComponentNode(
        "n_begin",
        std::string(BOUNDARY_BEGIN_COMPONENT_GUID)));
    doc.nodes.push_back(
        MakeComponentNode("n_work", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));

    auto boundary = compiler.DeriveBoundaryPorts(doc);

    // 输出侧：begin 的 out 口排除，仅普通节点的 result 导出。
    ASSERT_EQ(boundary.outputs.size(), 1u);
    EXPECT_EQ(boundary.outputs[0].port_id, "result");
    EXPECT_EQ(boundary.outputs[0].node_ids[0], "n_work");

    // 输入侧零改动：begin 的悬空 in 口与普通节点的悬空输入都照常推导。
    ASSERT_EQ(boundary.inputs.size(), 2u);
    bool begin_input_found = false;
    bool work_input_found = false;
    for (const auto& boundary_input : boundary.inputs)
    {
        if (boundary_input.node_ids[0] == "n_begin")
        {
            begin_input_found = true;
            EXPECT_EQ(boundary_input.port_id, "左值");
        }
        if (boundary_input.node_ids[0] == "n_work")
        {
            work_input_found = true;
            EXPECT_EQ(boundary_input.port_id, "eat");
        }
    }
    EXPECT_TRUE(begin_input_found);
    EXPECT_TRUE(work_input_found);
}

// 防误伤对称断言：end 的输出口照常导出——排除规则只作用于 begin 节点
// GUID，end 的口就是子图出参面。口面经 nodeFaces 直读注入。
TEST(BoundaryPortDerivationTest, EndOutPortsStillExported)
{
    GraphCompiler               compiler;
    GraphCompiler::NodeFacesMap node_faces;
    node_faces["n_end"] = {
        {"值", "int", "值", "output"},
        {"右值", "string", "右值", "output"},
    };
    compiler.BindNodeFaces(&node_faces);

    GraphDocumentDto doc;
    doc.nodes.push_back(
        MakeComponentNode("n_end", std::string(BOUNDARY_END_COMPONENT_GUID)));

    auto boundary = compiler.DeriveBoundaryPorts(doc);

    ASSERT_EQ(boundary.outputs.size(), 2u);
    EXPECT_EQ(boundary.outputs[0].port_id, "值");
    EXPECT_EQ(boundary.outputs[1].port_id, "右值");
    EXPECT_EQ(boundary.outputs[0].node_ids[0], "n_end");
}

// end 输入口导出排除（D-14 输入侧镜像）：end 的口语义是「从图内收值」，
// 悬空（无入边）的 end 口永不构成图级入参——否则调用方图输入会经广播
// 绑定写进 end 转写输出，回声进出参面。begin/end 动态口 id 是节点内计
// 数器（首口恒 p_1），by-name 合并下系统性地撞成同一条边界口——排除后
// 撞名面随之消失（动态口分区仅 begin/end 可持）。
TEST(BoundaryPortDerivationTest, EndInPortsExcludedFromBoundaryInputs)
{
    std::unordered_map<
        std::string,
        std::pair<
            std::vector<std::pair<std::string, std::string>>,
            std::vector<std::pair<std::string, std::string>>>>
        surfaces;
    surfaces["BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"] = {
        {{"eat", "int"}},
        {{"result", "string"}}};

    GraphCompiler compiler;
    compiler.SetPortSurfaceProvider(MakePortSurfaceProvider(surfaces));
    // begin/end 动态口对形态（首口同 id p_1）经 nodeFaces 按节点注入。
    GraphCompiler::NodeFacesMap node_faces;
    node_faces["n_begin"] = {{"p_1", "int", "p_1", "input"}};
    node_faces["n_end"] = {{"p_1", "int", "p_1", "output"}};
    compiler.BindNodeFaces(&node_faces);

    GraphDocumentDto doc;
    doc.nodes.push_back(MakeComponentNode(
        "n_begin",
        std::string(BOUNDARY_BEGIN_COMPONENT_GUID)));
    doc.nodes.push_back(
        MakeComponentNode("n_end", std::string(BOUNDARY_END_COMPONENT_GUID)));
    doc.nodes.push_back(
        MakeComponentNode("n_work", "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"));

    auto boundary = compiler.DeriveBoundaryPorts(doc);

    // 输入侧：end 的悬空口排除；begin 的悬空 in 口照常推导；p_1 不得把
    // begin/end 合并成一条（node_ids 只含 begin）。
    ASSERT_EQ(boundary.inputs.size(), 2u);
    const Dto::BoundaryPortDto* begin_p1 = nullptr;
    for (const auto& boundary_input : boundary.inputs)
    {
        if (boundary_input.port_id == "p_1")
        {
            begin_p1 = &boundary_input;
        }
    }
    ASSERT_NE(begin_p1, nullptr);
    ASSERT_EQ(begin_p1->node_ids.size(), 1u);
    EXPECT_EQ(begin_p1->node_ids[0], "n_begin");

    // 输出侧镜像不变：end 的 p_1 照常导出（出参面），begin 的排除。
    bool end_output_found = false;
    for (const auto& boundary_output : boundary.outputs)
    {
        ASSERT_NE(boundary_output.node_ids[0], "n_begin");
        if (boundary_output.port_id == "p_1")
        {
            end_output_found = true;
            EXPECT_EQ(boundary_output.node_ids[0], "n_end");
        }
    }
    EXPECT_TRUE(end_output_found);
}

// begin/end 动态口面的唯一来源纪律：faces 条目缺席（编译宿主未跑预备
// pass）=动态口面为空降级——不产生任何幻影动态口；声明面（manifest/
// catalog 声明口）照常经 provider 解析不被误伤。faces 注入后同名声明口
// face 胜出（与 ResolveNodePorts 合并规则一致）。
TEST(BoundaryPortDerivationTest, BoundaryFacesAbsentLeavesDeclaredOnly)
{
    std::unordered_map<
        std::string,
        std::pair<
            std::vector<std::pair<std::string, std::string>>,
            std::vector<std::pair<std::string, std::string>>>>
        surfaces;
    surfaces[std::string(BOUNDARY_BEGIN_COMPONENT_GUID)] = {
        {{"declared_in", "int"}},
        {{"declared_out", "int"}}};

    GraphCompiler compiler;
    compiler.SetPortSurfaceProvider(MakePortSurfaceProvider(surfaces));

    GraphDocumentDto doc;
    doc.nodes.push_back(MakeComponentNode(
        "n_begin",
        std::string(BOUNDARY_BEGIN_COMPONENT_GUID)));

    // 无 faces：动态口面为空——边界只有声明口（declared_in 悬空推导；
    // begin 的 declared_out 输出侧被排除）。
    auto boundary = compiler.DeriveBoundaryPorts(doc);
    ASSERT_EQ(boundary.inputs.size(), 1u);
    EXPECT_EQ(boundary.inputs[0].port_id, "declared_in");
    EXPECT_TRUE(boundary.outputs.empty());

    // faces 注入：动态口面并入（口对双侧展开），同名声明口 face 胜出。
    GraphCompiler::NodeFacesMap node_faces;
    node_faces["n_begin"] = {
        {"declared_in", "string", "新面", "input"},
        {"dyn_in", "int", "dyn_in", "input"},
    };
    compiler.BindNodeFaces(&node_faces);
    auto merged = compiler.DeriveBoundaryPorts(doc);
    ASSERT_EQ(merged.inputs.size(), 2u);
    bool face_won = false;
    bool dynamic_found = false;
    for (const auto& input : merged.inputs)
    {
        if (input.port_id == "declared_in")
        {
            face_won = true;
            EXPECT_EQ(input.port_type, "string");
            EXPECT_EQ(input.display_label, "新面");
        }
        if (input.port_id == "dyn_in")
        {
            dynamic_found = true;
        }
    }
    EXPECT_TRUE(face_won);
    EXPECT_TRUE(dynamic_found);
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
// 推导悬空输入的 Boundary 集成：编译产物为悬空输入生成 kind=Boundary
// 记录（default_value 取该口声明默认值——注入端口面通道可携带默认，
// manifest 通道不携带即为 null）；边界口清单写入编译产物。
// ===================================================================

TEST(SignalAwareCompileTest, DerivedDanglingInputsGenerateBoundaryBindings)
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
        if (binding.kind != PortBindingKind::Boundary)
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
    EXPECT_EQ(p1_binding->target_node_id, "N");
    EXPECT_EQ(p1_binding->target_port_id, "p1");
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

// ---------------------------------------------------------------------------
// Document port faces (nodeFaces channel): per-node merge semantics
// ---------------------------------------------------------------------------

// face 口 id 与 catalog 声明口同 id：按口 id 去重、文档侧胜出（口面仅一
// 份，类型取 face 侧）且不拒绝——重名 WARN 语义（文档侧为准）的行为锁
// 定。声明为 int 的 shared 口被 face 覆写为 string 后，string 源边类型
// 兼容通过、绑定计划 target_type 读 face 侧。
TEST(NodeFacesMergeTest, DocumentFaceWinsOverSameNameDeclaredPort)
{
    const std::unordered_map<
        std::string,
        std::pair<
            std::vector<std::pair<std::string, std::string>>,
            std::vector<std::pair<std::string, std::string>>>>
        surfaces{
            {"{ga}", {{}, {{"out_str", "string"}, {"out_int", "int"}}}},
            {"{gb}", {{{"shared", "int"}}, {}}}};

    GraphCompiler compiler;
    compiler.SetPortSurfaceProvider(MakePortSurfaceProvider(surfaces));

    GraphCompiler::NodeFacesMap faces;
    faces["B"] = std::vector<GraphCompiler::DocumentFacePort>{
        {"shared", "string", "shared", "input"}};
    compiler.BindNodeFaces(&faces);

    GraphDocumentDto doc;
    doc.nodes.push_back(MakeComponentNode("A", "{ga}"));
    doc.nodes.push_back(MakeComponentNode("B", "{gb}"));
    doc.edges.push_back(MakeEdge("e1", "A", "out_str", "B", "shared"));

    // string→string：face 胜出后类型兼容；若声明口胜出（int）则同一
    // 条边会产出 TypeMismatch。
    const auto diagnostics = compiler.ValidateEdgePorts(doc);
    EXPECT_TRUE(diagnostics.empty());

    // 口面仅一份且类型取 face 侧：绑定计划的 target_type 读 string。
    const auto plan = compiler.Compile(doc);
    ASSERT_EQ(plan.binding_plan.bindings.size(), 1u);
    EXPECT_EQ(plan.binding_plan.bindings[0].target_type, "string");
}

// 同 guid 两节点各自注入不同 face：口面互不渗透（按节点合并，绝不按
// guid 聚合）。C 与 A/B 同 guid 但零 faces——若面按 guid 聚合，C 会继承
// b_in 使 e_leak 通过；按节点则 e_leak 的 target 口缺失（PortNotFound）。
TEST(NodeFacesMergeTest, FacesArePerNodeNeverAggregatedByGuid)
{
    // {g} 在 catalog 侧是零口组件（was_resolved=true 的空声明面）——三节
    // 点的口面差异全部来自各自的 faces，探针才隔离出「按节点合并」语
    // 义（无 provider 时未解析口面会先以 UnresolvableComponentGuid 早
    // 退，探针到不了口检查）。
    const std::unordered_map<
        std::string,
        std::pair<
            std::vector<std::pair<std::string, std::string>>,
            std::vector<std::pair<std::string, std::string>>>>
        surfaces{{"{g}", {{}, {}}}};

    GraphCompiler compiler;
    compiler.SetPortSurfaceProvider(MakePortSurfaceProvider(surfaces));

    GraphCompiler::NodeFacesMap faces;
    faces["A"] = std::vector<GraphCompiler::DocumentFacePort>{
        {"a_out", "string", "a_out", "output"}};
    faces["B"] = std::vector<GraphCompiler::DocumentFacePort>{
        {"b_in", "string", "b_in", "input"}};
    compiler.BindNodeFaces(&faces);

    GraphDocumentDto doc;
    doc.nodes.push_back(MakeComponentNode("A", "{g}"));
    doc.nodes.push_back(MakeComponentNode("B", "{g}"));
    doc.nodes.push_back(MakeComponentNode("C", "{g}"));
    // A 的 a_out → B 的 b_in：per-node 面上合法。
    doc.edges.push_back(MakeEdge("e_ok", "A", "a_out", "B", "b_in"));
    // 交叉渗透探针：A 的 a_out → C 的 b_in——C 零 faces，按 guid 聚合才
    // 会通过。
    doc.edges.push_back(MakeEdge("e_leak", "A", "a_out", "C", "b_in"));

    const auto diagnostics = compiler.ValidateEdgePorts(doc);
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].kind, CompileDiagnosticKind::PortNotFound);
    EXPECT_EQ(diagnostics[0].edge_id, "e_leak");
    EXPECT_EQ(diagnostics[0].port_id, "b_in");
}

// ===================================================================
// 存储边复核（编译期自动断线）：口面演化后按节点新口面复核存量数据
// 边——口不存在/类型不配→标记断开+StaleEdgeDisconnect 诊断（WARN）。
// 类型判据=IsTypeCompatible 单一权威+文档 types[] 注册表闭世界
// （compatibleWith）；signal 边与 faces 缺席节点不在复核域。
// ===================================================================

// 口消失断开：face 演化后目标口不在新口面（TargetPortMissing）——stale
// 记录+诊断含可定位字段（node/port id）；静态节点侧缺口（faces 条目缺
// 席——预备 pass 未跑而非演化）不进复核，ValidateEdgePorts 的
// PortNotFound 域原样保留（无误删）。
TEST(StaleEdgeReviewTest, PortGoneFromEvolvedFaceDisconnects)
{
    const std::unordered_map<
        std::string,
        std::pair<
            std::vector<std::pair<std::string, std::string>>,
            std::vector<std::pair<std::string, std::string>>>>
        surfaces{
            {"{ga}", {{}, {{"out_str", "string"}}}},
            {"{gstatic}", {{{"in_str", "string"}}, {}}}};

    GraphCompiler compiler;
    compiler.SetPortSurfaceProvider(MakePortSurfaceProvider(surfaces));

    GraphCompiler::NodeFacesMap faces;
    faces["B"] = std::vector<GraphCompiler::DocumentFacePort>{
        {"other_in", "string", "other_in", "input"}};
    compiler.BindNodeFaces(&faces);

    GraphDocumentDto doc;
    doc.nodes.push_back(MakeComponentNode("A", "{ga}"));
    doc.nodes.push_back(MakeComponentNode("B", "{gb}"));
    doc.nodes.push_back(MakeComponentNode("C", "{gstatic}"));
    // e_gone：B 的 face 已演化（b_in 不在面上）→ 断开。
    doc.edges.push_back(MakeEdge("e_gone", "A", "out_str", "B", "b_in"));
    // e_static：C 是静态节点（无 faces 条目），同款缺口不复核。
    doc.edges.push_back(MakeEdge("e_static", "A", "out_str", "C", "no_such"));

    const auto review = compiler.ReviewStaleEdges(doc);
    ASSERT_EQ(review.stale_edges.size(), 1u);
    EXPECT_EQ(review.stale_edges[0].edge_id, "e_gone");
    EXPECT_EQ(review.stale_edges[0].target_node_id, "B");
    EXPECT_EQ(review.stale_edges[0].target_port_id, "b_in");
    EXPECT_EQ(
        review.stale_edges[0].reason,
        GraphCompiler::StaleEdgeRecord::Reason::TargetPortMissing);

    ASSERT_EQ(review.diagnostics.size(), 1u);
    EXPECT_EQ(
        review.diagnostics[0].kind,
        CompileDiagnosticKind::StaleEdgeDisconnect);
    EXPECT_EQ(review.diagnostics[0].edge_id, "e_gone");
    EXPECT_EQ(review.diagnostics[0].node_id, "B");
    EXPECT_EQ(review.diagnostics[0].port_id, "b_in");
    EXPECT_NE(review.diagnostics[0].message.find("e_gone"), std::string::npos);
}

// 类型改配断开 vs 兼容保持：face 类型演化后 int→bool 不配断开；int→
// double 数值族加宽保持（IsTypeCompatible 单一权威的既有表义）。
TEST(StaleEdgeReviewTest, TypeChangeOnEvolvedFaceDisconnectsIncompatibleOnly)
{
    const std::unordered_map<
        std::string,
        std::pair<
            std::vector<std::pair<std::string, std::string>>,
            std::vector<std::pair<std::string, std::string>>>>
        surfaces{{"{ga}", {{}, {{"out_int", "int"}}}}};

    GraphCompiler compiler;
    compiler.SetPortSurfaceProvider(MakePortSurfaceProvider(surfaces));

    GraphCompiler::NodeFacesMap faces;
    faces["B"] = std::vector<GraphCompiler::DocumentFacePort>{
        {"b_bool", "bool", "b_bool", "input"},
        {"b_dbl", "double", "b_dbl", "input"}};
    compiler.BindNodeFaces(&faces);

    GraphDocumentDto doc;
    doc.nodes.push_back(MakeComponentNode("A", "{ga}"));
    doc.nodes.push_back(MakeComponentNode("B", "{gb}"));
    doc.edges.push_back(MakeEdge("e_bad", "A", "out_int", "B", "b_bool"));
    doc.edges.push_back(MakeEdge("e_keep", "A", "out_int", "B", "b_dbl"));

    const auto review = compiler.ReviewStaleEdges(doc);
    ASSERT_EQ(review.stale_edges.size(), 1u);
    EXPECT_EQ(review.stale_edges[0].edge_id, "e_bad");
    EXPECT_EQ(
        review.stale_edges[0].reason,
        GraphCompiler::StaleEdgeRecord::Reason::TypeIncompatible);
    EXPECT_EQ(review.stale_edges[0].source_type, "int");
    EXPECT_EQ(review.stale_edges[0].target_type, "bool");

    ASSERT_EQ(review.diagnostics.size(), 1u);
    EXPECT_EQ(review.diagnostics[0].edge_id, "e_bad");
    EXPECT_EQ(review.diagnostics[0].actual_type, "int");
    EXPECT_EQ(review.diagnostics[0].expected_type, "bool");
}

// 文档 types[] 注册表兼容：自定义类型经 compatibleWith 声明保持；可判
// （注册×内建）而未声明配对闭世界断开；不可判（未注册非内建）维持
// permissive 不误删。
TEST(StaleEdgeReviewTest, RegistryCompatibleWithKeepsLinkAndClosesWorld)
{
    const std::unordered_map<
        std::string,
        std::pair<
            std::vector<std::pair<std::string, std::string>>,
            std::vector<std::pair<std::string, std::string>>>>
        surfaces{{"{ga}", {{}, {{"out_str", "string"}}}}};

    GraphCompiler compiler;
    compiler.SetPortSurfaceProvider(MakePortSurfaceProvider(surfaces));

    GraphCompiler::NodeFacesMap faces;
    faces["B"] = std::vector<GraphCompiler::DocumentFacePort>{
        {"my_in", "MyMode", "my_in", "input"},
        {"other_in", "OtherMode", "other_in", "input"},
        {"wild_in", "WildType", "wild_in", "input"}};
    compiler.BindNodeFaces(&faces);

    std::vector<GraphCompiler::DocumentFaceType> types;
    types.push_back(GraphCompiler::DocumentFaceType{"MyMode", {"string"}});
    types.push_back(GraphCompiler::DocumentFaceType{"OtherMode", {}});
    compiler.BindFaceTypes(&types);

    GraphDocumentDto doc;
    doc.nodes.push_back(MakeComponentNode("A", "{ga}"));
    doc.nodes.push_back(MakeComponentNode("B", "{gb}"));
    // e_link：MyMode 声明 compatibleWith ["string"]——string→MyMode 保持。
    doc.edges.push_back(MakeEdge("e_link", "A", "out_str", "B", "my_in"));
    // e_closed：OtherMode 注册但零声明——string→OtherMode 可判不配，断开。
    doc.edges.push_back(MakeEdge("e_closed", "A", "out_str", "B", "other_in"));
    // e_wild：WildType 未注册非内建——判不了，permissive 保持。
    doc.edges.push_back(MakeEdge("e_wild", "A", "out_str", "B", "wild_in"));

    const auto review = compiler.ReviewStaleEdges(doc);
    ASSERT_EQ(review.stale_edges.size(), 1u);
    EXPECT_EQ(review.stale_edges[0].edge_id, "e_closed");
    EXPECT_EQ(review.stale_edges[0].target_type, "OtherMode");
    EXPECT_EQ(review.stale_edges[0].source_type, "string");
}

// signal 边不受数据口复核影响：规范时序端点（signal_out/signal_in）不
// 在数据口面——若复核越域会按口缺失断开；复核域限数据边，signal 边
// 零产出。
TEST(StaleEdgeReviewTest, SignalEdgesOutsideReviewDomain)
{
    const std::unordered_map<
        std::string,
        std::pair<
            std::vector<std::pair<std::string, std::string>>,
            std::vector<std::pair<std::string, std::string>>>>
        surfaces{
            {"{ga}", {{}, {{"out_str", "string"}}}},
            {"{gb}", {{{"b_in", "string"}}, {}}}};

    GraphCompiler compiler;
    compiler.SetPortSurfaceProvider(MakePortSurfaceProvider(surfaces));

    GraphCompiler::NodeFacesMap faces;
    faces["B"] = std::vector<GraphCompiler::DocumentFacePort>{
        {"b_in", "string", "b_in", "input"}};
    compiler.BindNodeFaces(&faces);

    GraphDocumentDto doc;
    doc.nodes.push_back(MakeComponentNode("A", "{ga}"));
    doc.nodes.push_back(MakeComponentNode("B", "{gb}"));
    // 规范 signal 边：端点均不在数据口面（signal_out/signal_in 是时序
    // 坐标系，非数据口声明）。
    GraphEdgeDto signal_edge =
        MakeEdge("e_seq", "A", "signal_out", "B", "signal_in");
    signal_edge.edge_type = "signal";
    doc.edges.push_back(signal_edge);
    // 数据对照边：同节点对的数据边复核域内正常存活（口面双双在场且
    // string→string 兼容）。
    doc.edges.push_back(MakeEdge("e_data", "A", "out_str", "B", "b_in"));

    const auto review = compiler.ReviewStaleEdges(doc);
    EXPECT_TRUE(review.stale_edges.empty());
    EXPECT_TRUE(review.diagnostics.empty());
}

// Compile 只产诊断零变异：复核接线进 Compile 后，编译器本体对输入文档
// 零改动（edges/revision 逐项不变）——写回是会话 Apply 路径的行为（读
// 无副作用守卫的编译器侧断言）。
TEST(StaleEdgeReviewTest, CompileEmitsWarningsWithoutMutatingDocument)
{
    const std::unordered_map<
        std::string,
        std::pair<
            std::vector<std::pair<std::string, std::string>>,
            std::vector<std::pair<std::string, std::string>>>>
        surfaces{{"{ga}", {{}, {{"out_str", "string"}}}}};

    GraphCompiler compiler;
    compiler.SetPortSurfaceProvider(MakePortSurfaceProvider(surfaces));

    GraphCompiler::NodeFacesMap faces;
    faces["B"] = std::vector<GraphCompiler::DocumentFacePort>{
        {"b_bool", "bool", "b_bool", "input"}};
    compiler.BindNodeFaces(&faces);

    GraphDocumentDto doc;
    doc.nodes.push_back(MakeComponentNode("A", "{ga}"));
    doc.nodes.push_back(MakeComponentNode("B", "{gb}"));
    doc.edges.push_back(MakeEdge("e_bad", "A", "out_str", "B", "b_bool"));
    const GraphDocumentDto doc_before = doc;

    const auto plan = compiler.Compile(doc);

    // 断线诊断随编译输出可见（WARN 级 kind 进 plan.diagnostics）。
    bool seen_stale = false;
    for (const auto& diagnostic : plan.diagnostics)
    {
        auto diagnostic_obj = diagnostic.as_object();
        if (!diagnostic_obj.has_value()
            || !diagnostic_obj->contains(std::string_view("kind")))
        {
            continue;
        }
        if ((*diagnostic_obj)[std::string_view("kind")].as_string()
            == std::to_string(
                static_cast<int>(CompileDiagnosticKind::StaleEdgeDisconnect)))
        {
            seen_stale = true;
        }
    }
    EXPECT_TRUE(seen_stale);

    // 文档零变异：边集与 revision 逐项不变（修剪只发生在 Apply 语境）。
    ASSERT_EQ(doc.edges.size(), doc_before.edges.size());
    EXPECT_EQ(doc.edges[0].edge_id, doc_before.edges[0].edge_id);
    EXPECT_EQ(doc.version, doc_before.version);
}

// ===================================================================
// 引用闭包口面 pass（编译期只算口面）：绑条目节点 → creator 开被引条目会话
// → GetDocument 现算口面（产物投递归执行时编译段）。fake creator 走
// manager 后端注入（执行编译路径形状：SetFactoryManager +
// SetBackendResolvers）。
// ===================================================================
namespace
{
    constexpr std::string_view BOUND_GRAPH_TASK_GUID =
        "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";
    constexpr std::string_view BOUND_ENTRY_TASK_TYPE_GUID =
        "D5E6F7A8-9999-4000-8000-000000000042";
    constexpr int64_t BOUND_ENTRY_ID = 42;

    // 固定响应的 authoring 会话替身：GetDocument 返回固定文档（根 ports
    // 自述），Compile 返回固定产物（compile.executionInput 携 shell 串；
    // 调用计数供「编译期零 Compile 调用」负断言钉死）。
    class FakeReferenceAuthoringSession final
        : public Das::PluginInterface::IDasTaskAuthoringSession
    {
    public:
        FakeReferenceAuthoringSession(
            std::string document_json,
            std::string compile_json,
            int*        compile_call_counter = nullptr)
            : document_json_(std::move(document_json)),
              compile_json_(std::move(compile_json)),
              compile_call_counter_(compile_call_counter)
        {
        }

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DasIidOf<IDasBase>())
            {
                *pp_out = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid
                == DasIidOf<Das::PluginInterface::IDasTaskAuthoringSession>())
            {
                *pp_out = static_cast<
                    Das::PluginInterface::IDasTaskAuthoringSession*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult GetGuid(DasGuid* p_out_guid) override
        {
            if (p_out_guid == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_out_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                std::string{BOUND_ENTRY_TASK_TYPE_GUID});
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            if (pp_out_name == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            return CreateIDasReadOnlyStringFromUtf8(
                "FakeReferenceAuthoringSession",
                pp_out_name);
        }

        DasResult GetDocument(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_document_json) override
        {
            if (pp_out_document_json == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (document_json_.empty())
            {
                *pp_out_document_json = nullptr;
                return DAS_E_FAIL;
            }
            // c_str 显式走 const char* 解析构造（std::string 会落到
            // yyjson::value&& 重载构造出「字符串值」而非解析文档）。
            *pp_out_document_json =
                new Das::Core::Utils::IDasJsonImpl(document_json_.c_str());
            (*pp_out_document_json)->AddRef();
            return DAS_S_OK;
        }

        DasResult ApplyChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_result_json = nullptr;
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult Compile(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (compile_call_counter_ != nullptr)
            {
                ++*compile_call_counter_;
            }
            if (compile_json_.empty())
            {
                *pp_out_result_json = nullptr;
                return DAS_E_FAIL;
            }
            *pp_out_result_json =
                new Das::Core::Utils::IDasJsonImpl(compile_json_.c_str());
            (*pp_out_result_json)->AddRef();
            return DAS_S_OK;
        }

    private:
        std::string           document_json_;
        std::string           compile_json_;
        int*                  compile_call_counter_;
        std::atomic<uint32_t> ref_count_{0};
    };

    // 注入 pass 用例的公共夹具：manager 后端注入 fake 条目解析 + fake
    // 会话工厂；repository_fields 声明 repositoryRef 字段。
    struct ReferencePassFixture
    {
        StubFactoryManager mgr;
        // 会话工厂收到的 context 原文（闭包栈键经 context 传递的取证槽）。
        std::vector<std::string> seen_contexts;
        // 会话 Compile 调用计数（编译期零 Compile 负断言的取证槽）。
        int compile_calls = 0;

        ReferencePassFixture(
            std::string                           document_json,
            std::string                           compile_json,
            DasResult                             session_result = DAS_S_OK,
            std::optional<RepositoryEntrySummary> summary =
                []
            {
                RepositoryEntrySummary seeded;
                seeded.task_type_guid = std::string{BOUND_ENTRY_TASK_TYPE_GUID};
                seeded.accepted_properties = Das::Utils::MakeYyjsonObject();
                return seeded;
            }())
        {
            mgr.SetBackendResolvers(
                [summary](int64_t entry_id)
                {
                    if (!summary.has_value() || entry_id != BOUND_ENTRY_ID)
                    {
                        return std::optional<RepositoryEntrySummary>{};
                    }
                    return summary;
                },
                [this,
                 document_json = std::move(document_json),
                 compile_json = std::move(compile_json),
                 session_result](
                    const DasGuid&                  task_guid,
                    Das::ExportInterface::IDasJson* p_context_json,
                    Das::PluginInterface::IDasTaskAuthoringSession**
                        pp_out_session)
                {
                    if (DAS::IsFailed(session_result))
                    {
                        *pp_out_session = nullptr;
                        return session_result;
                    }
                    std::ignore = task_guid;
                    if (p_context_json != nullptr)
                    {
                        Das::DasPtr<IDasReadOnlyString> text;
                        if (DAS::IsOk(p_context_json->ToString(0, text.Put()))
                            && text.Get() != nullptr)
                        {
                            const char* utf8 = nullptr;
                            if (DAS::IsOk(text->GetUtf8(&utf8))
                                && utf8 != nullptr)
                            {
                                seen_contexts.emplace_back(utf8);
                            }
                        }
                    }
                    auto* session = new FakeReferenceAuthoringSession(
                        document_json,
                        compile_json,
                        &compile_calls);
                    session->AddRef();
                    *pp_out_session = session;
                    return DAS_S_OK;
                });
        }

        GraphCompiler MakeCompiler()
        {
            GraphCompiler compiler;
            compiler.SetFactoryManager(&mgr);
            compiler.BindRepositoryFields(
                {{std::string{BOUND_GRAPH_TASK_GUID}, {"repositoryRef"}}});
            return compiler;
        }

        GraphNodeDto MakeBoundNode(
            const std::string& node_id,
            std::string_view   field_value_json)
        {
            GraphNodeDto node =
                MakeComponentNode(node_id, std::string{BOUND_GRAPH_TASK_GUID});
            const auto settings = Das::Utils::ParseYyjsonFromString(
                std::string(R"({"repositoryRef":)")
                + std::string(field_value_json) + "}");
            if (settings.has_value())
            {
                node.settings = std::move(*settings);
            }
            return node;
        }
    };

    bool HasReferenceDiagnostic(
        const Dto::CompiledGraphPlanDto& plan,
        CompileDiagnosticKind            kind)
    {
        const std::string kind_text = std::to_string(static_cast<int>(kind));
        for (const auto& diag : plan.diagnostics)
        {
            auto obj = diag.as_object();
            if (!obj.has_value())
            {
                continue;
            }
            auto kind_value = (*obj)[std::string_view("kind")].as_string();
            if (kind_value.has_value() && *kind_value == kind_text)
            {
                return true;
            }
        }
        return false;
    }

    const PortBindingDto* FindBoundaryExecutionInputBinding(
        const Dto::CompiledGraphPlanDto& plan,
        const std::string&               target_node_id)
    {
        for (const auto& binding : plan.binding_plan.bindings)
        {
            if (binding.kind == Dto::PortBindingKind::Boundary
                && binding.target_node_id == target_node_id
                && binding.target_port_id == "executionInput")
            {
                return &binding;
            }
        }
        return nullptr;
    }
} // namespace

TEST(
    ReferenceClosureInjectionPassTest,
    RecordsBoundEntryAndDerivesPortsFromReferencedDocument)
{
    // 被引条目文档：根 ports 自述一个 int 输出口 doc_out。
    const std::string document_json = R"({
        "document": {
            "viewKind": "graph",
            "ports": [
                {"id": "doc_out", "type": "int", "direction": "output"}
            ]
        },
        "diagnostics": []
    })";
    const std::string compile_json = R"({"compile":{"executionInput":"x"}})";

    ReferencePassFixture fixture{document_json, compile_json};
    auto                 compiler = fixture.MakeCompiler();

    GraphDocumentDto doc;
    doc.document_id = "doc_ref_ok";
    doc.nodes.push_back(fixture.MakeBoundNode("n_bound", "42"));
    doc.nodes.push_back(MakeComponentNode("n_sink", "{gs}"));
    doc.edges.push_back(MakeEdge("e_1", "n_bound", "doc_out", "n_sink", "in"));

    const auto plan = compiler.Compile(doc);

    // ① 绑条目记档：reference_entries 携 {n_bound, 42}（执行时编译段的
    // 驱动数据）；编译产物零寄存——无任何 executionInput 目标绑定。
    ASSERT_EQ(plan.reference_entries.size(), 1u);
    EXPECT_EQ(plan.reference_entries[0].node_id, "n_bound");
    EXPECT_EQ(plan.reference_entries[0].entry_id, 42);
    EXPECT_EQ(FindBoundaryExecutionInputBinding(plan, "n_bound"), nullptr);

    // ② 编译期对绑条目节点零 Compile 调用（口面半段只 GetDocument——
    // 产物投递归执行时编译段）。
    EXPECT_EQ(fixture.compile_calls, 0);

    // ③ 节点口面 = 文档根 ports：doc_out 出口可供边校验（无 PortNotFound）
    EXPECT_FALSE(HasReferenceDiagnostic(
        plan,
        CompileDiagnosticKind::ReferenceDocumentMissing));
    bool port_not_found = false;
    for (const auto& diag : plan.diagnostics)
    {
        auto obj = diag.as_object();
        if (obj.has_value()
            && (*obj)[std::string_view("kind")].as_string()
                   == std::to_string(
                       static_cast<int>(CompileDiagnosticKind::PortNotFound)))
        {
            port_not_found = true;
        }
    }
    EXPECT_FALSE(port_not_found)
        << "doc-declared port must feed edge validation";

    // 成功路径：execution_order 保留
    EXPECT_FALSE(plan.execution_order.empty());
}

TEST(
    ReferenceClosureInjectionPassTest,
    MissingDocumentYieldsNamedDiagnosticAndClearsExecutionOrder)
{
    // GetDocument 失败（空文档响应）→ 具名诊断 + execution_order 清空
    ReferencePassFixture fixture{"", R"({"compile":{"executionInput":"x"}})"};
    auto                 compiler = fixture.MakeCompiler();

    GraphDocumentDto doc;
    doc.document_id = "doc_ref_no_doc";
    doc.nodes.push_back(fixture.MakeBoundNode("n_bound", "42"));

    const auto plan = compiler.Compile(doc);
    EXPECT_TRUE(HasReferenceDiagnostic(
        plan,
        CompileDiagnosticKind::ReferenceDocumentMissing));
    EXPECT_TRUE(plan.execution_order.empty());
    EXPECT_EQ(FindBoundaryExecutionInputBinding(plan, "n_bound"), nullptr);
}

TEST(
    ReferenceClosureInjectionPassTest,
    EmptyRepositoryRefValueYieldsNamedDiagnostic)
{
    // catalog 声明了 taskRepositoryRef 字段但值非正整数（Pitfall 3 近根因
    // 诊断）
    ReferencePassFixture fixture{
        R"({"document":{"ports":[]}})",
        R"({"compile":{"executionInput":"x"}})"};
    auto compiler = fixture.MakeCompiler();

    GraphDocumentDto doc;
    doc.document_id = "doc_ref_empty_value";
    doc.nodes.push_back(fixture.MakeBoundNode("n_bound", "0"));

    const auto plan = compiler.Compile(doc);
    EXPECT_TRUE(HasReferenceDiagnostic(
        plan,
        CompileDiagnosticKind::ReferenceEntryValueEmpty));
    EXPECT_TRUE(plan.execution_order.empty());
}

TEST(
    ReferenceClosureInjectionPassTest,
    SameDocumentRepeatedEntryIdRecordsBothNodes)
{
    // 同层复用 DFS 弹栈语义：同文档两节点绑同一条目是合法复用形态
    // （非环）——前一节点处理完弹栈，两个节点各自独立走口面取数链。
    ReferencePassFixture fixture{
        R"({"document":{"ports":[]}})",
        R"({"compile":{"executionInput":"x"}})"};
    auto compiler = fixture.MakeCompiler();

    GraphDocumentDto doc;
    doc.document_id = "doc_ref_same_entry_reuse";
    doc.nodes.push_back(fixture.MakeBoundNode("n_bound_a", "42"));
    doc.nodes.push_back(fixture.MakeBoundNode("n_bound_b", "42"));

    const auto plan = compiler.Compile(doc);
    EXPECT_FALSE(HasReferenceDiagnostic(
        plan,
        CompileDiagnosticKind::ReferenceCycleDetected));
    EXPECT_FALSE(plan.execution_order.empty());
    ASSERT_NE(
        std::find(
            plan.execution_order.begin(),
            plan.execution_order.end(),
            "n_bound_a"),
        plan.execution_order.end());
    ASSERT_NE(
        std::find(
            plan.execution_order.begin(),
            plan.execution_order.end(),
            "n_bound_b"),
        plan.execution_order.end());

    // 两个节点各自记档（执行时编译段照单各自现编），产物零寄存。
    ASSERT_EQ(plan.reference_entries.size(), 2u);
    EXPECT_EQ(plan.reference_entries[0].node_id, "n_bound_a");
    EXPECT_EQ(plan.reference_entries[0].entry_id, 42);
    EXPECT_EQ(plan.reference_entries[1].node_id, "n_bound_b");
    EXPECT_EQ(plan.reference_entries[1].entry_id, 42);
    EXPECT_EQ(FindBoundaryExecutionInputBinding(plan, "n_bound_a"), nullptr);
    EXPECT_EQ(FindBoundaryExecutionInputBinding(plan, "n_bound_b"), nullptr);

    // 两个节点各开一次子会话（同层复用各自独立取口面），编译期零
    // Compile 调用。
    EXPECT_EQ(fixture.seen_contexts.size(), 2u);
    EXPECT_EQ(fixture.compile_calls, 0);
}

TEST(
    ReferenceClosureInjectionPassTest,
    FailedNodeLeavesNoStackResidueForSiblingDiagnostics)
{
    // 失败节点的 entryId 不残留编译栈：creator 缺失的失败分支先于压栈，
    // 同文档后续兄弟节点绑同一条目得到真根因诊断（creator 缺失）而非
    // 误导性环错。
    ReferencePassFixture fixture{
        R"({"document":{"ports":[]}})",
        R"({"compile":{"executionInput":"x"}})"};

    GraphDocumentDto doc;
    doc.document_id = "doc_ref_no_residue";
    doc.nodes.push_back(fixture.MakeBoundNode("n_bound_a", "42"));
    doc.nodes.push_back(fixture.MakeBoundNode("n_bound_b", "42"));

    // 无 creator 装配（既无显式绑定又无 factory manager host）。
    GraphCompiler compiler;
    compiler.BindRepositoryFields(
        {{std::string{BOUND_GRAPH_TASK_GUID}, {"repositoryRef"}}});

    const auto plan = compiler.Compile(doc);
    EXPECT_FALSE(HasReferenceDiagnostic(
        plan,
        CompileDiagnosticKind::ReferenceCycleDetected));
    // 两个节点各自得到真根因诊断（creator 缺失），后者不被前者的失败
    // 污染为环错。
    int        creator_missing_count = 0;
    const auto kind_text = std::to_string(
        static_cast<int>(CompileDiagnosticKind::ReferenceCreatorMissing));
    for (const auto& diag : plan.diagnostics)
    {
        auto obj = diag.as_object();
        if (obj.has_value()
            && (*obj)[std::string_view("kind")].as_string() == kind_text)
        {
            ++creator_missing_count;
        }
    }
    EXPECT_EQ(creator_missing_count, 2);
    EXPECT_TRUE(plan.execution_order.empty());
}

TEST(
    ReferenceClosureInjectionPassTest,
    ContextEncodeFailureYieldsNamedDiagnostic)
{
    // context 序列化失败 → 具名诊断：条目 summary 携带非 finite 实数
    // （JSON 文本不可能表达的值形态），clone 后进入 context，编码器在
    // NoFlag 写出时拒绝——ReferenceContextEncodeFailed + 失败出口语义。
    // 同族 ReferenceContextWrapFailed（IDasJson 包装构造抛异常）不可经
    // 数据面强制触发：包装构造只做值拷贝与分配，可抛面仅剩内存耗尽，
    // 不造假测试（该分支与既有会话失败分支模板同构）。
    RepositoryEntrySummary poisoned;
    poisoned.task_type_guid = std::string{BOUND_ENTRY_TASK_TYPE_GUID};
    poisoned.accepted_properties =
        yyjson::value(std::numeric_limits<double>::quiet_NaN());
    ReferencePassFixture fixture{
        R"({"document":{"ports":[]}})",
        R"({"compile":{"executionInput":"x"}})",
        DAS_S_OK,
        poisoned};
    auto compiler = fixture.MakeCompiler();

    GraphDocumentDto doc;
    doc.document_id = "doc_ref_ctx_encode_fail";
    doc.nodes.push_back(fixture.MakeBoundNode("n_bound", "42"));

    const auto plan = compiler.Compile(doc);
    EXPECT_TRUE(HasReferenceDiagnostic(
        plan,
        CompileDiagnosticKind::ReferenceContextEncodeFailed));
    EXPECT_TRUE(plan.execution_order.empty());
    EXPECT_EQ(FindBoundaryExecutionInputBinding(plan, "n_bound"), nullptr);
}

TEST(
    ReferenceClosureInjectionPassTest,
    FailedPassKeepsBindingPlanIdenticalToNoInjectionBaseline)
{
    // 硬不变量：pass 失败 ⇒ binding_plan 与无注入 pass 完全一致——零
    // 派生 Boundary 删除。用可强制的失败（fake creator 会话开启失败）驱动；
    // 节点声明 executionInput 输入口使基线携带派生悬空 Boundary（失败态
    // 必须原样保留）。
    ReferencePassFixture fixture{
        R"({"document":{"ports":[]}})",
        R"({"compile":{"executionInput":"x"}})",
        DAS_E_FAIL};
    fixture.mgr.AddDefinition(
        std::string{BOUND_GRAPH_TASK_GUID},
        MakeDefinition({{"executionInput", "string"}}, {}));
    auto compiler = fixture.MakeCompiler();

    GraphDocumentDto doc;
    doc.document_id = "doc_ref_failed_zero_injection";
    doc.nodes.push_back(fixture.MakeBoundNode("n_bound", "42"));

    const auto plan = compiler.Compile(doc);
    EXPECT_TRUE(HasReferenceDiagnostic(
        plan,
        CompileDiagnosticKind::ReferenceSessionFailed));
    EXPECT_TRUE(plan.execution_order.empty());

    // 基线：同一编译器装配但不声明仓库字段（pass 对本文档无绑条目节点
    // 可见，等价无注入 pass）；派生悬空 Boundary 在场，失败态原样保留。
    GraphCompiler baseline;
    baseline.SetFactoryManager(&fixture.mgr);
    const auto baseline_plan = baseline.Compile(doc);
    ASSERT_NE(
        FindBoundaryExecutionInputBinding(baseline_plan, "n_bound"),
        nullptr)
        << "baseline must carry the derived dangling-input Boundary";

    ASSERT_EQ(
        plan.binding_plan.bindings.size(),
        baseline_plan.binding_plan.bindings.size());
    for (std::size_t i = 0; i < plan.binding_plan.bindings.size(); ++i)
    {
        const auto& actual = plan.binding_plan.bindings[i];
        const auto& expected = baseline_plan.binding_plan.bindings[i];
        EXPECT_EQ(actual.kind, expected.kind);
        EXPECT_EQ(actual.source_node_id, expected.source_node_id);
        EXPECT_EQ(actual.source_port_id, expected.source_port_id);
        EXPECT_EQ(actual.target_node_id, expected.target_node_id);
        EXPECT_EQ(actual.target_port_id, expected.target_port_id);
        EXPECT_EQ(actual.expected_type, expected.expected_type);
        EXPECT_EQ(actual.target_type, expected.target_type);
        EXPECT_EQ(actual.default_value.write(), expected.default_value.write());
    }
    EXPECT_NE(FindBoundaryExecutionInputBinding(plan, "n_bound"), nullptr)
        << "failed pass must keep the derived Boundary (no erasure)";
}

TEST(
    ReferenceClosureInjectionPassTest,
    AncestorEntryIdOnClosureStackYieldsCycleWithPath)
{
    // 深层文档绑回祖先 entryId：编译器继承闭包栈（子会话编译路径经
    // context 恢复的祖先链 [41, 42]），本文档节点再绑 42 → 栈上重现 =
    // 具名环错，诊断含环路径序列（41 -> 42 -> 42）。
    ReferencePassFixture fixture{
        R"({"document":{"ports":[]}})",
        R"({"compile":{"executionInput":"x"}})"};
    auto compiler = fixture.MakeCompiler();
    compiler.BindClosureEntryIds({41, 42});

    GraphDocumentDto doc;
    doc.document_id = "doc_ref_ancestor_cycle";
    doc.nodes.push_back(fixture.MakeBoundNode("n_bound", "42"));

    const auto plan = compiler.Compile(doc);
    EXPECT_TRUE(HasReferenceDiagnostic(
        plan,
        CompileDiagnosticKind::ReferenceCycleDetected));
    EXPECT_TRUE(plan.execution_order.empty());

    // 环路径进诊断 message：祖先链 + 重现 entryId 的完整序列。
    bool path_seen = false;
    for (const auto& diag : plan.diagnostics)
    {
        auto obj = diag.as_object();
        if (!obj.has_value())
        {
            continue;
        }
        auto kind_value = (*obj)[std::string_view("kind")].as_string();
        if (!kind_value.has_value()
            || *kind_value
                   != std::to_string(
                       static_cast<int>(
                           CompileDiagnosticKind::ReferenceCycleDetected)))
        {
            continue;
        }
        auto message = (*obj)[std::string_view("message")].as_string();
        if (message.has_value()
            && message->find("41 -> 42 -> 42") != std::string::npos)
        {
            path_seen = true;
        }
    }
    EXPECT_TRUE(path_seen) << "cycle diagnostic must carry the path sequence";
}

TEST(
    ReferenceClosureInjectionPassTest,
    PassCarriesClosureStackIntoSessionContext)
{
    // 父 pass 开子会话时把当前栈（含本层 entryId）写进 CreateAuthoringSession
    // 的 context（闭包栈键）——子会话编译路径从 context 恢复栈继续压栈。
    ReferencePassFixture fixture{
        R"({"document":{"ports":[]}})",
        R"({"compile":{"executionInput":"x"}})"};
    auto compiler = fixture.MakeCompiler();
    compiler.BindClosureEntryIds({41});

    GraphDocumentDto doc;
    doc.document_id = "doc_ref_stack_context";
    doc.nodes.push_back(fixture.MakeBoundNode("n_bound", "42"));

    const auto plan = compiler.Compile(doc);
    ASSERT_FALSE(plan.execution_order.empty());
    ASSERT_FALSE(fixture.seen_contexts.empty());
    // context = 继承栈 [41] + 本层 42；键名与数组形状锁死（结构化断言，
    // 不依赖序列化空白形态）。
    const auto& context = fixture.seen_contexts.back();
    auto        parsed_context = yyjson::read(context);
    auto        context_obj = parsed_context.as_object();
    ASSERT_TRUE(context_obj.has_value()) << context;
    ASSERT_TRUE(context_obj->contains(std::string_view("closureEntryIds")))
        << context;
    auto closure_ids =
        (*context_obj)[std::string_view("closureEntryIds")].as_array();
    ASSERT_TRUE(closure_ids.has_value()) << context;
    ASSERT_EQ(closure_ids->size(), 2u) << context;
    EXPECT_EQ((*closure_ids)[0].as_int().value_or(0), 41) << context;
    EXPECT_EQ((*closure_ids)[1].as_int().value_or(0), 42) << context;
    EXPECT_EQ(
        (*context_obj)[std::string_view("repositoryEntryId")].as_int(),
        42);
}
