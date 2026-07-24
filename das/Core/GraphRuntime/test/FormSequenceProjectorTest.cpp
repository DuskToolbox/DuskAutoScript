#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>
#include <das/Core/GraphRuntime/FormSequenceProjector.h>
#include <das/Core/GraphRuntime/GraphCompiler.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <string>

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

    FormSequenceItemDto MakeItem(const std::string& item_id)
    {
        FormSequenceItemDto item;
        item.item_id = item_id;
        item.target.target_kind = "componentRef";
        item.target.component_ref =
            ComponentRefDto{"componentRef", "{comp-guid}", "{plug-guid}"};
        item.settings = MakeSettings("threshold", 0.5);
        return item;
    }

    FormSequenceDto MakeEmptySequence()
    {
        FormSequenceDto seq;
        seq.document_id = "test_seq";
        seq.version = 1;
        seq.fingerprint = "sha256:test";
        return seq;
    }

    /// Count signal edges in a projected document.
    std::size_t CountSignalEdges(const GraphDocumentDto& doc)
    {
        std::size_t n = 0;
        for (const auto& e : doc.edges)
        {
            if (e.edge_type == "signal")
            {
                ++n;
            }
        }
        return n;
    }
} // anonymous namespace

// ===========================================================================
// Project — formSequence → GraphDocumentDto
// ===========================================================================

TEST(FormSequenceProjectorTest, EmptySequenceProjectsToEmptyDocument)
{
    auto seq = MakeEmptySequence();
    auto doc = FormSequenceProjector::Project(seq);

    EXPECT_EQ(doc.document_id, "test_seq");
    EXPECT_EQ(doc.version, 1);
    EXPECT_EQ(doc.fingerprint, "sha256:test");
    EXPECT_TRUE(doc.nodes.empty());
    EXPECT_TRUE(doc.edges.empty());
}

TEST(FormSequenceProjectorTest, ItemsBecomeNodesInOrder)
{
    auto seq = MakeEmptySequence();
    seq.items = {MakeItem("a"), MakeItem("b"), MakeItem("c")};
    auto doc = FormSequenceProjector::Project(seq);

    ASSERT_EQ(doc.nodes.size(), 3u);
    EXPECT_EQ(doc.nodes[0].node_id, "a");
    EXPECT_EQ(doc.nodes[1].node_id, "b");
    EXPECT_EQ(doc.nodes[2].node_id, "c");
    // Target + settings are preserved on the projected node.
    EXPECT_EQ(doc.nodes[0].target.target_kind, "componentRef");
    ASSERT_TRUE(doc.nodes[0].target.component_ref.has_value());
    EXPECT_EQ(doc.nodes[0].target.component_ref->component_guid, "{comp-guid}");
}

TEST(FormSequenceProjectorTest, ConsecutiveItemsLinkedBySignalEdge)
{
    auto seq = MakeEmptySequence();
    seq.items = {MakeItem("a"), MakeItem("b"), MakeItem("c")};
    auto doc = FormSequenceProjector::Project(seq);

    // N items → N-1 linear signal edges, auto-maintained.
    ASSERT_EQ(CountSignalEdges(doc), 2u);
    ASSERT_EQ(doc.edges.size(), 2u);

    EXPECT_EQ(doc.edges[0].source_node_id, "a");
    EXPECT_EQ(doc.edges[0].target_node_id, "b");
    EXPECT_EQ(
        doc.edges[0].source_port_id,
        std::string(FormSequenceProjector::SignalOutPort));
    EXPECT_EQ(
        doc.edges[0].target_port_id,
        std::string(FormSequenceProjector::SignalInPort));
    EXPECT_EQ(doc.edges[0].edge_type, "signal");

    EXPECT_EQ(doc.edges[1].source_node_id, "b");
    EXPECT_EQ(doc.edges[1].target_node_id, "c");
}

TEST(FormSequenceProjectorTest, SingleItemHasNoEdges)
{
    auto seq = MakeEmptySequence();
    seq.items = {MakeItem("solo")};
    auto doc = FormSequenceProjector::Project(seq);

    ASSERT_EQ(doc.nodes.size(), 1u);
    EXPECT_TRUE(doc.edges.empty());
}

TEST(FormSequenceProjectorTest, ProjectIsPureAndIdempotent)
{
    auto seq = MakeEmptySequence();
    seq.items = {MakeItem("a"), MakeItem("b")};

    auto doc1 = FormSequenceProjector::Project(seq);
    auto doc2 = FormSequenceProjector::Project(seq);

    ASSERT_EQ(doc1.nodes.size(), doc2.nodes.size());
    ASSERT_EQ(doc1.edges.size(), doc2.edges.size());
    // Projecting does not mutate the source sequence.
    ASSERT_EQ(seq.items.size(), 2u);
}

// ===========================================================================
// ApplyChange — append / insert / move / remove / setValue
// ===========================================================================

TEST(FormSequenceProjectorTest, AppendAddsItemAtEnd)
{
    auto seq = MakeEmptySequence();
    EXPECT_TRUE(
        FormSequenceProjector::ApplyChange(
            seq,
            FormSequenceChange{AppendSequenceItemChange{MakeItem("a")}})
            .Ok());
    EXPECT_TRUE(
        FormSequenceProjector::ApplyChange(
            seq,
            FormSequenceChange{AppendSequenceItemChange{MakeItem("b")}})
            .Ok());

    ASSERT_EQ(seq.items.size(), 2u);
    EXPECT_EQ(seq.items[0].item_id, "a");
    EXPECT_EQ(seq.items[1].item_id, "b");
}

TEST(FormSequenceProjectorTest, AppendRejectsEmptyAndDuplicateItemId)
{
    auto seq = MakeEmptySequence();
    seq.items = {MakeItem("a")};

    auto r1 = FormSequenceProjector::ApplyChange(
        seq,
        FormSequenceChange{AppendSequenceItemChange{MakeItem("")}});
    EXPECT_FALSE(r1.Ok());
    EXPECT_EQ(r1.error_kind, FormSequenceErrorKind::EmptyItemId);

    auto r2 = FormSequenceProjector::ApplyChange(
        seq,
        FormSequenceChange{AppendSequenceItemChange{MakeItem("a")}});
    EXPECT_FALSE(r2.Ok());
    EXPECT_EQ(r2.error_kind, FormSequenceErrorKind::DuplicateItemId);
    ASSERT_EQ(seq.items.size(), 1u); // unchanged
}

TEST(FormSequenceProjectorTest, InsertPlacesItemAtIndex)
{
    auto seq = MakeEmptySequence();
    seq.items = {MakeItem("a"), MakeItem("c")};

    EXPECT_TRUE(
        FormSequenceProjector::ApplyChange(
            seq,
            FormSequenceChange{InsertSequenceItemChange{1u, MakeItem("b")}})
            .Ok());

    ASSERT_EQ(seq.items.size(), 3u);
    EXPECT_EQ(seq.items[0].item_id, "a");
    EXPECT_EQ(seq.items[1].item_id, "b");
    EXPECT_EQ(seq.items[2].item_id, "c");
}

TEST(FormSequenceProjectorTest, InsertRejectsOutOfRangeIndex)
{
    auto seq = MakeEmptySequence();
    seq.items = {MakeItem("a")};

    auto r = FormSequenceProjector::ApplyChange(
        seq,
        FormSequenceChange{InsertSequenceItemChange{5u, MakeItem("b")}});
    EXPECT_FALSE(r.Ok());
    EXPECT_EQ(r.error_kind, FormSequenceErrorKind::InvalidIndex);
    ASSERT_EQ(seq.items.size(), 1u);
}

TEST(FormSequenceProjectorTest, MoveReordersItem)
{
    auto seq = MakeEmptySequence();
    seq.items = {MakeItem("a"), MakeItem("b"), MakeItem("c")};

    // Move "a" (index 0) to index 2 → [b, c, a]
    EXPECT_TRUE(
        FormSequenceProjector::ApplyChange(
            seq,
            FormSequenceChange{MoveSequenceItemChange{0u, 2u}})
            .Ok());

    ASSERT_EQ(seq.items.size(), 3u);
    EXPECT_EQ(seq.items[0].item_id, "b");
    EXPECT_EQ(seq.items[1].item_id, "c");
    EXPECT_EQ(seq.items[2].item_id, "a");
}

TEST(FormSequenceProjectorTest, MoveRejectsOutOfRange)
{
    auto seq = MakeEmptySequence();
    seq.items = {MakeItem("a")};

    auto r = FormSequenceProjector::ApplyChange(
        seq,
        FormSequenceChange{MoveSequenceItemChange{0u, 3u}});
    EXPECT_FALSE(r.Ok());
    EXPECT_EQ(r.error_kind, FormSequenceErrorKind::InvalidIndex);
}

TEST(FormSequenceProjectorTest, RemoveDeletesItem)
{
    auto seq = MakeEmptySequence();
    seq.items = {MakeItem("a"), MakeItem("b"), MakeItem("c")};

    EXPECT_TRUE(
        FormSequenceProjector::ApplyChange(
            seq,
            FormSequenceChange{RemoveSequenceItemChange{"b"}})
            .Ok());

    ASSERT_EQ(seq.items.size(), 2u);
    EXPECT_EQ(seq.items[0].item_id, "a");
    EXPECT_EQ(seq.items[1].item_id, "c");
}

TEST(FormSequenceProjectorTest, RemoveRejectsMissingItem)
{
    auto seq = MakeEmptySequence();
    seq.items = {MakeItem("a")};

    auto r = FormSequenceProjector::ApplyChange(
        seq,
        FormSequenceChange{RemoveSequenceItemChange{"nope"}});
    EXPECT_FALSE(r.Ok());
    EXPECT_EQ(r.error_kind, FormSequenceErrorKind::ItemNotFound);
    ASSERT_EQ(seq.items.size(), 1u);
}

TEST(FormSequenceProjectorTest, SetValueReplacesSettings)
{
    auto seq = MakeEmptySequence();
    seq.items = {MakeItem("a")};

    EXPECT_TRUE(
        FormSequenceProjector::ApplyChange(
            seq,
            FormSequenceChange{SetSequenceItemValueChange{
                "a",
                MakeSettings("threshold", 0.9)}})
            .Ok());

    auto json_str = std::string(seq.items[0].settings.write());
    EXPECT_TRUE(json_str.find("0.9") != std::string::npos)
        << "Expected updated value 0.9, got: " << json_str;
}

// ===========================================================================
// Projection stays consistent after edits (signal edges auto-maintained)
// ===========================================================================

TEST(FormSequenceProjectorTest, EditsKeepProjectionLinear)
{
    auto seq = MakeEmptySequence();
    FormSequenceProjector::ApplyChange(
        seq,
        FormSequenceChange{AppendSequenceItemChange{MakeItem("a")}});
    FormSequenceProjector::ApplyChange(
        seq,
        FormSequenceChange{AppendSequenceItemChange{MakeItem("b")}});
    FormSequenceProjector::ApplyChange(
        seq,
        FormSequenceChange{AppendSequenceItemChange{MakeItem("c")}});

    // Remove the middle item — the projection must re-link a→c.
    FormSequenceProjector::ApplyChange(
        seq,
        FormSequenceChange{RemoveSequenceItemChange{"b"}});

    auto doc = FormSequenceProjector::Project(seq);
    ASSERT_EQ(doc.nodes.size(), 2u);
    ASSERT_EQ(CountSignalEdges(doc), 1u);
    EXPECT_EQ(doc.edges[0].source_node_id, "a");
    EXPECT_EQ(doc.edges[0].target_node_id, "c");
}

// ===========================================================================
// Compile-level E2E: project → GraphCompiler → valid executable plan
// ===========================================================================

TEST(FormSequenceProjectorTest, ProjectedChainCompilesInSequenceOrder)
{
    auto seq = MakeEmptySequence();
    seq.items = {MakeItem("t1"), MakeItem("t2"), MakeItem("t3")};
    auto doc = FormSequenceProjector::Project(seq);

    GraphCompiler compiler;
    auto          plan = compiler.Compile(doc);

    // Linear signal chain → execution order == sequence order.
    ASSERT_EQ(plan.execution_order.size(), 3u);
    EXPECT_EQ(plan.execution_order[0], "t1");
    EXPECT_EQ(plan.execution_order[1], "t2");
    EXPECT_EQ(plan.execution_order[2], "t3");

    // Signal routes mirror the chain; no loops, no diagnostics.
    EXPECT_EQ(plan.signal_routes.size(), 2u);
    EXPECT_TRUE(plan.back_edges.empty());
    // Signal edges are not port-validated → no PortNotFound diagnostics.
    EXPECT_TRUE(plan.diagnostics.empty());
}
