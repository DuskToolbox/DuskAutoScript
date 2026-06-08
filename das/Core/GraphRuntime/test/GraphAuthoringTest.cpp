#include <das/Core/GraphRuntime/GraphAuthoring.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace
{
    using namespace Das::Core::GraphRuntime;
    using namespace Das::Core::GraphRuntime::Dto;

    // -----------------------------------------------------------------------
    // Test helpers
    // -----------------------------------------------------------------------

    yyjson::value MakeSettings(const std::string& key, double value)
    {
        auto payload = Das::Utils::MakeYyjsonObject();
        auto obj = *payload.as_object();
        obj[std::string_view(key)] = value;
        return payload;
    }

    GraphNodeDto MakeNode(const std::string& node_id)
    {
        GraphNodeDto node;
        node.node_id = node_id;
        node.target.target_kind = "componentRef";
        node.target.component_ref =
            ComponentRefDto{"componentRef", "{comp-guid}", "{plug-guid}"};
        node.settings = MakeSettings("threshold", 0.5);
        return node;
    }

    GraphEdgeDto MakeEdge(
        const std::string& edge_id,
        const std::string& src_node,
        const std::string& src_port,
        const std::string& tgt_node,
        const std::string& tgt_port)
    {
        return GraphEdgeDto{edge_id, src_node, src_port, tgt_node, tgt_port};
    }

    GraphDocumentDto MakeEmptyDocument()
    {
        GraphDocumentDto doc;
        doc.document_id = "test_graph";
        doc.version = 1;
        doc.fingerprint = "sha256:test";
        return doc;
    }

    GraphDocumentDto MakeDocumentWithTwoNodes()
    {
        auto doc = MakeEmptyDocument();
        doc.nodes.push_back(MakeNode("node_1"));
        doc.nodes.push_back(MakeNode("node_2"));
        return doc;
    }

} // anonymous namespace

// ===========================================================================
// AddNodeChange tests
// ===========================================================================

TEST(GraphAuthoringTest, AddNodeSuccess)
{
    auto doc = MakeEmptyDocument();
    auto node = MakeNode("new_node");

    GraphAuthoringChange change{AddNodeChange{std::move(node)}};

    auto result = ApplySettingsChange(doc, change);
    EXPECT_TRUE(result.Ok());
    ASSERT_EQ(doc.nodes.size(), 1u);
    EXPECT_EQ(doc.nodes[0].node_id, "new_node");
}

TEST(GraphAuthoringTest, AddNodeRejectsEmptyNodeId)
{
    auto doc = MakeEmptyDocument();
    auto node = MakeNode("");

    GraphAuthoringChange change{AddNodeChange{std::move(node)}};
    auto                 result = ApplySettingsChange(doc, change);

    EXPECT_FALSE(result.Ok());
    EXPECT_EQ(result.error_kind, AuthoringErrorKind::EmptyNodeId);
    EXPECT_TRUE(doc.nodes.empty());
}

TEST(GraphAuthoringTest, AddNodeRejectsDuplicateNodeId)
{
    auto doc = MakeDocumentWithTwoNodes();

    auto                 node = MakeNode("node_1"); // already exists
    GraphAuthoringChange change{AddNodeChange{std::move(node)}};
    auto                 result = ApplySettingsChange(doc, change);

    EXPECT_FALSE(result.Ok());
    EXPECT_EQ(result.error_kind, AuthoringErrorKind::DuplicateNodeId);
    ASSERT_EQ(doc.nodes.size(), 2u); // document unchanged
}

// ===========================================================================
// RemoveNodeChange tests
// ===========================================================================

TEST(GraphAuthoringTest, RemoveNodeSuccess)
{
    auto doc = MakeDocumentWithTwoNodes();
    // Add an edge connected to node_1
    doc.edges.push_back(MakeEdge("edge_1", "node_1", "out", "node_2", "in"));

    GraphAuthoringChange change{RemoveNodeChange{"node_1"}};
    auto                 result = ApplySettingsChange(doc, change);

    EXPECT_TRUE(result.Ok());
    ASSERT_EQ(doc.nodes.size(), 1u);
    EXPECT_EQ(doc.nodes[0].node_id, "node_2");
    // Edges should cascade-delete
    EXPECT_TRUE(doc.edges.empty());
}

TEST(GraphAuthoringTest, RemoveNodeRejectsEmptyNodeId)
{
    auto doc = MakeDocumentWithTwoNodes();

    GraphAuthoringChange change{RemoveNodeChange{""}};
    auto                 result = ApplySettingsChange(doc, change);

    EXPECT_FALSE(result.Ok());
    EXPECT_EQ(result.error_kind, AuthoringErrorKind::EmptyNodeId);
    ASSERT_EQ(doc.nodes.size(), 2u);
}

TEST(GraphAuthoringTest, RemoveNodeRejectsNonExistentNodeId)
{
    auto doc = MakeDocumentWithTwoNodes();

    GraphAuthoringChange change{RemoveNodeChange{"nonexistent"}};
    auto                 result = ApplySettingsChange(doc, change);

    EXPECT_FALSE(result.Ok());
    EXPECT_EQ(result.error_kind, AuthoringErrorKind::NodeNotFound);
    ASSERT_EQ(doc.nodes.size(), 2u);
}

TEST(GraphAuthoringTest, RemoveNodeCascadesMultipleEdges)
{
    auto doc = MakeDocumentWithTwoNodes();
    doc.edges.push_back(MakeEdge("e1", "node_1", "out1", "node_2", "in1"));
    doc.edges.push_back(MakeEdge("e2", "node_2", "out1", "node_1", "in1"));
    doc.edges.push_back(MakeEdge("e3", "node_2", "out2", "node_1", "in2"));

    GraphAuthoringChange change{RemoveNodeChange{"node_1"}};
    auto                 result = ApplySettingsChange(doc, change);

    EXPECT_TRUE(result.Ok());
    // Only edge e1 remains (node_2 -> node_2 was not in the list)
    // Actually: e1 (1->2), e2 (2->1), e3 (2->1) — all reference node_1
    EXPECT_TRUE(doc.edges.empty());
}

// ===========================================================================
// ConnectPortsChange tests
// ===========================================================================

TEST(GraphAuthoringTest, ConnectPortsSuccess)
{
    auto doc = MakeDocumentWithTwoNodes();

    auto edge = MakeEdge("edge_1", "node_1", "out", "node_2", "in");
    GraphAuthoringChange change{ConnectPortsChange{std::move(edge)}};
    auto                 result = ApplySettingsChange(doc, change);

    EXPECT_TRUE(result.Ok());
    ASSERT_EQ(doc.edges.size(), 1u);
    EXPECT_EQ(doc.edges[0].edge_id, "edge_1");
}

TEST(GraphAuthoringTest, ConnectPortsRejectsEmptyEdgeId)
{
    auto doc = MakeDocumentWithTwoNodes();

    auto                 edge = MakeEdge("", "node_1", "out", "node_2", "in");
    GraphAuthoringChange change{ConnectPortsChange{std::move(edge)}};
    auto                 result = ApplySettingsChange(doc, change);

    EXPECT_FALSE(result.Ok());
    EXPECT_EQ(result.error_kind, AuthoringErrorKind::EmptyEdgeId);
}

TEST(GraphAuthoringTest, ConnectPortsRejectsDuplicateEdgeId)
{
    auto doc = MakeDocumentWithTwoNodes();
    doc.edges.push_back(MakeEdge("edge_1", "node_1", "out", "node_2", "in"));

    auto edge = MakeEdge("edge_1", "node_1", "out2", "node_2", "in2");
    GraphAuthoringChange change{ConnectPortsChange{std::move(edge)}};
    auto                 result = ApplySettingsChange(doc, change);

    EXPECT_FALSE(result.Ok());
    EXPECT_EQ(result.error_kind, AuthoringErrorKind::DuplicateEdgeId);
    ASSERT_EQ(doc.edges.size(), 1u); // unchanged
}

TEST(GraphAuthoringTest, ConnectPortsRejectsMissingSourceNode)
{
    auto doc = MakeDocumentWithTwoNodes();

    auto edge = MakeEdge("edge_1", "nonexistent", "out", "node_2", "in");
    GraphAuthoringChange change{ConnectPortsChange{std::move(edge)}};
    auto                 result = ApplySettingsChange(doc, change);

    EXPECT_FALSE(result.Ok());
    EXPECT_EQ(result.error_kind, AuthoringErrorKind::InvalidEdge);
    EXPECT_TRUE(doc.edges.empty());
}

TEST(GraphAuthoringTest, ConnectPortsRejectsMissingTargetNode)
{
    auto doc = MakeDocumentWithTwoNodes();

    auto edge = MakeEdge("edge_1", "node_1", "out", "nonexistent", "in");
    GraphAuthoringChange change{ConnectPortsChange{std::move(edge)}};
    auto                 result = ApplySettingsChange(doc, change);

    EXPECT_FALSE(result.Ok());
    EXPECT_EQ(result.error_kind, AuthoringErrorKind::InvalidEdge);
    EXPECT_TRUE(doc.edges.empty());
}

// ===========================================================================
// DisconnectPortsChange tests
// ===========================================================================

TEST(GraphAuthoringTest, DisconnectPortsSuccess)
{
    auto doc = MakeDocumentWithTwoNodes();
    doc.edges.push_back(MakeEdge("edge_1", "node_1", "out", "node_2", "in"));

    GraphAuthoringChange change{DisconnectPortsChange{"edge_1"}};
    auto                 result = ApplySettingsChange(doc, change);

    EXPECT_TRUE(result.Ok());
    EXPECT_TRUE(doc.edges.empty());
}

TEST(GraphAuthoringTest, DisconnectPortsRejectsEmptyEdgeId)
{
    auto doc = MakeDocumentWithTwoNodes();

    GraphAuthoringChange change{DisconnectPortsChange{""}};
    auto                 result = ApplySettingsChange(doc, change);

    EXPECT_FALSE(result.Ok());
    EXPECT_EQ(result.error_kind, AuthoringErrorKind::EmptyEdgeId);
}

TEST(GraphAuthoringTest, DisconnectPortsRejectsNonExistentEdge)
{
    auto doc = MakeDocumentWithTwoNodes();
    doc.edges.push_back(MakeEdge("edge_1", "node_1", "out", "node_2", "in"));

    GraphAuthoringChange change{DisconnectPortsChange{"nonexistent"}};
    auto                 result = ApplySettingsChange(doc, change);

    EXPECT_FALSE(result.Ok());
    EXPECT_EQ(result.error_kind, AuthoringErrorKind::EdgeNotFound);
    ASSERT_EQ(doc.edges.size(), 1u);
}

// ===========================================================================
// UpdateNodeConfigChange tests
// ===========================================================================

TEST(GraphAuthoringTest, UpdateNodeConfigSuccess)
{
    auto doc = MakeDocumentWithTwoNodes();

    auto                 new_settings = MakeSettings("threshold", 0.9);
    GraphAuthoringChange change{
        UpdateNodeConfigChange{"node_1", std::move(new_settings)}};
    auto result = ApplySettingsChange(doc, change);

    EXPECT_TRUE(result.Ok());
    // Verify settings changed — serialize and re-read to inspect value
    auto json_str = std::string(doc.nodes[0].settings.write());
    EXPECT_TRUE(json_str.find("\"threshold\"") != std::string::npos)
        << "Settings should contain 'threshold' key, got: " << json_str;
    EXPECT_TRUE(json_str.find("0.9") != std::string::npos)
        << "Settings should contain value 0.9, got: " << json_str;
}

TEST(GraphAuthoringTest, UpdateNodeConfigRejectsEmptyNodeId)
{
    auto doc = MakeDocumentWithTwoNodes();

    GraphAuthoringChange change{
        UpdateNodeConfigChange{"", MakeSettings("x", 1.0)}};
    auto result = ApplySettingsChange(doc, change);

    EXPECT_FALSE(result.Ok());
    EXPECT_EQ(result.error_kind, AuthoringErrorKind::EmptyNodeId);
}

TEST(GraphAuthoringTest, UpdateNodeConfigRejectsNonExistentNode)
{
    auto doc = MakeDocumentWithTwoNodes();

    GraphAuthoringChange change{
        UpdateNodeConfigChange{"nonexistent", MakeSettings("x", 1.0)}};
    auto result = ApplySettingsChange(doc, change);

    EXPECT_FALSE(result.Ok());
    EXPECT_EQ(result.error_kind, AuthoringErrorKind::NodeNotFound);
}

// ===========================================================================
// Composite scenario: add node → connect → update → disconnect → remove
// ===========================================================================

TEST(GraphAuthoringTest, CompositeScenario)
{
    auto doc = MakeEmptyDocument();

    // 1. Add node_1
    {
        GraphAuthoringChange change{AddNodeChange{MakeNode("node_1")}};
        EXPECT_TRUE(ApplySettingsChange(doc, change).Ok());
    }

    // 2. Add node_2
    {
        GraphAuthoringChange change{AddNodeChange{MakeNode("node_2")}};
        EXPECT_TRUE(ApplySettingsChange(doc, change).Ok());
    }

    ASSERT_EQ(doc.nodes.size(), 2u);
    EXPECT_TRUE(doc.edges.empty());

    // 3. Connect node_1 -> node_2
    {
        auto edge = MakeEdge("e1", "node_1", "output", "node_2", "input");
        GraphAuthoringChange change{ConnectPortsChange{std::move(edge)}};
        EXPECT_TRUE(ApplySettingsChange(doc, change).Ok());
    }
    ASSERT_EQ(doc.edges.size(), 1u);

    // 4. Update node_1 settings
    {
        GraphAuthoringChange change{
            UpdateNodeConfigChange{"node_1", MakeSettings("value", 42.0)}};
        EXPECT_TRUE(ApplySettingsChange(doc, change).Ok());
    }

    // 5. Disconnect
    {
        GraphAuthoringChange change{DisconnectPortsChange{"e1"}};
        EXPECT_TRUE(ApplySettingsChange(doc, change).Ok());
    }
    EXPECT_TRUE(doc.edges.empty());

    // 6. Remove node_1
    {
        GraphAuthoringChange change{RemoveNodeChange{"node_1"}};
        EXPECT_TRUE(ApplySettingsChange(doc, change).Ok());
    }
    ASSERT_EQ(doc.nodes.size(), 1u);
    EXPECT_EQ(doc.nodes[0].node_id, "node_2");
}
