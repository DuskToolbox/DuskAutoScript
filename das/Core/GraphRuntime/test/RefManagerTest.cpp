#include <das/Core/GraphRuntime/RefManager.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/GraphRuntime/GraphEntryId.h>
#include <das/Core/TaskScheduler/TaskRepositoryDtos.h>
#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

namespace
{
    using namespace Das::Core::GraphRuntime;
    using namespace Das::Core::GraphRuntime::Dto;
    using TaskDto =
        Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto;

    // --- Test helpers ---

    GraphNodeDto MakeEntryRefNode(
        const std::string& node_id,
        int64_t            ref_entry_id)
    {
        GraphNodeDto node;
        node.node_id = node_id;
        node.target = GraphNodeTargetDto{
            "entryRef",
            std::nullopt,
            EntryRefDto{"entryRef", ref_entry_id, std::nullopt, std::nullopt}};
        return node;
    }

    GraphNodeDto MakeComponentRefNode(
        const std::string& node_id,
        const std::string& component_guid)
    {
        GraphNodeDto node;
        node.node_id = node_id;
        node.target = GraphNodeTargetDto{
            "componentRef",
            ComponentRefDto{"componentRef", component_guid, "{plugin-guid}"},
            std::nullopt};
        return node;
    }

    GraphDocumentDto MakeGraphDocumentWithNodes(
        const std::string&                 doc_id,
        std::vector<GraphNodeDto>          nodes)
    {
        GraphDocumentDto doc;
        doc.document_id = doc_id;
        doc.version = 1;
        doc.fingerprint = "sha256:test";
        doc.nodes = std::move(nodes);
        return doc;
    }

    GraphDocumentDto MakeGraphDocumentWithEntryRefNode(
        const std::string& doc_id,
        int64_t            ref_entry_id)
    {
        return MakeGraphDocumentWithNodes(
            doc_id,
            {MakeEntryRefNode("node_ref_" + doc_id, ref_entry_id)});
    }

    TaskDto MakeEntryWithGraphDocument(
        int64_t                  entry_id,
        const GraphDocumentDto&  graph_doc)
    {
        TaskDto entry;
        entry.entry_id = entry_id;
        entry.display_name = "entry_" + std::to_string(entry_id);
        entry.plugin_guid = "{plugin-guid}";
        entry.task_type_guid = "{task-type-guid}";
        entry.graph_document = yyjson::object(graph_doc);
        return entry;
    }

    TaskDto MakeEntryWithoutGraphDocument(int64_t entry_id)
    {
        TaskDto entry;
        entry.entry_id = entry_id;
        entry.display_name = "entry_" + std::to_string(entry_id);
        entry.plugin_guid = "{plugin-guid}";
        entry.task_type_guid = "{task-type-guid}";
        return entry;
    }

    // --- RefManager fixture ---

    class RefManagerTest : public ::testing::Test
    {
    protected:
        RefManager manager;
    };

    // --- Tests ---

    TEST_F(RefManagerTest, ScanEntryRefsSingleDocument)
    {
        // 1 GraphDocument with 1 entryRef node pointing to entry 99
        auto doc = MakeGraphDocumentWithEntryRefNode("doc_1", 99);
        auto entries = std::vector<TaskDto>{
            MakeEntryWithGraphDocument(1, doc)};

        auto result = manager.ScanEntryRefs(entries);

        ASSERT_EQ(result.size(), 1u);
        ASSERT_TRUE(result.count(99) > 0);
        ASSERT_EQ(result[99].size(), 1u);
        EXPECT_EQ(result[99][0], "doc_1");
    }

    TEST_F(RefManagerTest, ComputeRefCountSingleReference)
    {
        auto doc = MakeGraphDocumentWithEntryRefNode("doc_1", 99);
        auto entries = std::vector<TaskDto>{
            MakeEntryWithGraphDocument(1, doc)};

        EXPECT_EQ(manager.ComputeRefCount(99, entries), 1);
    }

    TEST_F(RefManagerTest, ComputeRefCountZeroReferences)
    {
        // Empty entries — nothing references anything
        auto entries = std::vector<TaskDto>{};
        EXPECT_EQ(manager.ComputeRefCount(42, entries), 0);

        // Entry without any entryRef
        auto doc = MakeGraphDocumentWithNodes("doc_empty", {});
        auto entries_with_doc = std::vector<TaskDto>{
            MakeEntryWithGraphDocument(1, doc)};
        EXPECT_EQ(manager.ComputeRefCount(42, entries_with_doc), 0);
    }

    TEST_F(RefManagerTest, ComputeRefCountMultipleDocuments)
    {
        // 2 documents each referencing entry 99
        auto doc1 = MakeGraphDocumentWithEntryRefNode("doc_1", 99);
        auto doc2 = MakeGraphDocumentWithEntryRefNode("doc_2", 99);
        auto entries = std::vector<TaskDto>{
            MakeEntryWithGraphDocument(1, doc1),
            MakeEntryWithGraphDocument(2, doc2)};

        EXPECT_EQ(manager.ComputeRefCount(99, entries), 2);
    }

    TEST_F(RefManagerTest, ComputeRefCountMultipleTargets)
    {
        // 1 document referencing two different targets
        auto doc = MakeGraphDocumentWithNodes(
            "doc_multi",
            {MakeEntryRefNode("node_a", 100),
             MakeEntryRefNode("node_b", 200)});
        auto entries = std::vector<TaskDto>{
            MakeEntryWithGraphDocument(1, doc)};

        EXPECT_EQ(manager.ComputeRefCount(100, entries), 1);
        EXPECT_EQ(manager.ComputeRefCount(200, entries), 1);
        EXPECT_EQ(manager.ComputeRefCount(999, entries), 0);
    }

    TEST_F(RefManagerTest, ComputeRefCountSelfReference)
    {
        // Document with node targeting its own entry_id (100)
        auto doc =
            MakeGraphDocumentWithEntryRefNode("doc_self", 100);
        auto entries = std::vector<TaskDto>{
            MakeEntryWithGraphDocument(100, doc)};

        // Self-reference counted normally — no infinite loop
        EXPECT_EQ(manager.ComputeRefCount(100, entries), 1);
    }

    TEST_F(RefManagerTest, ComputeRefCountsReturnsAllEntries)
    {
        // 3 documents referencing 2 different targets
        auto doc1 = MakeGraphDocumentWithEntryRefNode("doc_1", 10);
        auto doc2 = MakeGraphDocumentWithEntryRefNode("doc_2", 10);
        auto doc3 = MakeGraphDocumentWithEntryRefNode("doc_3", 20);
        auto entries = std::vector<TaskDto>{
            MakeEntryWithGraphDocument(1, doc1),
            MakeEntryWithGraphDocument(2, doc2),
            MakeEntryWithGraphDocument(3, doc3)};

        auto ref_counts = manager.ComputeRefCounts(entries);

        ASSERT_EQ(ref_counts.size(), 2u);
        EXPECT_EQ(ref_counts[10], 2);
        EXPECT_EQ(ref_counts[20], 1);
    }

    TEST_F(RefManagerTest, ComputeRefCountMultipleRefsSameDocument)
    {
        // 1 document with 2 entryRef nodes to the same target
        auto doc = MakeGraphDocumentWithNodes(
            "doc_double",
            {MakeEntryRefNode("node_1", 50),
             MakeEntryRefNode("node_2", 50)});
        auto entries = std::vector<TaskDto>{
            MakeEntryWithGraphDocument(1, doc)};

        EXPECT_EQ(manager.ComputeRefCount(50, entries), 2);
    }

    TEST_F(RefManagerTest, ScanHandlesEmptyGraphDocument)
    {
        // Document with 0 nodes
        auto doc = MakeGraphDocumentWithNodes("doc_empty", {});
        auto entries = std::vector<TaskDto>{
            MakeEntryWithGraphDocument(1, doc)};

        auto result = manager.ScanEntryRefs(entries);
        EXPECT_TRUE(result.empty());
        EXPECT_EQ(manager.ComputeRefCount(1, entries), 0);
    }

    TEST_F(RefManagerTest, ScanHandlesMissingGraphDocumentField)
    {
        // Entry with graph_document=null (default)
        auto entries = std::vector<TaskDto>{
            MakeEntryWithoutGraphDocument(1),
            MakeEntryWithoutGraphDocument(2)};

        auto result = manager.ScanEntryRefs(entries);
        EXPECT_TRUE(result.empty());
        EXPECT_EQ(manager.ComputeRefCount(1, entries), 0);
    }

    TEST_F(RefManagerTest, ScanHandlesMixedTargetKinds)
    {
        // Document with 2 componentRef + 1 entryRef — only entryRef counted
        auto doc = MakeGraphDocumentWithNodes(
            "doc_mixed",
            {MakeComponentRefNode("comp_1", "{guid-a}"),
             MakeComponentRefNode("comp_2", "{guid-b}"),
             MakeEntryRefNode("entry_node", 77)});
        auto entries = std::vector<TaskDto>{
            MakeEntryWithGraphDocument(1, doc)};

        auto result = manager.ScanEntryRefs(entries);
        ASSERT_EQ(result.size(), 1u);
        ASSERT_TRUE(result.count(77) > 0);
        EXPECT_EQ(result[77].size(), 1u);
        EXPECT_EQ(manager.ComputeRefCount(77, entries), 1);
    }
} // namespace
