#include <das/Core/GraphRuntime/AuthoringDocContract.h>
#include <das/Core/GraphRuntime/FormSequenceProjector.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace
{
    using namespace Das::Core::GraphRuntime;
    using namespace Das::Core::GraphRuntime::Contract;
    using namespace Das::Core::GraphRuntime::Dto;

    // -----------------------------------------------------------------------
    // Builders
    // -----------------------------------------------------------------------

    yyjson::value MakeSettings(double v)
    {
        auto payload = Das::Utils::MakeYyjsonObject();
        auto obj     = *payload.as_object();
        obj[std::string_view("threshold")] = v;
        return payload;
    }

    ComponentRefDto MakeCompRef(std::string_view guid)
    {
        return ComponentRefDto{
            std::string("componentRef"),
            std::string(guid),
            std::string("{plug-guid}")};
    }

    GraphNodeDto MakeNode(
        std::string_view id,
        std::string_view guid,
        double           settings_val)
    {
        GraphNodeDto node;
        node.node_id = std::string(id);
        node.target.target_kind   = "componentRef";
        node.target.component_ref = MakeCompRef(guid);
        node.settings             = MakeSettings(settings_val);
        return node;
    }

    GraphEdgeDto MakeSignalEdge(
        std::string_view id,
        std::string_view from,
        std::string_view to)
    {
        GraphEdgeDto edge;
        edge.edge_id        = std::string(id);
        edge.source_node_id = std::string(from);
        edge.source_port_id = std::string(FormSequenceProjector::SignalOutPort);
        edge.target_node_id = std::string(to);
        edge.target_port_id = std::string(FormSequenceProjector::SignalInPort);
        edge.edge_type      = "signal";
        return edge;
    }

    /// Serialize the authoritative store directly into contract JSON.
    std::string ToJson(const Dto::GraphDocumentDto& doc)
    {
        auto v   = Contract::SerializeDocument(doc);
        auto str = Das::Utils::SerializeYyjsonValue(v);
        return str.value_or(std::string{});
    }

    // -----------------------------------------------------------------------
    // ToAuthoringDocument — graph mode (no linear tag)
    // -----------------------------------------------------------------------

    TEST(AuthoringDocContractTest, GraphMode_ThinMapsNodesAndDropsPrivateFields)
    {
        GraphDocumentDto doc;
        doc.version     = 7;
        doc.fingerprint = "fp";
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.1));
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.2));
        doc.edges.push_back(MakeSignalEdge("e1", "n1", "n2"));

        auto out = ToAuthoringDocument(doc);
        ASSERT_EQ(out.kind, Kind::Graph);
        ASSERT_TRUE(out.graph.has_value());
        ASSERT_FALSE(out.form_sequence.has_value());

        const auto& view = *out.graph;
        ASSERT_EQ(view.nodes.size(), 2u);
        EXPECT_EQ(view.nodes[0].id, "n1");
        EXPECT_EQ(view.nodes[0].component_guid, "{g1}");
        ASSERT_EQ(view.connections.size(), 1u);
        EXPECT_EQ(view.connections[0].from_node_id, "n1");
        EXPECT_EQ(view.connections[0].to_node_id, "n2");
    }

    TEST(AuthoringDocContractTest, GraphMode_SerializeHasNoEdgeTypeOrEdgeId)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        // A non-signal (data) edge to prove edge_type is stripped regardless.
        GraphEdgeDto edge =
            MakeSignalEdge("private-edge", "n1", "n2");
        edge.edge_type = "data";
        doc.edges.push_back(edge);

        auto json = ToJson(doc);
        EXPECT_EQ(json.find("edgeType"), std::string::npos);
        EXPECT_EQ(json.find("edgeId"), std::string::npos);
        EXPECT_EQ(json.find("dynamicPorts"), std::string::npos);
        EXPECT_NE(json.find("\"graph\""), std::string::npos);
        EXPECT_NE(json.find("\"connections\""), std::string::npos);
    }

    // -----------------------------------------------------------------------
    // ToAuthoringDocument — formSequence mode (linear tag)
    // -----------------------------------------------------------------------

    TEST(AuthoringDocContractTest, FormSequenceMode_ReverseProjectsLinearChain)
    {
        // Build via the projector so the structure matches production.
        FormSequenceDto seq;
        seq.version     = 3;
        seq.fingerprint = "seq-fp";
        seq.items.push_back({.item_id = "a",
                             .target  = {.target_kind = "componentRef",
                                        .component_ref = MakeCompRef("{ga}")},
                             .settings = MakeSettings(1.0)});
        seq.items.push_back({.item_id = "b",
                             .target  = {.target_kind = "componentRef",
                                        .component_ref = MakeCompRef("{gb}")},
                             .settings = MakeSettings(2.0)});
        seq.items.push_back({.item_id = "c",
                             .target  = {.target_kind = "componentRef",
                                        .component_ref = MakeCompRef("{gc}")},
                             .settings = MakeSettings(3.0)});

        auto doc = FormSequenceProjector::Project(seq);
        doc.tags.emplace_back(kLinearTag); // mark linear state

        auto out = ToAuthoringDocument(doc);
        ASSERT_EQ(out.kind, Kind::FormSequence);
        ASSERT_TRUE(out.form_sequence.has_value());
        ASSERT_FALSE(out.graph.has_value());

        const auto& items = out.form_sequence->sequence;
        ASSERT_EQ(items.size(), 3u);
        EXPECT_EQ(items[0].id, "a");
        EXPECT_EQ(items[0].type, "{ga}");
        EXPECT_EQ(items[1].id, "b");
        EXPECT_EQ(items[2].id, "c");
    }

    TEST(AuthoringDocContractTest, FormSequenceMode_SerializeShape)
    {
        GraphDocumentDto doc;
        doc.version = 1;
        doc.nodes.push_back(MakeNode("x", "{gx}", 9.0));
        doc.tags.emplace_back(kLinearTag);

        auto json = ToJson(doc);
        EXPECT_NE(json.find("\"kind\":\"formSequence\""), std::string::npos);
        EXPECT_NE(json.find("\"formSequence\""), std::string::npos);
        EXPECT_NE(json.find("\"sequence\""), std::string::npos);
        // Private fields must not leak even in formSequence mode.
        EXPECT_EQ(json.find("edgeType"), std::string::npos);
    }

    // -----------------------------------------------------------------------
    // Upgrade / downgrade — lossless on the store
    // -----------------------------------------------------------------------

    TEST(AuthoringDocContractTest, UpgradeToGraph_RemovesTagAndIsLossless)
    {
        FormSequenceDto seq;
        seq.items.push_back({.item_id = "a",
                             .target  = {.target_kind = "componentRef"}});
        seq.items.push_back({.item_id = "b",
                             .target  = {.target_kind = "componentRef"}});
        auto doc = FormSequenceProjector::Project(seq);
        doc.tags.emplace_back(kLinearTag);
        const auto  nodes_before = doc.nodes.size();
        const auto  edges_before = doc.edges.size();

        ASSERT_TRUE(UpgradeToGraph(doc));
        EXPECT_FALSE(IsLinear(doc));
        // Store untouched apart from the tag.
        EXPECT_EQ(doc.nodes.size(), nodes_before);
        EXPECT_EQ(doc.edges.size(), edges_before);

        // After upgrade, projection is graph mode.
        EXPECT_EQ(ToAuthoringDocument(doc).kind, Kind::Graph);

        // Reversible: downgrade restores the formSequence view.
        ASSERT_TRUE(DowngradeToFormSequence(doc));
        EXPECT_TRUE(IsLinear(doc));
        EXPECT_EQ(ToAuthoringDocument(doc).kind, Kind::FormSequence);
    }

    TEST(AuthoringDocContractTest, Upgrade_IsIdempotentWhenNotLinear)
    {
        GraphDocumentDto doc; // no tag
        EXPECT_FALSE(UpgradeToGraph(doc));
        EXPECT_FALSE(DowngradeToFormSequence(doc) == false); // first add succeeds
        EXPECT_FALSE(DowngradeToFormSequence(doc));          // already linear
    }

    TEST(AuthoringDocContractTest, IsLinear_RespectsOnlyCanonicalTag)
    {
        GraphDocumentDto doc;
        EXPECT_FALSE(IsLinear(doc));
        doc.tags.emplace_back("fromsequence"); // legacy/other tag — not linear
        EXPECT_FALSE(IsLinear(doc));
        doc.tags.emplace_back(kLinearTag);
        EXPECT_TRUE(IsLinear(doc));
    }
} // namespace

