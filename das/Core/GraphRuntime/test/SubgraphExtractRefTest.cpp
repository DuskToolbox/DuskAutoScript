/// @file SubgraphExtractRefTest.cpp
/// Integration tests for the extract→compile→ref-check pipeline.
///
/// Validates the end-to-end flow spanning ExtractSubgraph, SubgraphCompiler,
/// and RefManager:
///   - extract → recompile → execute
///   - ref-check on delete
///   - stale fingerprint → needsCompile
///   - deep clone vs shallow copy
///   - port projection through subgraph boundaries
///   - circular reference detection

#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/ExtractSubgraph.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/GraphRuntime/GraphEntryId.h>
#include <das/Core/GraphRuntime/RefManager.h>
#include <das/Core/GraphRuntime/SubgraphCompiler.h>
#include <das/Core/TaskScheduler/TaskRepositoryDtos.h>
#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace
{
    using namespace Das::Core::GraphRuntime;
    using namespace Das::Core::GraphRuntime::Dto;
    using TaskDto =
        Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto;

    // -----------------------------------------------------------------------
    // DTO helpers (same patterns as ExtractSubgraphTest / SubgraphCompilerTest)
    // -----------------------------------------------------------------------

    GraphPortDefinitionDto MakePortDef(
        const std::string& port_id,
        const std::string& port_type)
    {
        GraphPortDefinitionDto def;
        def.port_id = port_id;
        def.port_type = port_type;
        return def;
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

    GraphNodeDto MakeEntryRefNode(
        const std::string& node_id,
        int64_t            ref_entry_id,
        int64_t            revision = 0,
        const std::string& fingerprint = "")
    {
        GraphNodeDto node;
        node.node_id = node_id;
        EntryRefDto entry_ref;
        entry_ref.kind = "entryRef";
        entry_ref.entry_id = ref_entry_id;
        if (revision > 0)
        {
            entry_ref.expected_revision = revision;
        }
        if (!fingerprint.empty())
        {
            entry_ref.source_fingerprint = fingerprint;
        }
        node.target = GraphNodeTargetDto{"entryRef", std::nullopt, entry_ref};
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

    GraphDocumentDto MakeGraphDocument(
        const std::string&                  doc_id,
        std::vector<GraphNodeDto>           nodes,
        std::vector<GraphEdgeDto>           edges = {},
        std::vector<GraphPortDefinitionDto> graph_inputs = {},
        std::vector<GraphPortDefinitionDto> graph_outputs = {},
        int32_t                             version = 1,
        const std::string&                  fingerprint = "sha256:test")
    {
        GraphDocumentDto doc;
        doc.document_id = doc_id;
        doc.version = version;
        doc.fingerprint = fingerprint;
        doc.nodes = std::move(nodes);
        doc.edges = std::move(edges);
        doc.graph_inputs = std::move(graph_inputs);
        doc.graph_outputs = std::move(graph_outputs);
        return doc;
    }

    TaskDto MakeRepositoryEntry(
        int64_t                 entry_id,
        const GraphDocumentDto& graph_doc)
    {
        TaskDto entry;
        entry.entry_id = entry_id;
        entry.task_type_guid = "graph-task-guid";
        entry.graph_document = yyjson::object(graph_doc);
        return entry;
    }

    EntryAccessor MockEntryAccessor(
        const std::map<GraphEntryId, TaskDto>& entries)
    {
        return [entries](GraphEntryId id) -> std::optional<TaskDto>
        {
            auto it = entries.find(id);
            if (it != entries.end())
            {
                return it->second;
            }
            return std::nullopt;
        };
    }

    CompiledGraphPlanDto MakeCompiledPlan(int64_t entry_id)
    {
        CompiledGraphPlanDto plan;
        plan.document_id = "doc_" + std::to_string(entry_id);
        plan.source_revision = 1;
        plan.compiled_fingerprint = "fp_" + std::to_string(entry_id);
        return plan;
    }

    // -----------------------------------------------------------------------
    // Fixture for integration tests
    // -----------------------------------------------------------------------

    class SubgraphExtractRefTest : public ::testing::Test
    {
    protected:
        ExtractSubgraph  extractor;
        SubgraphCompiler compiler;
        RefManager       ref_manager;
        int64_t          next_entry_id = 1000;

        ExtractSubgraph::EntryFactory MakeMockFactory()
        {
            return [this](const TaskDto&) -> GraphEntryId
            { return next_entry_id++; };
        }

        ExtractSubgraph::CompileCallback MakeMockCompile()
        {
            return [](GraphEntryId entry_id) -> CompiledGraphPlanDto
            { return MakeCompiledPlan(entry_id); };
        }
    };

    // ===================================================================
    // 1. extract → recompile → execute
    // ===================================================================

    TEST_F(SubgraphExtractRefTest, ExtractThenRecompileProducesExecutionOrder)
    {
        // Parent: A → B → C, select B+C for extraction
        auto parent = MakeGraphDocument(
            "parent_doc",
            {MakeComponentRefNode("A", "{g-A}"),
             MakeComponentRefNode("B", "{g-B}"),
             MakeComponentRefNode("C", "{g-C}")},
            {MakeEdge("e1", "A", "out", "B", "in"),
             MakeEdge("e2", "B", "out", "C", "in")});

        // Extract B+C into a child subgraph
        auto extract_result = extractor.Extract(
            1,
            {"B", "C"},
            parent,
            MakeMockFactory(),
            MakeMockCompile());

        ASSERT_NE(extract_result.child_entry_id, 0);
        EXPECT_EQ(extract_result.child_graph_document.nodes.size(), 2u);
        EXPECT_EQ(extract_result.child_graph_document.edges.size(), 1u);

        // Register the child entry so SubgraphCompiler can find it
        auto child_entry = MakeRepositoryEntry(
            extract_result.child_entry_id,
            extract_result.child_graph_document);
        auto accessor =
            MockEntryAccessor({{extract_result.child_entry_id, child_entry}});

        // Replace B+C with entryRef in parent
        auto replace_result =
            extractor.ReplaceWithSubgraph(parent, extract_result, {"B", "C"});

        // Recompile the parent graph through SubgraphCompiler
        auto snapshot = compiler.CompileRecursive(
            replace_result.updated_graph_document,
            accessor);

        // Verify nested snapshot was produced for the child entry
        ASSERT_EQ(snapshot.nested_snapshots.size(), 1u);
        const auto& child_snapshot = snapshot.nested_snapshots[0];
        EXPECT_EQ(child_snapshot.entry_id, extract_result.child_entry_id);

        // Execution order inside the compiled child should contain
        // both extracted nodes in topological order
        EXPECT_GE(child_snapshot.compiled_plan.execution_order.size(), 2u);
    }

    TEST_F(SubgraphExtractRefTest, ExtractRecompilePreservesEdgeSemantics)
    {
        // Parent: X(out1) → Y(in1), extract Y only
        auto parent = MakeGraphDocument(
            "parent_e2e",
            {MakeComponentRefNode("X", "{g-X}"),
             MakeComponentRefNode("Y", "{g-Y}")},
            {MakeEdge("e1", "X", "out1", "Y", "in1")},
            {MakePortDef("in1", "number")});

        auto extract_result = extractor.Extract(
            1,
            {"Y"},
            parent,
            MakeMockFactory(),
            MakeMockCompile());

        auto child_entry = MakeRepositoryEntry(
            extract_result.child_entry_id,
            extract_result.child_graph_document);
        auto accessor =
            MockEntryAccessor({{extract_result.child_entry_id, child_entry}});

        auto replace_result =
            extractor.ReplaceWithSubgraph(parent, extract_result, {"Y"});

        // Parent now has: X → replacement_node (entryRef to child)
        EXPECT_EQ(replace_result.updated_graph_document.edges.size(), 1u);
        const auto& rewired = replace_result.updated_graph_document.edges[0];
        EXPECT_EQ(rewired.source_node_id, "X");
        EXPECT_EQ(rewired.target_node_id, replace_result.replacement_node_id);

        // Recompile should succeed
        auto snapshot = compiler.CompileRecursive(
            replace_result.updated_graph_document,
            accessor);
        ASSERT_EQ(snapshot.nested_snapshots.size(), 1u);
        EXPECT_FALSE(
            snapshot.nested_snapshots[0].compiled_plan.execution_order.empty());
    }

    // ===================================================================
    // 2. ref-check on delete
    // ===================================================================

    TEST_F(SubgraphExtractRefTest, RefCheckBlocksDeleteAfterExtract)
    {
        // Parent: A, B, C. Extract B into child.
        auto parent = MakeGraphDocument(
            "parent_ref",
            {MakeComponentRefNode("A", "{gA}"),
             MakeComponentRefNode("B", "{gB}"),
             MakeComponentRefNode("C", "{gC}")});

        auto extract_result = extractor.Extract(
            1,
            {"B"},
            parent,
            MakeMockFactory(),
            MakeMockCompile());

        auto replace_result =
            extractor.ReplaceWithSubgraph(parent, extract_result, {"B"});

        // Build entries list for RefManager
        auto child_entry = MakeRepositoryEntry(
            extract_result.child_entry_id,
            extract_result.child_graph_document);
        auto parent_entry =
            MakeRepositoryEntry(1, replace_result.updated_graph_document);

        std::vector<TaskDto> all_entries = {parent_entry, child_entry};

        // The child entry is referenced by the parent via entryRef
        auto check =
            ref_manager.CheckDelete(extract_result.child_entry_id, all_entries);
        EXPECT_FALSE(check.allowed);
        EXPECT_GE(check.ref_count, 1);
    }

    TEST_F(SubgraphExtractRefTest, RefCheckAllowsDeleteAfterUndo)
    {
        auto parent = MakeGraphDocument(
            "parent_undo_ref",
            {MakeComponentRefNode("A", "{gA}"),
             MakeComponentRefNode("B", "{gB}"),
             MakeComponentRefNode("C", "{gC}")});

        auto extract_result = extractor.Extract(
            1,
            {"B"},
            parent,
            MakeMockFactory(),
            MakeMockCompile());

        auto replace_result =
            extractor.ReplaceWithSubgraph(parent, extract_result, {"B"});

        // Undo the extract — restores original graph, removes entryRef
        auto delete_cb = [](GraphEntryId) -> bool { return true; };
        auto ref_count_cb = [](GraphEntryId) -> int { return 0; };

        auto restored = extractor.UndoExtract(
            replace_result.undo_snapshot,
            delete_cb,
            ref_count_cb);

        // After undo, the restored document has no entryRef
        bool has_entry_ref = false;
        for (const auto& node : restored.nodes)
        {
            if (node.target.target_kind == "entryRef")
            {
                has_entry_ref = true;
            }
        }
        EXPECT_FALSE(has_entry_ref);

        // If we build entries from the restored state, the child is no
        // longer referenced.
        auto                 restored_entry = MakeRepositoryEntry(1, restored);
        std::vector<TaskDto> entries_after_undo = {restored_entry};

        auto check = ref_manager.CheckDelete(
            extract_result.child_entry_id,
            entries_after_undo);
        EXPECT_TRUE(check.allowed);
        EXPECT_EQ(check.ref_count, 0);
    }

    TEST_F(SubgraphExtractRefTest, RefCheckReportsReferencingDocumentIds)
    {
        auto parent = MakeGraphDocument(
            "doc_alpha",
            {MakeComponentRefNode("A", "{gA}"),
             MakeComponentRefNode("B", "{gB}"),
             MakeComponentRefNode("C", "{gC}")});

        auto extract_result = extractor.Extract(
            1,
            {"B"},
            parent,
            MakeMockFactory(),
            MakeMockCompile());

        auto replace_result =
            extractor.ReplaceWithSubgraph(parent, extract_result, {"B"});

        auto parent_entry =
            MakeRepositoryEntry(1, replace_result.updated_graph_document);

        auto check = ref_manager.CheckDelete(
            extract_result.child_entry_id,
            {parent_entry});

        EXPECT_FALSE(check.allowed);
        EXPECT_FALSE(check.referencing_document_ids.empty());
    }

    // ===================================================================
    // 3. stale fingerprint → needsCompile
    // ===================================================================

    TEST_F(SubgraphExtractRefTest, StaleFingerprintDetectedAfterEdit)
    {
        // Extract B from parent into child
        auto parent_v1 = MakeGraphDocument(
            "parent_fp",
            {MakeComponentRefNode("A", "{gA}"),
             MakeComponentRefNode("B", "{gB}"),
             MakeComponentRefNode("C", "{gC}")},
            {},
            {},
            {},
            1,
            "fp_v1");

        auto extract_result = extractor.Extract(
            1,
            {"B"},
            parent_v1,
            MakeMockFactory(),
            MakeMockCompile());

        // The extract result captures the child's fingerprint at extract time
        EXPECT_FALSE(extract_result.fingerprint.empty());

        // Replace and get the entryRef node
        auto replace_result =
            extractor.ReplaceWithSubgraph(parent_v1, extract_result, {"B"});

        // Find the entryRef node in the replaced graph
        const EntryRefDto* entry_ref = nullptr;
        for (const auto& node : replace_result.updated_graph_document.nodes)
        {
            if (node.target.target_kind == "entryRef"
                && node.target.entry_ref.has_value())
            {
                entry_ref = &node.target.entry_ref.value();
                break;
            }
        }
        ASSERT_NE(entry_ref, nullptr);

        // The entryRef pins the source_fingerprint from the child at
        // extraction time
        ASSERT_TRUE(entry_ref->source_fingerprint.has_value());

        // Now "edit" the child graph document — change its fingerprint
        auto edited_child_doc = extract_result.child_graph_document;
        edited_child_doc.fingerprint = "fp_v2_edited";
        edited_child_doc.version = 2;

        auto edited_child_entry = MakeRepositoryEntry(
            extract_result.child_entry_id,
            edited_child_doc);
        auto accessor = MockEntryAccessor(
            {{extract_result.child_entry_id, edited_child_entry}});

        // Recompile via SubgraphCompiler — the snapshot should reflect the
        // new fingerprint, not the pinned one
        auto snapshot = compiler.CompileEntryRef(
            extract_result.child_entry_id,
            replace_result.updated_graph_document,
            accessor);

        // The SubgraphCompileResultDto pins the current document fingerprint
        EXPECT_EQ(snapshot.source_fingerprint, "fp_v2_edited");
        EXPECT_NE(
            snapshot.source_fingerprint,
            entry_ref->source_fingerprint.value());
    }

    TEST_F(SubgraphExtractRefTest, FingerprintMatchWhenNoEdits)
    {
        auto parent = MakeGraphDocument(
            "parent_fp_match",
            {MakeComponentRefNode("A", "{gA}"),
             MakeComponentRefNode("B", "{gB}"),
             MakeComponentRefNode("C", "{gC}")});

        auto extract_result = extractor.Extract(
            1,
            {"B"},
            parent,
            MakeMockFactory(),
            MakeMockCompile());

        auto replace_result =
            extractor.ReplaceWithSubgraph(parent, extract_result, {"B"});

        // Register the unedited child
        auto child_entry = MakeRepositoryEntry(
            extract_result.child_entry_id,
            extract_result.child_graph_document);
        auto accessor =
            MockEntryAccessor({{extract_result.child_entry_id, child_entry}});

        // Compile and check fingerprint matches
        auto snapshot = compiler.CompileEntryRef(
            extract_result.child_entry_id,
            replace_result.updated_graph_document,
            accessor);

        // Find the entryRef to get the pinned fingerprint
        std::string pinned_fp;
        for (const auto& node : replace_result.updated_graph_document.nodes)
        {
            if (node.target.target_kind == "entryRef"
                && node.target.entry_ref.has_value()
                && node.target.entry_ref->source_fingerprint.has_value())
            {
                pinned_fp = node.target.entry_ref->source_fingerprint.value();
                break;
            }
        }

        // Fingerprint should match — no edits made
        if (!pinned_fp.empty())
        {
            EXPECT_EQ(snapshot.source_fingerprint, pinned_fp);
        }
    }

    // ===================================================================
    // 4. deep clone vs shallow copy
    // ===================================================================

    TEST_F(SubgraphExtractRefTest, ShallowCopyPreservesEntryId)
    {
        EntryRefDto original{
            "entryRef",
            42,
            int64_t{7},
            std::string{"sha256:abc"}};

        auto copy = RefManager::ShallowCopyEntryRef(original);

        EXPECT_EQ(copy.entry_id, 42);
        ASSERT_TRUE(copy.expected_revision.has_value());
        EXPECT_EQ(copy.expected_revision.value(), 7);
        ASSERT_TRUE(copy.source_fingerprint.has_value());
        EXPECT_EQ(copy.source_fingerprint.value(), "sha256:abc");
    }

    TEST_F(SubgraphExtractRefTest, DeepCloneCreatesNewEntryIds)
    {
        // Build: root (100) → child (200) via entryRef
        auto child_doc =
            MakeGraphDocument("child_doc", {MakeComponentRefNode("X", "{gX}")});
        auto root_doc = MakeGraphDocument(
            "root_doc",
            {MakeComponentRefNode("A", "{gA}"),
             MakeEntryRefNode("ref_node", 200)});

        auto root_entry = MakeRepositoryEntry(100, root_doc);
        auto child_entry = MakeRepositoryEntry(200, child_doc);

        std::vector<TaskDto> all_entries = {root_entry, child_entry};

        std::vector<TaskDto> created;
        auto factory = [&created](const TaskDto& src) mutable -> GraphEntryId
        {
            static int64_t next = 5000;
            TaskDto        copy = src;
            copy.entry_id = next++;
            created.push_back(copy);
            return copy.entry_id;
        };

        auto result = ref_manager.DeepCloneEntryRef(100, all_entries, factory);

        // New root entry_id is different
        EXPECT_NE(result.new_entry_id, 100);

        // id_mapping contains both root and child
        ASSERT_EQ(result.id_mapping.size(), 2u);
        EXPECT_NE(result.id_mapping[100], 100);
        EXPECT_NE(result.id_mapping[200], 200);

        // The cloned root's graph_document has entryRef remapped to new child
        ASSERT_GE(created.size(), 2u);
        auto cloned_root_doc =
            yyjson::cast<GraphDocumentDto>(created[1].graph_document);
        bool found_remapped_ref = false;
        for (const auto& node : cloned_root_doc.nodes)
        {
            if (node.target.target_kind == "entryRef"
                && node.target.entry_ref.has_value())
            {
                found_remapped_ref = true;
                EXPECT_EQ(
                    node.target.entry_ref->entry_id,
                    result.id_mapping[200]);
                EXPECT_NE(node.target.entry_ref->entry_id, 200);
            }
        }
        EXPECT_TRUE(found_remapped_ref);
    }

    TEST_F(SubgraphExtractRefTest, DeepClonePreservesComponentRefNodes)
    {
        // Root with both componentRef and entryRef nodes
        auto child_doc = MakeGraphDocument(
            "child_leaf",
            {MakeComponentRefNode("Z", "{gZ}")});
        auto root_doc = MakeGraphDocument(
            "root_mixed",
            {MakeComponentRefNode("comp", "{guid-comp}"),
             MakeEntryRefNode("entry_ref", 200)});

        auto root_entry = MakeRepositoryEntry(100, root_doc);
        auto child_entry = MakeRepositoryEntry(200, child_doc);

        std::vector<TaskDto> all_entries = {root_entry, child_entry};

        std::vector<TaskDto> created;
        auto factory = [&created](const TaskDto& src) mutable -> GraphEntryId
        {
            static int64_t next = 6000;
            TaskDto        copy = src;
            copy.entry_id = next++;
            created.push_back(copy);
            return copy.entry_id;
        };

        auto result = ref_manager.DeepCloneEntryRef(100, all_entries, factory);

        auto cloned_root_doc =
            yyjson::cast<GraphDocumentDto>(created[1].graph_document);

        ASSERT_EQ(cloned_root_doc.nodes.size(), 2u);

        bool found_comp = false;
        bool found_ref = false;
        for (const auto& node : cloned_root_doc.nodes)
        {
            if (node.target.target_kind == "componentRef")
            {
                found_comp = true;
                ASSERT_TRUE(node.target.component_ref.has_value());
                EXPECT_EQ(
                    node.target.component_ref->component_guid,
                    "{guid-comp}");
            }
            else if (node.target.target_kind == "entryRef")
            {
                found_ref = true;
                ASSERT_TRUE(node.target.entry_ref.has_value());
                EXPECT_EQ(
                    node.target.entry_ref->entry_id,
                    result.id_mapping[200]);
            }
        }
        EXPECT_TRUE(found_comp);
        EXPECT_TRUE(found_ref);
    }

    // ===================================================================
    // 5. port projection
    // ===================================================================

    TEST_F(SubgraphExtractRefTest, PortProjectionThroughExtractAndCompile)
    {
        // Parent: outer_in → A(selected) → outer_out
        // After extraction, the child subgraph should have matching ports
        auto parent = MakeGraphDocument(
            "parent_port",
            {MakeComponentRefNode("outside", "{g-out}"),
             MakeComponentRefNode("selected", "{g-sel}")},
            {MakeEdge("e1", "outside", "out_port", "selected", "in_port")},
            {MakePortDef("in_port", "image")},
            {MakePortDef("out_port", "image")});

        // Extract "selected" into child
        auto extract_result = extractor.Extract(
            1,
            {"selected"},
            parent,
            MakeMockFactory(),
            MakeMockCompile());

        // Child should have 1 input boundary port
        ASSERT_EQ(extract_result.input_port_mappings.size(), 1u);
        EXPECT_EQ(extract_result.input_port_mappings[0].port_type, "image");

        // Now create an outer doc with graph_inputs for port projection
        // Simulate: parent with entryRef node (after replace)
        auto child_doc = extract_result.child_graph_document;

        // Add dynamic_ports to the child's root node for projection
        // The child has one node (remapped "selected"), make it have an
        // input port matching the outer graph_input type
        GraphNodeDto root_with_port;
        root_with_port.node_id = "R";
        root_with_port.target.target_kind = "componentRef";
        root_with_port.target.component_ref =
            ComponentRefDto{"componentRef", "{g-R}", "{plugin-guid}"};
        root_with_port.dynamic_ports.push_back(MakePortDef("img_in", "image"));

        auto inner_doc = MakeGraphDocument(
            "inner_doc",
            {root_with_port},
            {},
            {},
            {},
            1,
            "fp_inner");

        auto outer_doc = MakeGraphDocument(
            "outer_doc",
            {MakeEntryRefNode("sub_ref", 500)},
            {},
            {MakePortDef("outer_image", "image")},
            {MakePortDef("outer_result", "image")});

        auto inner_entry = MakeRepositoryEntry(500, inner_doc);
        auto accessor = MockEntryAccessor({{500, inner_entry}});

        auto snapshot = compiler.CompileEntryRef(500, outer_doc, accessor);

        // Verify input port projection: outer_image → img_in
        ASSERT_EQ(snapshot.input_mapping.size(), 1u);
        EXPECT_EQ(snapshot.input_mapping[0].outer_port_id, "outer_image");
        EXPECT_EQ(snapshot.input_mapping[0].inner_port_id, "img_in");
        EXPECT_EQ(snapshot.input_mapping[0].node_id, "R");
        EXPECT_EQ(snapshot.input_mapping[0].port_type, "image");
    }

    TEST_F(SubgraphExtractRefTest, OutputPortProjectionThroughSubgraph)
    {
        // Inner graph: terminal node T with output port "result_out"
        GraphNodeDto terminal;
        terminal.node_id = "T";
        terminal.target.target_kind = "componentRef";
        terminal.target.component_ref =
            ComponentRefDto{"componentRef", "{g-T}", "{plugin-guid}"};
        terminal.dynamic_ports.push_back(MakePortDef("result_out", "string"));

        auto inner_doc = MakeGraphDocument("inner_out", {terminal});

        auto outer_doc = MakeGraphDocument(
            "outer_out",
            {MakeEntryRefNode("ref", 600)},
            {},
            {},
            {MakePortDef("outer_str", "string")});

        auto inner_entry = MakeRepositoryEntry(600, inner_doc);
        auto accessor = MockEntryAccessor({{600, inner_entry}});

        auto snapshot = compiler.CompileEntryRef(600, outer_doc, accessor);

        ASSERT_EQ(snapshot.output_mapping.size(), 1u);
        EXPECT_EQ(snapshot.output_mapping[0].outer_port_id, "outer_str");
        EXPECT_EQ(snapshot.output_mapping[0].inner_port_id, "result_out");
        EXPECT_EQ(snapshot.output_mapping[0].node_id, "T");
        EXPECT_EQ(snapshot.output_mapping[0].port_type, "string");
    }

    TEST_F(SubgraphExtractRefTest, PortProjectionMultiplePorts)
    {
        // Inner graph: 2 root nodes with different port types
        GraphNodeDto r1;
        r1.node_id = "R1";
        r1.target.target_kind = "componentRef";
        r1.target.component_ref =
            ComponentRefDto{"componentRef", "{g-R1}", "{plugin-guid}"};
        r1.dynamic_ports.push_back(MakePortDef("img_port", "image"));

        GraphNodeDto r2;
        r2.node_id = "R2";
        r2.target.target_kind = "componentRef";
        r2.target.component_ref =
            ComponentRefDto{"componentRef", "{g-R2}", "{plugin-guid}"};
        r2.dynamic_ports.push_back(MakePortDef("num_port", "number"));

        auto inner_doc = MakeGraphDocument("inner_multi", {r1, r2});

        auto outer_doc = MakeGraphDocument(
            "outer_multi",
            {MakeEntryRefNode("sub", 700)},
            {},
            {MakePortDef("outer_img", "image"),
             MakePortDef("outer_num", "number")});

        auto inner_entry = MakeRepositoryEntry(700, inner_doc);
        auto accessor = MockEntryAccessor({{700, inner_entry}});

        auto snapshot = compiler.CompileEntryRef(700, outer_doc, accessor);

        EXPECT_EQ(snapshot.input_mapping.size(), 2u);

        std::set<std::string> mapped_outer;
        for (const auto& m : snapshot.input_mapping)
        {
            mapped_outer.insert(m.outer_port_id);
        }
        EXPECT_TRUE(mapped_outer.count("outer_img"));
        EXPECT_TRUE(mapped_outer.count("outer_num"));
    }

    // ===================================================================
    // 6. circular ref detection
    // ===================================================================

    TEST_F(SubgraphExtractRefTest, CircularRefDetectedBySubgraphCompiler)
    {
        // Entry A (entryRef→B) and Entry B (entryRef→A)
        auto doc_a = MakeGraphDocument(
            "doc_a",
            {MakeComponentRefNode("A_node", "{gA}"),
             MakeEntryRefNode("A_ref", 200)});

        auto doc_b = MakeGraphDocument(
            "doc_b",
            {MakeComponentRefNode("B_node", "{gB}"),
             MakeEntryRefNode("B_ref", 100)});

        auto entry_100 = MakeRepositoryEntry(100, doc_a);
        auto entry_200 = MakeRepositoryEntry(200, doc_b);

        auto accessor = MockEntryAccessor({{100, entry_100}, {200, entry_200}});

        auto snapshot = compiler.CompileRecursive(doc_a, accessor);

        // Should produce snapshots but also detect the cycle
        EXPECT_FALSE(snapshot.nested_snapshots.empty());

        // Walk all snapshots looking for a cycle diagnostic
        bool has_cycle_diagnostic = false;
        std::function<void(const SubgraphCompileResultDto&)> check_diags;
        check_diags = [&](const SubgraphCompileResultDto& snap)
        {
            for (const auto& diag : snap.diagnostics)
            {
                if (diag.find("cyclic") != std::string::npos)
                {
                    has_cycle_diagnostic = true;
                }
            }
            for (const auto& nested : snap.nested_snapshots)
            {
                check_diags(nested);
            }
        };
        for (const auto& ns : snapshot.nested_snapshots)
        {
            check_diags(ns);
        }
        EXPECT_TRUE(has_cycle_diagnostic);
    }

    TEST_F(SubgraphExtractRefTest, SelfReferenceDetectedBySubgraphCompiler)
    {
        // Entry 500 references itself
        auto doc_self = MakeGraphDocument(
            "doc_self",
            {MakeComponentRefNode("self_node", "{g-self}"),
             MakeEntryRefNode("self_ref", 500)});

        auto entry_500 = MakeRepositoryEntry(500, doc_self);
        auto accessor = MockEntryAccessor({{500, entry_500}});

        auto snapshot = compiler.CompileRecursive(doc_self, accessor);

        bool has_cycle_diag = false;
        std::function<void(const SubgraphCompileResultDto&)> find_cycle;
        find_cycle = [&](const SubgraphCompileResultDto& snap)
        {
            for (const auto& diag : snap.diagnostics)
            {
                if (diag.find("cyclic") != std::string::npos)
                {
                    has_cycle_diag = true;
                }
            }
            for (const auto& nested : snap.nested_snapshots)
            {
                find_cycle(nested);
            }
        };
        find_cycle(snapshot);
        EXPECT_TRUE(has_cycle_diag);
    }

    TEST_F(SubgraphExtractRefTest, CircularRefFromExtractedSubgraphs)
    {
        // Create two subgraphs that reference each other:
        // entry 100: entryRef → 200
        // entry 200: entryRef → 100
        auto doc_100 = MakeGraphDocument(
            "doc_100",
            {MakeComponentRefNode("A", "{gA}"),
             MakeEntryRefNode("ref_to_200", 200)});

        auto doc_200 = MakeGraphDocument(
            "doc_200",
            {MakeComponentRefNode("B", "{gB}"),
             MakeEntryRefNode("ref_to_100", 100)});

        auto entry_100 = MakeRepositoryEntry(100, doc_100);
        auto entry_200 = MakeRepositoryEntry(200, doc_200);
        auto accessor = MockEntryAccessor({{100, entry_100}, {200, entry_200}});

        auto snapshot = compiler.CompileRecursive(doc_100, accessor);

        // Should detect the mutual reference cycle
        bool has_cycle_diagnostic = false;
        std::function<void(const SubgraphCompileResultDto&)> check;
        check = [&](const SubgraphCompileResultDto& snap)
        {
            for (const auto& diag : snap.diagnostics)
            {
                if (diag.find("cyclic") != std::string::npos)
                {
                    has_cycle_diagnostic = true;
                }
            }
            for (const auto& nested : snap.nested_snapshots)
            {
                check(nested);
            }
        };
        check(snapshot);
        EXPECT_TRUE(has_cycle_diagnostic);
    }

    TEST_F(SubgraphExtractRefTest, NoCycleDiagnosticForAcyclicRefs)
    {
        // Acyclic: outer → entry 300 → entry 400 (leaf)
        auto leaf_doc =
            MakeGraphDocument("leaf", {MakeComponentRefNode("Z", "{gZ}")});

        auto mid_doc = MakeGraphDocument(
            "mid",
            {MakeComponentRefNode("Y", "{gY}"),
             MakeEntryRefNode("Y_ref", 400)});

        auto outer_doc = MakeGraphDocument(
            "outer",
            {MakeComponentRefNode("X", "{gX}"),
             MakeEntryRefNode("X_ref", 300)});

        auto entry_400 = MakeRepositoryEntry(400, leaf_doc);
        auto entry_300 = MakeRepositoryEntry(300, mid_doc);

        auto accessor = MockEntryAccessor({{300, entry_300}, {400, entry_400}});

        auto snapshot = compiler.CompileRecursive(outer_doc, accessor);

        // Should have nested snapshots (300 and 400) but NO cycle diagnostics
        bool has_cycle_diagnostic = false;
        std::function<void(const SubgraphCompileResultDto&)> check;
        check = [&](const SubgraphCompileResultDto& snap)
        {
            for (const auto& diag : snap.diagnostics)
            {
                if (diag.find("cyclic") != std::string::npos)
                {
                    has_cycle_diagnostic = true;
                }
            }
            for (const auto& nested : snap.nested_snapshots)
            {
                check(nested);
            }
        };
        check(snapshot);
        EXPECT_FALSE(has_cycle_diagnostic);

        // Verify correct nesting depth
        ASSERT_EQ(snapshot.nested_snapshots.size(), 1u);
        EXPECT_EQ(snapshot.nested_snapshots[0].entry_id, 300);
        ASSERT_EQ(snapshot.nested_snapshots[0].nested_snapshots.size(), 1u);
        EXPECT_EQ(
            snapshot.nested_snapshots[0].nested_snapshots[0].entry_id,
            400);
    }

    // ===================================================================
    // Edge cases: extract→ref-check integration
    // ===================================================================

    TEST_F(
        SubgraphExtractRefTest,
        MultipleExtractsIncrementRefCountOnSharedChild)
    {
        // Extract B from parent_1 → child_1
        // Extract C from parent_2 → child_2, but parent_2 also references
        // child_1
        auto parent1 = MakeGraphDocument(
            "parent1_multi",
            {MakeComponentRefNode("A", "{gA}"),
             MakeComponentRefNode("B", "{gB}"),
             MakeComponentRefNode("C", "{gC}")});

        auto extract1 = extractor.Extract(
            1,
            {"B"},
            parent1,
            MakeMockFactory(),
            MakeMockCompile());

        auto replace1 = extractor.ReplaceWithSubgraph(parent1, extract1, {"B"});

        // Create a second parent that references child_1 via entryRef
        auto parent2 = MakeGraphDocument(
            "parent2_multi",
            {MakeComponentRefNode("D", "{gD}"),
             MakeEntryRefNode("ref_to_child1", extract1.child_entry_id),
             MakeComponentRefNode("E", "{gE}")});

        auto child1_entry = MakeRepositoryEntry(
            extract1.child_entry_id,
            extract1.child_graph_document);
        auto parent1_entry =
            MakeRepositoryEntry(1, replace1.updated_graph_document);
        auto parent2_entry = MakeRepositoryEntry(2, parent2);

        std::vector<TaskDto> all_entries = {
            parent1_entry,
            parent2_entry,
            child1_entry};

        // child_1 is referenced by both parent1 and parent2
        auto check =
            ref_manager.CheckDelete(extract1.child_entry_id, all_entries);
        EXPECT_FALSE(check.allowed);
        EXPECT_GE(check.ref_count, 2);
    }

} // namespace
