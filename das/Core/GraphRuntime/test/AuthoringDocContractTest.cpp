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

    // copy_string must deep-copy strings nested INSIDE yyjson::value settings
    // subtrees too, not just top-level scalar members — otherwise the returned
    // value dangles once the store is gone (das-yyjson-string-safety skill).
    TEST(AuthoringDocContractTest, GraphMode_CopyStringDeepCopiesNestedStringSettings)
    {
        GraphDocumentDto doc;
        // settings carries a string payload that would dangle if not copied.
        auto payload = Das::Utils::MakeYyjsonObject();
        auto sobj    = *payload.as_object();
        sobj[std::string_view("name")]      = std::string("deep-string-payload");
        sobj[std::string_view("threshold")] = 0.5;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes.back().settings = payload;

        auto json = ToJson(doc);
        EXPECT_NE(json.find("deep-string-payload"), std::string::npos);
        // Sanity: the nested object survived the cross-boundary return intact.
        EXPECT_NE(json.find("\"threshold\":0.5"), std::string::npos);
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
        EXPECT_EQ(items[0].component_guid, "{ga}");
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

    // Comprehensive zero-leak guard: the contract output must NEVER carry any
    // runtime-private field, regardless of mode or edge type. Covers edgeType,
    // edgeId, dynamicPorts, and the signal/data edge-type tokens (which only
    // exist on the internal GraphEdgeDto, never on the contract GraphConnection).
    TEST(AuthoringDocContractTest, BothModes_NeverLeakRuntimePrivateFields)
    {
        // graph mode: a node with dynamic_ports + a data edge + a signal edge.
        GraphDocumentDto g;
        auto& n1 = g.nodes.emplace_back(MakeNode("n1", "{g1}", 0.0));
        n1.dynamic_ports.push_back({.port_id = "dyn", .port_type = "data"});
        g.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        GraphEdgeDto data_edge = MakeSignalEdge("e-data", "n1", "n2");
        data_edge.edge_type    = "data";
        g.edges.push_back(data_edge);
        GraphEdgeDto sig = MakeSignalEdge("e-sig", "n1", "n2");
        g.edges.push_back(sig);

        const auto graph_json = ToJson(g);
        EXPECT_EQ(graph_json.find("edgeType"), std::string::npos);
        EXPECT_EQ(graph_json.find("edgeId"), std::string::npos);
        EXPECT_EQ(graph_json.find("dynamicPorts"), std::string::npos);
        EXPECT_EQ(graph_json.find("\"signal\""), std::string::npos);
        EXPECT_EQ(graph_json.find("\"data\""), std::string::npos);

        // formSequence mode: project a sequence (signal edges auto-generated) +
        // ensure the same fields are absent in the projected view.
        FormSequenceDto seq;
        seq.items.push_back({.item_id = "a",
                             .target  = {.target_kind = "componentRef",
                                         .component_ref = MakeCompRef("{ga}")},
                             .settings = MakeSettings(1.0)});
        seq.items.push_back({.item_id = "b",
                             .target  = {.target_kind = "componentRef",
                                         .component_ref = MakeCompRef("{gb}")},
                             .settings = MakeSettings(2.0)});
        auto fdoc = FormSequenceProjector::Project(seq);
        fdoc.tags.emplace_back(kLinearTag);
        const auto fs_json = ToJson(fdoc);
        EXPECT_EQ(fs_json.find("edgeType"), std::string::npos);
        EXPECT_EQ(fs_json.find("edgeId"), std::string::npos);
        EXPECT_EQ(fs_json.find("dynamicPorts"), std::string::npos);
        EXPECT_EQ(fs_json.find("\"signal\""), std::string::npos);
    }

    // -----------------------------------------------------------------------
    // DAS-81 compliance: componentGuid (no `type`), entryRef emit, fields[]
    // -----------------------------------------------------------------------

    GraphNodeDto MakeEntryRefNode(
        std::string_view id,
        int64_t          entry_id,
        int64_t          expected_rev,
        std::string_view fingerprint)
    {
        GraphNodeDto node;
        node.node_id          = std::string(id);
        node.target.target_kind = "entryRef";
        node.target.entry_ref =
            EntryRefDto{"entryRef", entry_id, expected_rev, std::string(fingerprint)};
        return node;
    }

    TEST(AuthoringDocContractTest, GraphMode_EmitsComponentGuidAndNotType)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        auto json = ToJson(doc);
        // DAS-81 rename: componentGuid present, legacy `type` absent.
        EXPECT_NE(json.find("\"componentGuid\":\"{g1}\""), std::string::npos);
        EXPECT_EQ(json.find("\"type\":\"{g1}\""), std::string::npos);
    }

    TEST(AuthoringDocContractTest, GraphMode_EntryRefEmittedForSubgraphNode)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeEntryRefNode("sub", 42, 3, "fp-sub"));
        auto out = ToAuthoringDocument(doc);
        ASSERT_TRUE(out.graph.has_value());
        ASSERT_EQ(out.graph->nodes.size(), 1u);
        const auto& n = out.graph->nodes[0];
        EXPECT_EQ(n.target_kind, "entryRef");
        ASSERT_TRUE(n.entry_ref.has_value());
        EXPECT_EQ(n.entry_ref->entry_id, 42);
        ASSERT_TRUE(n.entry_ref->expected_revision.has_value());
        EXPECT_EQ(*n.entry_ref->expected_revision, 3);
        ASSERT_TRUE(n.entry_ref->source_fingerprint.has_value());
        EXPECT_EQ(*n.entry_ref->source_fingerprint, "fp-sub");

        auto json = ToJson(doc);
        EXPECT_NE(json.find("\"entryId\":42"), std::string::npos);
        EXPECT_NE(json.find("\"expectedRevision\":3"), std::string::npos);
        EXPECT_NE(json.find("\"sourceFingerprint\":\"fp-sub\""), std::string::npos);
    }

    TEST(AuthoringDocContractTest, GraphMode_FieldsProjectedFromSettings)
    {
        GraphDocumentDto doc;
        auto& n = doc.nodes.emplace_back(MakeNode("n1", "{g1}", 0.0));
        auto payload = Das::Utils::MakeYyjsonObject();
        auto sobj    = *payload.as_object();
        sobj[std::string_view("threshold")] = 0.5;   // float -> valueType 2
        sobj[std::string_view("count")]     = 3;     // int -> valueType 0
        sobj[std::string_view("name")]      = std::string("x"); // string -> 4
        sobj[std::string_view("enabled")]   = true;  // bool -> 8
        n.settings = payload;

        auto out = ToAuthoringDocument(doc);
        ASSERT_TRUE(out.graph.has_value());
        ASSERT_EQ(out.graph->nodes.size(), 1u);
        const auto& fields = out.graph->nodes[0].fields;
        ASSERT_EQ(fields.size(), 4u);

        auto json = ToJson(doc);
        EXPECT_NE(json.find("\"fields\""), std::string::npos);
        EXPECT_NE(json.find("\"valueType\":2"), std::string::npos);
        EXPECT_NE(json.find("\"valueType\":0"), std::string::npos);
        EXPECT_NE(json.find("\"inputType\":0"), std::string::npos);
    }

    TEST(AuthoringDocContractTest, SerializeSource_PersistsFullGraphDocumentDto)
    {
        // D-41: acceptedProperties = source (full GraphDocumentDto). It MUST
        // carry the internal-only fields (edgeId/edgeType/dynamicPorts) that
        // the regenerated document view deliberately drops — proving source and
        // view are now distinct blobs.
        GraphDocumentDto doc;
        doc.version     = 5;
        doc.fingerprint = "fp";
        auto& n1 = doc.nodes.emplace_back(MakeNode("n1", "{g1}", 0.0));
        n1.dynamic_ports.push_back({.port_id = "dyn", .port_type = "data"});
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        GraphEdgeDto e;
        e.edge_id = "e1"; e.source_node_id = "n1"; e.source_port_id = "o";
        e.target_node_id = "n2"; e.target_port_id = "i"; e.edge_type = "data";
        doc.edges.push_back(e);

        auto src = Contract::SerializeSource(doc);
        auto src_str = Das::Utils::SerializeYyjsonValue(src).value_or(std::string{});
        // Source keeps internal-only fields...
        EXPECT_NE(src_str.find("\"edgeId\""), std::string::npos);
        EXPECT_NE(src_str.find("\"edgeType\""), std::string::npos);
        EXPECT_NE(src_str.find("\"dynamicPorts\""), std::string::npos);
        EXPECT_NE(src_str.find("\"nodes\""), std::string::npos);

        // ...while the document view still strips them.
        auto view_str = ToJson(doc);
        EXPECT_EQ(view_str.find("edgeId"), std::string::npos);
        EXPECT_EQ(view_str.find("edgeType"), std::string::npos);
        EXPECT_EQ(view_str.find("dynamicPorts"), std::string::npos);
    }

    // -----------------------------------------------------------------------
    // Upgrade — one-way, lossless on the store
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
        // Store untouched apart from the tag → lossless.
        EXPECT_EQ(doc.nodes.size(), nodes_before);
        EXPECT_EQ(doc.edges.size(), edges_before);

        // After upgrade, projection is graph mode.
        EXPECT_EQ(ToAuthoringDocument(doc).kind, Kind::Graph);

        // One-way: re-applying UpgradeToGraph on an already-graph doc is a no-op
        // (returns false), and there is intentionally no downgrade path.
        EXPECT_FALSE(UpgradeToGraph(doc));
        EXPECT_EQ(ToAuthoringDocument(doc).kind, Kind::Graph);
    }

    TEST(AuthoringDocContractTest, Upgrade_IsNoOpWhenNotLinear)
    {
        GraphDocumentDto doc; // no tag → already graph mode
        EXPECT_FALSE(UpgradeToGraph(doc)); // nothing to remove
        EXPECT_FALSE(IsLinear(doc));
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

    // -----------------------------------------------------------------------
    // ApplyAuthoringChange — graph mode (delegates to GraphAuthoring)
    // -----------------------------------------------------------------------

    TEST(AuthoringDocContractTest, GraphChange_AddNodeThenConnectPorts)
    {
        GraphDocumentDto doc; // graph mode (no tag)
        doc.version = 0;

        AuthoringChange add;
        add.op   = "addNode";
        add.node = GraphNode{.id = "n1", .component_guid = "{g1}",
                             .settings = MakeSettings(1.0)};
        ASSERT_TRUE(ApplyAuthoringChange(doc, add).Ok());
        ASSERT_EQ(doc.nodes.size(), 1u);
        EXPECT_EQ(doc.version, 1);

        AuthoringChange add2;
        add2.op   = "addNode";
        add2.node = GraphNode{.id = "n2", .component_guid = "{g2}",
                              .settings = MakeSettings(2.0)};
        ASSERT_TRUE(ApplyAuthoringChange(doc, add2).Ok());

        AuthoringChange conn;
        conn.op         = "connectPorts";
        conn.connection = GraphConnection{"n1", "out", "n2", "in"};
        ASSERT_TRUE(ApplyAuthoringChange(doc, conn).Ok());
        ASSERT_EQ(doc.edges.size(), 1u);
        EXPECT_EQ(doc.edges[0].source_node_id, "n1");
        EXPECT_EQ(doc.edges[0].edge_type, "data"); // graph-mode edges are data

        // The public projection still hides edge_type / edge_id.
        auto json = ToJson(doc);
        EXPECT_EQ(json.find("edgeType"), std::string::npos);
        EXPECT_EQ(json.find("edgeId"), std::string::npos);
    }

    TEST(AuthoringDocContractTest, GraphChange_RemoveNodeCascadesEdges)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        GraphEdgeDto e;
        e.edge_id = "e1"; e.source_node_id = "n1"; e.source_port_id = "o";
        e.target_node_id = "n2"; e.target_port_id = "i"; e.edge_type = "data";
        doc.edges.push_back(e);

        AuthoringChange rm;
        rm.op = "removeNode";
        rm.node_id = "n1";
        ASSERT_TRUE(ApplyAuthoringChange(doc, rm).Ok());
        ASSERT_EQ(doc.nodes.size(), 1u);
        EXPECT_EQ(doc.nodes[0].node_id, "n2");
        EXPECT_TRUE(doc.edges.empty()); // cascade
    }

    TEST(AuthoringDocContractTest, GraphChange_DisconnectByEndpoints)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        GraphEdgeDto e;
        e.edge_id = "private"; e.source_node_id = "n1"; e.source_port_id = "o";
        e.target_node_id = "n2"; e.target_port_id = "i"; e.edge_type = "data";
        doc.edges.push_back(e);

        AuthoringChange disc;
        disc.op         = "disconnectPorts";
        disc.connection = GraphConnection{"n1", "o", "n2", "i"};
        ASSERT_TRUE(ApplyAuthoringChange(doc, disc).Ok());
        EXPECT_TRUE(doc.edges.empty());
    }

    TEST(AuthoringDocContractTest, GraphChange_DuplicateNodeIsRejected)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        const auto before = doc.nodes.size();

        AuthoringChange add;
        add.op = "addNode";
        add.node = GraphNode{.id = "n1", .component_guid = "{g1}",
                             .settings = MakeSettings(0.0)};
        auto r = ApplyAuthoringChange(doc, add);
        ASSERT_FALSE(r.Ok());
        EXPECT_EQ(r.error_kind, ChangeErrorKind::DuplicateNodeId);
        EXPECT_EQ(doc.nodes.size(), before); // unchanged
    }

    // -----------------------------------------------------------------------
    // ApplyAuthoringChange — linear mode (reuses FormSequenceProjector)
    // -----------------------------------------------------------------------

    TEST(AuthoringDocContractTest, LinearChange_AddItemAppendsToChain)
    {
        // Seed a 2-item linear store via the projector + tag.
        FormSequenceDto seq;
        seq.items.push_back({.item_id = "a", .target = {.target_kind = "componentRef",
                              .component_ref = MakeCompRef("{ga}")}, .settings = MakeSettings(1.0)});
        seq.items.push_back({.item_id = "b", .target = {.target_kind = "componentRef",
                              .component_ref = MakeCompRef("{gb}")}, .settings = MakeSettings(2.0)});
        auto doc = FormSequenceProjector::Project(seq);
        doc.tags.emplace_back(kLinearTag);
        ASSERT_EQ(doc.edges.size(), 1u); // a->b signal

        AuthoringChange add;
        add.op   = "addSequenceItem";
        add.item = SequenceItem{.id = "c", .component_guid = "{gc}",
                                .settings = MakeSettings(3.0)};
        ASSERT_TRUE(ApplyAuthoringChange(doc, add).Ok());

        // Chain extended: a->b->c (two signal edges), still linear.
        ASSERT_EQ(doc.nodes.size(), 3u);
        ASSERT_EQ(doc.edges.size(), 2u);
        EXPECT_TRUE(IsLinear(doc));
        EXPECT_EQ(doc.edges[0].edge_type, "signal");
    }

    TEST(AuthoringDocContractTest, LinearChange_RemoveItemReconnectsChain)
    {
        FormSequenceDto seq;
        seq.items.push_back({.item_id = "a", .target = {.target_kind = "componentRef",
                              .component_ref = MakeCompRef("{ga}")}});
        seq.items.push_back({.item_id = "b", .target = {.target_kind = "componentRef",
                              .component_ref = MakeCompRef("{gb}")}});
        seq.items.push_back({.item_id = "c", .target = {.target_kind = "componentRef",
                              .component_ref = MakeCompRef("{gc}")}});
        auto doc = FormSequenceProjector::Project(seq);
        doc.tags.emplace_back(kLinearTag);

        AuthoringChange rm;
        rm.op = "removeSequenceItem";
        rm.node_id = "b";
        ASSERT_TRUE(ApplyAuthoringChange(doc, rm).Ok());

        // b gone; chain a->c reconnected (1 signal edge).
        ASSERT_EQ(doc.nodes.size(), 2u);
        ASSERT_EQ(doc.edges.size(), 1u);
        EXPECT_EQ(doc.edges[0].source_node_id, "a");
        EXPECT_EQ(doc.edges[0].target_node_id, "c");
    }

    TEST(AuthoringDocContractTest, LinearChange_MoveItemReordersChain)
    {
        FormSequenceDto seq;
        for (char c : {'a', 'b', 'c'})
        {
            seq.items.push_back({.item_id = std::string(1, c),
                                 .target = {.target_kind = "componentRef",
                                            .component_ref = MakeCompRef("{g}")}});
        }
        auto doc = FormSequenceProjector::Project(seq);
        doc.tags.emplace_back(kLinearTag);

        AuthoringChange mv;
        mv.op   = "moveSequenceItem";
        mv.from = 0;
        mv.to   = 2; // a moves to the end → b->c->a
        ASSERT_TRUE(ApplyAuthoringChange(doc, mv).Ok());

        // New head is b.
        auto items = ToAuthoringDocument(doc).form_sequence->sequence;
        ASSERT_EQ(items.size(), 3u);
        EXPECT_EQ(items[0].id, "b");
        EXPECT_EQ(items[1].id, "c");
        EXPECT_EQ(items[2].id, "a");
    }

    TEST(AuthoringDocContractTest, LinearChange_RejectsGraphOp)
    {
        GraphDocumentDto doc;
        doc.tags.emplace_back(kLinearTag);

        AuthoringChange bad;
        bad.op = "addNode";
        bad.node = GraphNode{.id = "x", .component_guid = "{g}",
                             .settings = MakeSettings(0.0)};
        auto r = ApplyAuthoringChange(doc, bad);
        EXPECT_FALSE(r.Ok());
        EXPECT_EQ(r.error_kind, ChangeErrorKind::InvalidOp);
    }
} // namespace

