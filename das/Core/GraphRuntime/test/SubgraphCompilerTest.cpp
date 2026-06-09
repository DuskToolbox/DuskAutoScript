#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/GraphRuntime/GraphEntryId.h>
#include <das/Core/GraphRuntime/SubgraphCompiler.h>
#include <das/Core/TaskScheduler/TaskRepositoryDtos.h>
#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <map>
#include <memory>
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

    // ---------------------------------------------------------------
    // Test helpers
    // ---------------------------------------------------------------

    GraphPortDefinitionDto MakePortDef(
        const std::string& port_id,
        const std::string& port_type)
    {
        GraphPortDefinitionDto port;
        port.port_id = port_id;
        port.port_type = port_type;
        return port;
    }

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

    GraphNodeDto MakeEntryRefNode(
        const std::string& node_id,
        int64_t            entry_id,
        int64_t            revision = 0,
        const std::string& fingerprint = "")
    {
        GraphNodeDto node;
        node.node_id = node_id;
        node.target.target_kind = "entryRef";
        EntryRefDto entry_ref;
        entry_ref.kind = "entryRef";
        entry_ref.entry_id = entry_id;
        if (revision > 0)
        {
            entry_ref.expected_revision = revision;
        }
        if (!fingerprint.empty())
        {
            entry_ref.source_fingerprint = fingerprint;
        }
        node.target.entry_ref = entry_ref;
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
        std::vector<GraphNodeDto>           nodes,
        std::vector<GraphEdgeDto>           edges = {},
        std::vector<GraphPortDefinitionDto> graph_inputs = {},
        std::vector<GraphPortDefinitionDto> graph_outputs = {})
    {
        GraphDocumentDto doc;
        doc.document_id = "test_doc";
        doc.version = 1;
        doc.fingerprint = "fp_test";
        doc.nodes = std::move(nodes);
        doc.edges = std::move(edges);
        doc.graph_inputs = std::move(graph_inputs);
        doc.graph_outputs = std::move(graph_outputs);
        return doc;
    }

    TaskDto MakeRepositoryEntry(
        int64_t                 entry_id,
        const GraphDocumentDto& graph_doc,
        const yyjson::value&    compiled_artifact = yyjson::value{})
    {
        TaskDto entry;
        entry.entry_id = entry_id;
        entry.task_type_guid = "graph-task-guid";
        entry.graph_document = yyjson::object(graph_doc);
        entry.compiled_artifact = compiled_artifact;
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

    // SubgraphCompiler uses real GraphCompiler internally.
    // GraphCompiler::Compile() works without a factory manager — it
    // produces execution_order via topological sort and generates
    // a binding plan from edges (port types may be empty).

    // ===================================================================
    // Test Suite: SubgraphCompilerTest
    // ===================================================================

    TEST(SubgraphCompilerTest, CompileEntryRefSingleSubgraph)
    {
        // Outer graph: node A (componentRef) + node B (entryRef→100)
        auto outer_doc = MakeGraphDocument(
            {MakeComponentNode("A", "guid-A"), MakeEntryRefNode("B", 100)});

        // Inner graph (entry 100): 2 componentRef nodes
        auto inner_doc = MakeGraphDocument(
            {MakeComponentNode("X", "guid-X"),
             MakeComponentNode("Y", "guid-Y")},
            {MakeEdge("e1", "X", "out", "Y", "in")});

        auto entry_100 = MakeRepositoryEntry(100, inner_doc);
        auto accessor = MockEntryAccessor({{100, entry_100}});

        SubgraphCompiler compiler;
        auto snapshot = compiler.CompileEntryRef(100, outer_doc, accessor);

        EXPECT_EQ(snapshot.entry_id, 100);
        EXPECT_TRUE(snapshot.nested_snapshots.empty());
        EXPECT_FALSE(snapshot.compiled_plan.execution_order.empty());
    }

    TEST(SubgraphCompilerTest, CompileEntryRefProducesCompiledPlan)
    {
        auto outer_doc = MakeGraphDocument({MakeEntryRefNode("B", 100)});

        auto inner_doc = MakeGraphDocument(
            {MakeComponentNode("X", "guid-X"),
             MakeComponentNode("Y", "guid-Y")},
            {MakeEdge("e1", "X", "out", "Y", "in")});

        auto entry_100 = MakeRepositoryEntry(100, inner_doc);
        auto accessor = MockEntryAccessor({{100, entry_100}});

        SubgraphCompiler compiler;
        auto snapshot = compiler.CompileEntryRef(100, outer_doc, accessor);

        // execution_order should contain all inner nodes in topological order
        ASSERT_EQ(snapshot.compiled_plan.execution_order.size(), 2u);
        EXPECT_EQ(snapshot.compiled_plan.execution_order[0], "X");
        EXPECT_EQ(snapshot.compiled_plan.execution_order[1], "Y");

        // binding_plan: without a factory manager, GraphCompiler may not
        // generate full bindings — just verify the plan was produced.
        EXPECT_FALSE(snapshot.compiled_plan.execution_order.empty());
    }

    TEST(SubgraphCompilerTest, InputMappingSingleRootNode)
    {
        // Outer graph has graph_input "outer_img"
        // Inner graph has root node R with input port "img_in"
        GraphNodeDto root_node = MakeComponentNode("R", "guid-R");
        root_node.dynamic_ports.push_back(MakePortDef("img_in", "image"));

        auto outer_doc = MakeGraphDocument(
            {MakeEntryRefNode("sub", 100)},
            {},
            {MakePortDef("outer_img", "image")});

        auto inner_doc = MakeGraphDocument({root_node});

        auto entry_100 = MakeRepositoryEntry(100, inner_doc);
        auto accessor = MockEntryAccessor({{100, entry_100}});

        SubgraphCompiler compiler;
        auto snapshot = compiler.CompileEntryRef(100, outer_doc, accessor);

        ASSERT_EQ(snapshot.input_mapping.size(), 1u);
        EXPECT_EQ(snapshot.input_mapping[0].outer_port_id, "outer_img");
        EXPECT_EQ(snapshot.input_mapping[0].inner_port_id, "img_in");
        EXPECT_EQ(snapshot.input_mapping[0].node_id, "R");
    }

    TEST(SubgraphCompilerTest, InputMappingMultipleRootNodes)
    {
        // Inner graph has 2 root nodes R1 (input "a") and R2 (input "b")
        GraphNodeDto r1 = MakeComponentNode("R1", "guid-R1");
        r1.dynamic_ports.push_back(MakePortDef("a", "int"));

        GraphNodeDto r2 = MakeComponentNode("R2", "guid-R2");
        r2.dynamic_ports.push_back(MakePortDef("b", "string"));

        auto outer_doc = MakeGraphDocument(
            {MakeEntryRefNode("sub", 100)},
            {},
            {MakePortDef("outer_a", "int"), MakePortDef("outer_b", "string")});

        auto inner_doc = MakeGraphDocument({r1, r2});

        auto entry_100 = MakeRepositoryEntry(100, inner_doc);
        auto accessor = MockEntryAccessor({{100, entry_100}});

        SubgraphCompiler compiler;
        auto snapshot = compiler.CompileEntryRef(100, outer_doc, accessor);

        EXPECT_EQ(snapshot.input_mapping.size(), 2u);

        // Verify both mappings exist (order may vary)
        std::set<std::string> mapped_outer;
        for (const auto& m : snapshot.input_mapping)
        {
            mapped_outer.insert(m.outer_port_id);
        }
        EXPECT_TRUE(mapped_outer.count("outer_a"));
        EXPECT_TRUE(mapped_outer.count("outer_b"));
    }

    TEST(SubgraphCompilerTest, OutputMappingSingleTerminalNode)
    {
        // Inner graph has terminal node T (out_degree 0) with output port
        // "result_out" Outer graph_output "outer_result"
        GraphNodeDto terminal = MakeComponentNode("T", "guid-T");
        terminal.dynamic_ports.push_back(MakePortDef("result_out", "image"));

        auto outer_doc = MakeGraphDocument(
            {MakeEntryRefNode("sub", 100)},
            {},
            {},
            {MakePortDef("outer_result", "image")});

        auto inner_doc = MakeGraphDocument({terminal});

        auto entry_100 = MakeRepositoryEntry(100, inner_doc);
        auto accessor = MockEntryAccessor({{100, entry_100}});

        SubgraphCompiler compiler;
        auto snapshot = compiler.CompileEntryRef(100, outer_doc, accessor);

        ASSERT_EQ(snapshot.output_mapping.size(), 1u);
        EXPECT_EQ(snapshot.output_mapping[0].inner_port_id, "result_out");
        EXPECT_EQ(snapshot.output_mapping[0].outer_port_id, "outer_result");
        EXPECT_EQ(snapshot.output_mapping[0].node_id, "T");
    }

    TEST(SubgraphCompilerTest, OutputMappingMultipleTerminalNodes)
    {
        // Inner graph has 2 terminals T1 ("out_1"), T2 ("out_2")
        GraphNodeDto t1 = MakeComponentNode("T1", "guid-T1");
        t1.dynamic_ports.push_back(MakePortDef("out_1", "int"));

        GraphNodeDto t2 = MakeComponentNode("T2", "guid-T2");
        t2.dynamic_ports.push_back(MakePortDef("out_2", "string"));

        auto outer_doc = MakeGraphDocument(
            {MakeEntryRefNode("sub", 100)},
            {},
            {},
            {MakePortDef("outer_1", "int"), MakePortDef("outer_2", "string")});

        auto inner_doc = MakeGraphDocument({t1, t2});

        auto entry_100 = MakeRepositoryEntry(100, inner_doc);
        auto accessor = MockEntryAccessor({{100, entry_100}});

        SubgraphCompiler compiler;
        auto snapshot = compiler.CompileEntryRef(100, outer_doc, accessor);

        EXPECT_EQ(snapshot.output_mapping.size(), 2u);

        std::set<std::string> mapped_outer;
        for (const auto& m : snapshot.output_mapping)
        {
            mapped_outer.insert(m.outer_port_id);
        }
        EXPECT_TRUE(mapped_outer.count("outer_1"));
        EXPECT_TRUE(mapped_outer.count("outer_2"));
    }

    TEST(SubgraphCompilerTest, CompileRecursiveMultiLevel)
    {
        // Level 0 (outer): entryRef → entry 200
        // Level 1 (entry 200): entryRef → entry 300
        // Level 2 (entry 300): componentRef only

        auto level2_doc =
            MakeGraphDocument({MakeComponentNode("L2_A", "g-L2")});

        auto level1_doc = MakeGraphDocument(
            {MakeComponentNode("L1_A", "g-L1"),
             MakeEntryRefNode("L1_sub", 300)},
            {MakeEdge("e1", "L1_A", "out", "L1_sub", "in")});

        auto outer_doc = MakeGraphDocument(
            {MakeComponentNode("L0_A", "g-L0"),
             MakeEntryRefNode("L0_sub", 200)},
            {MakeEdge("e0", "L0_A", "out", "L0_sub", "in")});

        auto entry_300 = MakeRepositoryEntry(300, level2_doc);
        auto entry_200 = MakeRepositoryEntry(200, level1_doc);

        auto accessor = MockEntryAccessor({{200, entry_200}, {300, entry_300}});

        SubgraphCompiler compiler;
        auto snapshot = compiler.CompileRecursive(outer_doc, accessor);

        // Should have snapshot for entry 200
        ASSERT_EQ(snapshot.nested_snapshots.size(), 1u);
        EXPECT_EQ(snapshot.nested_snapshots[0].entry_id, 200);

        // Entry 200's snapshot should contain nested snapshot for entry 300
        ASSERT_EQ(snapshot.nested_snapshots[0].nested_snapshots.size(), 1u);
        EXPECT_EQ(
            snapshot.nested_snapshots[0].nested_snapshots[0].entry_id,
            300);
    }

    TEST(SubgraphCompilerTest, CompileRecursiveCycleDetection)
    {
        // Entry A (entryRef→B) and Entry B (entryRef→A)
        auto doc_a = MakeGraphDocument(
            {MakeComponentNode("A_node", "g-A"),
             MakeEntryRefNode("A_ref", 200)});

        auto doc_b = MakeGraphDocument(
            {MakeComponentNode("B_node", "g-B"),
             MakeEntryRefNode("B_ref", 100)});

        auto entry_100 = MakeRepositoryEntry(100, doc_a);
        auto entry_200 = MakeRepositoryEntry(200, doc_b);

        auto accessor = MockEntryAccessor({{100, entry_100}, {200, entry_200}});

        SubgraphCompiler compiler;
        auto             snapshot = compiler.CompileRecursive(doc_a, accessor);

        // Should produce snapshot for entry 200
        // But should detect cycle when trying to recurse into entry 100
        // from within entry 200's document
        EXPECT_FALSE(snapshot.nested_snapshots.empty());

        // At least one diagnostic about cycle detection (any nesting depth)
        bool has_cycle_diagnostic = false;
        std::function<void(const Dto::SubgraphCompileResultDto&)> check_diags;
        check_diags = [&](const Dto::SubgraphCompileResultDto& snap)
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

    TEST(SubgraphCompilerTest, CompileRecursiveSelfReference)
    {
        // Entry 500 has entryRef node pointing to entry_id=500 (self)
        auto doc_self = MakeGraphDocument(
            {MakeComponentNode("self_node", "g-self"),
             MakeEntryRefNode("self_ref", 500)});

        auto entry_500 = MakeRepositoryEntry(500, doc_self);
        auto accessor = MockEntryAccessor({{500, entry_500}});

        SubgraphCompiler compiler;
        auto snapshot = compiler.CompileRecursive(doc_self, accessor);

        // Should detect self-reference and produce diagnostic (any depth)
        bool has_cycle_diag = false;
        std::function<void(const Dto::SubgraphCompileResultDto&)> find_cycle;
        find_cycle = [&](const Dto::SubgraphCompileResultDto& snap)
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

    TEST(SubgraphCompilerTest, CompileSubgraphWithNoEntryRefs)
    {
        // Graph has only componentRef nodes — no entryRefs
        auto doc = MakeGraphDocument(
            {MakeComponentNode("A", "g-A"), MakeComponentNode("B", "g-B")},
            {MakeEdge("e1", "A", "out", "B", "in")});

        SubgraphCompiler compiler;
        auto snapshot = compiler.CompileRecursive(doc, MockEntryAccessor({}));

        // No entryRefs → empty snapshot
        EXPECT_EQ(snapshot.entry_id, 0);
        EXPECT_TRUE(snapshot.nested_snapshots.empty());
        EXPECT_TRUE(snapshot.input_mapping.empty());
        EXPECT_TRUE(snapshot.output_mapping.empty());
    }

    TEST(SubgraphCompilerTest, CompileEntryRefUnreachableEntry)
    {
        auto outer_doc = MakeGraphDocument({MakeEntryRefNode("ref", 999)});

        // Empty accessor — entry 999 does not exist
        SubgraphCompiler compiler;
        auto             snapshot =
            compiler.CompileEntryRef(999, outer_doc, MockEntryAccessor({}));

        // Should return snapshot with diagnostic, no crash
        EXPECT_EQ(snapshot.entry_id, 999);
        EXPECT_FALSE(snapshot.diagnostics.empty());
    }

    TEST(SubgraphCompilerTest, EntryAccessorCallbackCalled)
    {
        auto outer_doc = MakeGraphDocument({MakeEntryRefNode("ref", 100)});

        auto inner_doc = MakeGraphDocument({MakeComponentNode("X", "g-X")});
        auto entry_100 = MakeRepositoryEntry(100, inner_doc);

        std::set<GraphEntryId> called_ids;
        EntryAccessor          tracking_accessor =
            [&called_ids, entry_100](GraphEntryId id) -> std::optional<TaskDto>
        {
            called_ids.insert(id);
            if (id == 100)
            {
                return entry_100;
            }
            return std::nullopt;
        };

        SubgraphCompiler compiler;
        compiler.CompileEntryRef(100, outer_doc, tracking_accessor);

        EXPECT_TRUE(called_ids.count(100));
    }

    TEST(SubgraphCompilerTest, PortMappingPreservesPortType)
    {
        GraphNodeDto root = MakeComponentNode("R", "g-R");
        root.dynamic_ports.push_back(MakePortDef("img_port", "image"));

        auto outer_doc = MakeGraphDocument(
            {MakeEntryRefNode("sub", 100)},
            {},
            {MakePortDef("outer_img", "image")});

        auto inner_doc = MakeGraphDocument({root});
        auto entry_100 = MakeRepositoryEntry(100, inner_doc);
        auto accessor = MockEntryAccessor({{100, entry_100}});

        SubgraphCompiler compiler;
        auto snapshot = compiler.CompileEntryRef(100, outer_doc, accessor);

        ASSERT_EQ(snapshot.input_mapping.size(), 1u);
        EXPECT_EQ(snapshot.input_mapping[0].port_type, "image");
    }

    TEST(SubgraphCompilerTest, NestedSnapshotPreservesParentEntryId)
    {
        auto inner_doc_l2 = MakeGraphDocument({MakeComponentNode("Z", "g-Z")});

        auto inner_doc_l1 = MakeGraphDocument(
            {MakeComponentNode("Y", "g-Y"), MakeEntryRefNode("Y_ref", 300)});

        auto outer_doc = MakeGraphDocument({MakeEntryRefNode("ref", 200)});

        auto entry_300 = MakeRepositoryEntry(300, inner_doc_l2);
        auto entry_200 = MakeRepositoryEntry(200, inner_doc_l1);

        auto accessor = MockEntryAccessor({{200, entry_200}, {300, entry_300}});

        SubgraphCompiler compiler;
        auto snapshot = compiler.CompileRecursive(outer_doc, accessor);

        ASSERT_EQ(snapshot.nested_snapshots.size(), 1u);
        // The outer level snapshot for entry 200
        EXPECT_EQ(snapshot.nested_snapshots[0].entry_id, 200);

        // Entry 200's nested snapshot should contain entry 300
        ASSERT_EQ(snapshot.nested_snapshots[0].nested_snapshots.size(), 1u);
        EXPECT_EQ(
            snapshot.nested_snapshots[0].nested_snapshots[0].entry_id,
            300);
    }
} // namespace
