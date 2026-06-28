#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/GraphRuntime/GraphEntryId.h>
#include <das/Core/GraphRuntime/RefManager.h>
#include <das/Core/TaskScheduler/TaskRepositoryDtos.h>
#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <functional>
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
        const std::string&        doc_id,
        std::vector<GraphNodeDto> nodes)
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
        int64_t                 entry_id,
        const GraphDocumentDto& graph_doc)
    {
        TaskDto entry;
        entry.entry_id = entry_id;
        entry.display_name = "entry_" + std::to_string(entry_id);
        entry.plugin_guid = "{plugin-guid}";
        entry.task_type_guid = "{task-type-guid}";
        entry.graph_document = yyjson::object(graph_doc, yyjson::copy_string);
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
        auto entries = std::vector<TaskDto>{MakeEntryWithGraphDocument(1, doc)};

        auto result = manager.ScanEntryRefs(entries);

        ASSERT_EQ(result.size(), 1u);
        ASSERT_TRUE(result.count(99) > 0);
        ASSERT_EQ(result[99].size(), 1u);
        EXPECT_EQ(result[99][0], "doc_1");
    }

    TEST_F(RefManagerTest, ComputeRefCountSingleReference)
    {
        auto doc = MakeGraphDocumentWithEntryRefNode("doc_1", 99);
        auto entries = std::vector<TaskDto>{MakeEntryWithGraphDocument(1, doc)};

        EXPECT_EQ(manager.ComputeRefCount(99, entries), 1);
    }

    TEST_F(RefManagerTest, ComputeRefCountZeroReferences)
    {
        // Empty entries — nothing references anything
        auto entries = std::vector<TaskDto>{};
        EXPECT_EQ(manager.ComputeRefCount(42, entries), 0);

        // Entry without any entryRef
        auto doc = MakeGraphDocumentWithNodes("doc_empty", {});
        auto entries_with_doc =
            std::vector<TaskDto>{MakeEntryWithGraphDocument(1, doc)};
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
            {MakeEntryRefNode("node_a", 100), MakeEntryRefNode("node_b", 200)});
        auto entries = std::vector<TaskDto>{MakeEntryWithGraphDocument(1, doc)};

        EXPECT_EQ(manager.ComputeRefCount(100, entries), 1);
        EXPECT_EQ(manager.ComputeRefCount(200, entries), 1);
        EXPECT_EQ(manager.ComputeRefCount(999, entries), 0);
    }

    TEST_F(RefManagerTest, ComputeRefCountSelfReference)
    {
        // Document with node targeting its own entry_id (100)
        auto doc = MakeGraphDocumentWithEntryRefNode("doc_self", 100);
        auto entries =
            std::vector<TaskDto>{MakeEntryWithGraphDocument(100, doc)};

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
            {MakeEntryRefNode("node_1", 50), MakeEntryRefNode("node_2", 50)});
        auto entries = std::vector<TaskDto>{MakeEntryWithGraphDocument(1, doc)};

        EXPECT_EQ(manager.ComputeRefCount(50, entries), 2);
    }

    TEST_F(RefManagerTest, ScanHandlesEmptyGraphDocument)
    {
        // Document with 0 nodes
        auto doc = MakeGraphDocumentWithNodes("doc_empty", {});
        auto entries = std::vector<TaskDto>{MakeEntryWithGraphDocument(1, doc)};

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
        auto entries = std::vector<TaskDto>{MakeEntryWithGraphDocument(1, doc)};

        auto result = manager.ScanEntryRefs(entries);
        ASSERT_EQ(result.size(), 1u);
        ASSERT_TRUE(result.count(77) > 0);
        EXPECT_EQ(result[77].size(), 1u);
        EXPECT_EQ(manager.ComputeRefCount(77, entries), 1);
    }

    // ==================================================================
    // RefManager Lifecycle tests (02-18: CheckDelete, ShallowCopy,
    // DeepClone)
    // ==================================================================

    // --- Additional helpers for lifecycle tests ---

    EntryRefDto MakeEntryRefDto(
        int64_t                    entry_id,
        std::optional<int64_t>     revision = std::nullopt,
        std::optional<std::string> fingerprint = std::nullopt)
    {
        return EntryRefDto{"entryRef", entry_id, revision, fingerprint};
    }

    TaskDto MakeEntryWithCompiledArtifact(
        int64_t                 entry_id,
        const GraphDocumentDto& graph_doc,
        const std::string&      artifact_data)
    {
        TaskDto entry = MakeEntryWithGraphDocument(entry_id, graph_doc);
        entry.compiled_artifact = yyjson::object();
        return entry;
    }

    EntryFactory MockEntryFactory(std::vector<TaskDto>& created)
    {
        int64_t next_id = 1000;
        return
            [&created, next_id](const TaskDto& source) mutable -> GraphEntryId
        {
            TaskDto copy = source;
            copy.entry_id = next_id++;
            created.push_back(copy);
            return copy.entry_id;
        };
    }

    // --- Lifecycle fixture ---

    class RefManagerLifecycleTest : public ::testing::Test
    {
    protected:
        RefManager manager;
    };

    // --- CheckDelete tests ---

    TEST_F(RefManagerLifecycleTest, CheckDeleteRefCountZero)
    {
        // Entry 42 is NOT referenced by any graph_document
        auto doc = MakeGraphDocumentWithEntryRefNode("doc_1", 99);
        auto entries = std::vector<TaskDto>{MakeEntryWithGraphDocument(1, doc)};

        auto result = manager.CheckDelete(42, entries);

        EXPECT_TRUE(result.allowed);
        EXPECT_EQ(result.ref_count, 0);
    }

    TEST_F(RefManagerLifecycleTest, CheckDeleteRefCountOne)
    {
        // 1 document references entry 99
        auto doc = MakeGraphDocumentWithEntryRefNode("doc_1", 99);
        auto entries = std::vector<TaskDto>{MakeEntryWithGraphDocument(1, doc)};

        auto result = manager.CheckDelete(99, entries);

        EXPECT_FALSE(result.allowed);
        EXPECT_EQ(result.ref_count, 1);
        EXPECT_NE(result.reason.find("Referenced by"), std::string::npos);
    }

    TEST_F(RefManagerLifecycleTest, CheckDeleteRefCountMultiple)
    {
        // doc_1: 1 entryRef to 42, doc_2: 2 entryRef to 42 → total refCount=3
        auto doc1 = MakeGraphDocumentWithEntryRefNode("doc_1", 42);
        auto doc2 = MakeGraphDocumentWithNodes(
            "doc_2",
            {MakeEntryRefNode("ref_a", 42), MakeEntryRefNode("ref_b", 42)});
        auto entries = std::vector<TaskDto>{
            MakeEntryWithGraphDocument(1, doc1),
            MakeEntryWithGraphDocument(2, doc2)};

        auto result = manager.CheckDelete(42, entries);

        EXPECT_FALSE(result.allowed);
        EXPECT_EQ(result.ref_count, 3);
    }

    TEST_F(RefManagerLifecycleTest, CheckDeleteReasonIncludesDocumentIds)
    {
        // 2 documents reference entry 42
        auto doc1 = MakeGraphDocumentWithEntryRefNode("doc_alpha", 42);
        auto doc2 = MakeGraphDocumentWithEntryRefNode("doc_beta", 42);
        auto entries = std::vector<TaskDto>{
            MakeEntryWithGraphDocument(1, doc1),
            MakeEntryWithGraphDocument(2, doc2)};

        auto result = manager.CheckDelete(42, entries);

        EXPECT_NE(result.reason.find("doc_alpha"), std::string::npos);
        EXPECT_NE(result.reason.find("doc_beta"), std::string::npos);
    }

    TEST_F(RefManagerLifecycleTest, CheckDeleteDoesNotModifyEntries)
    {
        auto doc = MakeGraphDocumentWithEntryRefNode("doc_1", 99);
        auto entries = std::vector<TaskDto>{MakeEntryWithGraphDocument(1, doc)};

        auto size_before = entries.size();
        auto id_before = entries[0].entry_id;

        // Call CheckDelete — it is a pure predicate, must not modify
        auto result = manager.CheckDelete(99, entries);

        EXPECT_EQ(entries.size(), size_before);
        EXPECT_EQ(entries[0].entry_id, id_before);
    }

    // --- ShallowCopy tests ---

    TEST_F(RefManagerLifecycleTest, ShallowCopyEntryRefSameEntryId)
    {
        EntryRefDto original{"entryRef", 42, std::nullopt, std::nullopt};

        auto copy = RefManager::ShallowCopyEntryRef(original);

        EXPECT_EQ(copy.entry_id, original.entry_id);
    }

    TEST_F(RefManagerLifecycleTest, ShallowCopyEntryRefPreservesOptionalFields)
    {
        EntryRefDto original{
            "entryRef",
            42,
            int64_t{7},
            std::string{"sha256:abc"}};

        auto copy = RefManager::ShallowCopyEntryRef(original);

        EXPECT_EQ(copy.entry_id, original.entry_id);
        ASSERT_TRUE(copy.expected_revision.has_value());
        EXPECT_EQ(copy.expected_revision.value(), 7);
        ASSERT_TRUE(copy.source_fingerprint.has_value());
        EXPECT_EQ(copy.source_fingerprint.value(), "sha256:abc");
    }

    // --- DeepClone tests ---

    TEST_F(RefManagerLifecycleTest, DeepCloneEntryRefCreatesNewEntryId)
    {
        // Root (100) has entryRef to child (200)
        auto root_doc = MakeGraphDocumentWithEntryRefNode("doc_root", 200);
        auto root_entry = MakeEntryWithGraphDocument(100, root_doc);
        auto child_doc = MakeGraphDocumentWithNodes("doc_child", {});
        auto child_entry = MakeEntryWithGraphDocument(200, child_doc);
        auto entries = std::vector<TaskDto>{root_entry, child_entry};

        std::vector<TaskDto> created;
        auto                 factory = MockEntryFactory(created);
        auto result = manager.DeepCloneEntryRef(100, entries, factory);

        EXPECT_NE(result.new_entry_id, 100);
        ASSERT_EQ(result.id_mapping.size(), 2u);
        EXPECT_NE(result.id_mapping[100], 100);
        EXPECT_NE(result.id_mapping[200], 200);
    }

    TEST_F(RefManagerLifecycleTest, DeepCloneEntryRefCopiesGraphDocument)
    {
        // Root (100) has entryRef to child (200)
        auto root_doc = MakeGraphDocumentWithEntryRefNode("doc_root", 200);
        auto root_entry = MakeEntryWithGraphDocument(100, root_doc);
        auto child_doc = MakeGraphDocumentWithNodes("doc_child", {});
        auto child_entry = MakeEntryWithGraphDocument(200, child_doc);
        auto entries = std::vector<TaskDto>{root_entry, child_entry};

        std::vector<TaskDto> created;
        auto                 factory = MockEntryFactory(created);
        auto result = manager.DeepCloneEntryRef(100, entries, factory);

        // created[0] = cloned child (200 → 1000)
        // created[1] = cloned root  (100 → 1001)
        ASSERT_GE(created.size(), 2u);

        // The cloned root's graph_document should have the entryRef
        // remapped to the new child ID
        auto cloned_root_doc =
            yyjson::cast<GraphDocumentDto>(created[1].graph_document);
        ASSERT_EQ(cloned_root_doc.nodes.size(), 1u);
        ASSERT_TRUE(cloned_root_doc.nodes[0].target.entry_ref.has_value());
        EXPECT_EQ(
            cloned_root_doc.nodes[0].target.entry_ref->entry_id,
            result.id_mapping[200]);
    }

    TEST_F(RefManagerLifecycleTest, DeepCloneEntryRefCopiesCompiledArtifact)
    {
        // Root (100) has entryRef to child (200), root has compiled_artifact
        auto root_doc = MakeGraphDocumentWithEntryRefNode("doc_root", 200);
        auto root_entry =
            MakeEntryWithCompiledArtifact(100, root_doc, "artifact_data");
        auto child_doc = MakeGraphDocumentWithNodes("doc_child", {});
        auto child_entry = MakeEntryWithGraphDocument(200, child_doc);
        auto entries = std::vector<TaskDto>{root_entry, child_entry};

        std::vector<TaskDto> created;
        auto                 factory = MockEntryFactory(created);
        auto result = manager.DeepCloneEntryRef(100, entries, factory);

        // The cloned root should also have a compiled_artifact
        ASSERT_GE(created.size(), 2u);
        // created[1] is the cloned root (reverse order: child first)
        EXPECT_FALSE(created[1].compiled_artifact.is_null());
    }

    TEST_F(RefManagerLifecycleTest, DeepCloneEntryRefIdMapping)
    {
        // Root (100) with 2 distinct child entryRefs: 200 and 300
        auto root_doc = MakeGraphDocumentWithNodes(
            "doc_root",
            {MakeEntryRefNode("ref_a", 200), MakeEntryRefNode("ref_b", 300)});
        auto root_entry = MakeEntryWithGraphDocument(100, root_doc);
        auto child_a = MakeEntryWithGraphDocument(
            200,
            MakeGraphDocumentWithNodes("doc_a", {}));
        auto child_b = MakeEntryWithGraphDocument(
            300,
            MakeGraphDocumentWithNodes("doc_b", {}));
        auto entries = std::vector<TaskDto>{root_entry, child_a, child_b};

        std::vector<TaskDto> created;
        auto                 factory = MockEntryFactory(created);
        auto result = manager.DeepCloneEntryRef(100, entries, factory);

        // id_mapping should contain 3 entries: root + 2 children
        EXPECT_EQ(result.id_mapping.size(), 3u);
        EXPECT_TRUE(result.id_mapping.count(100) > 0);
        EXPECT_TRUE(result.id_mapping.count(200) > 0);
        EXPECT_TRUE(result.id_mapping.count(300) > 0);
    }

    TEST_F(RefManagerLifecycleTest, DeepCloneEntryRefRootHasNoChildren)
    {
        // Root (100) with no entryRef children
        auto root_doc = MakeGraphDocumentWithNodes("doc_root", {});
        auto root_entry = MakeEntryWithGraphDocument(100, root_doc);
        auto entries = std::vector<TaskDto>{root_entry};

        std::vector<TaskDto> created;
        auto                 factory = MockEntryFactory(created);
        auto result = manager.DeepCloneEntryRef(100, entries, factory);

        EXPECT_EQ(result.id_mapping.size(), 1u);
        EXPECT_TRUE(result.id_mapping.count(100) > 0);
        EXPECT_NE(result.new_entry_id, 100);
    }

    TEST_F(RefManagerLifecycleTest, DeepCloneEntryRefPreservesComponentRef)
    {
        // Root (100) has 1 componentRef + 1 entryRef to child (200)
        auto root_doc = MakeGraphDocumentWithNodes(
            "doc_root",
            {MakeComponentRefNode("comp_1", "{guid-a}"),
             MakeEntryRefNode("ref_child", 200)});
        auto root_entry = MakeEntryWithGraphDocument(100, root_doc);
        auto child_doc = MakeGraphDocumentWithNodes("doc_child", {});
        auto child_entry = MakeEntryWithGraphDocument(200, child_doc);
        auto entries = std::vector<TaskDto>{root_entry, child_entry};

        std::vector<TaskDto> created;
        auto                 factory = MockEntryFactory(created);
        auto result = manager.DeepCloneEntryRef(100, entries, factory);

        // created[1] is the cloned root (reverse order: child first)
        ASSERT_GE(created.size(), 2u);
        auto cloned_root_doc =
            yyjson::cast<GraphDocumentDto>(created[1].graph_document);

        // Should have 2 nodes: componentRef + entryRef
        ASSERT_EQ(cloned_root_doc.nodes.size(), 2u);

        // Find the componentRef node — should be unchanged
        bool found_component = false;
        bool found_entry_ref = false;
        for (const auto& node : cloned_root_doc.nodes)
        {
            if (node.target.target_kind == "componentRef")
            {
                found_component = true;
                ASSERT_TRUE(node.target.component_ref.has_value());
                EXPECT_EQ(
                    node.target.component_ref->component_guid,
                    "{guid-a}");
            }
            else if (node.target.target_kind == "entryRef")
            {
                found_entry_ref = true;
                ASSERT_TRUE(node.target.entry_ref.has_value());
                // entryRef should be remapped to the new child ID
                EXPECT_NE(node.target.entry_ref->entry_id, 200);
                EXPECT_EQ(
                    node.target.entry_ref->entry_id,
                    result.id_mapping[200]);
            }
        }
        EXPECT_TRUE(found_component);
        EXPECT_TRUE(found_entry_ref);
    }

    TEST_F(RefManagerLifecycleTest, ShallowCopyVsDeepClone)
    {
        // Shallow copy preserves entry_id
        EntryRefDto original{"entryRef", 42, std::nullopt, std::nullopt};
        auto        shallow = RefManager::ShallowCopyEntryRef(original);
        EXPECT_EQ(shallow.entry_id, 42);

        // Deep clone creates new entry_id
        auto root_doc = MakeGraphDocumentWithEntryRefNode("doc_root", 200);
        auto root_entry = MakeEntryWithGraphDocument(100, root_doc);
        auto child_doc = MakeGraphDocumentWithNodes("doc_child", {});
        auto child_entry = MakeEntryWithGraphDocument(200, child_doc);
        auto entries = std::vector<TaskDto>{root_entry, child_entry};

        std::vector<TaskDto> created;
        auto                 factory = MockEntryFactory(created);
        auto result = manager.DeepCloneEntryRef(100, entries, factory);

        EXPECT_NE(result.new_entry_id, 100);
    }
} // namespace
