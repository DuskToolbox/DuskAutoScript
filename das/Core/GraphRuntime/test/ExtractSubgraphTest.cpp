#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/ExtractSubgraph.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/GraphRuntime/GraphEntryId.h>
#include <das/Core/TaskScheduler/TaskRepositoryDtos.h>
#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
    using namespace Das::Core::GraphRuntime;
    using namespace Das::Core::GraphRuntime::Dto;
    using TaskDto =
        Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto;

    // --- Test helpers ---

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

    GraphNodeDto MakeComponentRefNodeWithSettings(
        const std::string& node_id,
        const std::string& component_guid,
        const std::string& settings_json)
    {
        auto node = MakeComponentRefNode(node_id, component_guid);
        auto opt = Das::Utils::ParseYyjsonFromString(settings_json);
        node.settings = opt ? std::move(*opt) : yyjson::value{};
        return node;
    }

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

    GraphPortDefinitionDto MakePortDef(
        const std::string& port_id,
        const std::string& port_type)
    {
        GraphPortDefinitionDto def;
        def.port_id = port_id;
        def.port_type = port_type;
        return def;
    }

    GraphDocumentDto MakeGraphDocument(
        const std::string&                  doc_id,
        std::vector<GraphNodeDto>           nodes,
        std::vector<GraphEdgeDto>           edges = {},
        std::vector<GraphPortDefinitionDto> graph_inputs = {},
        std::vector<GraphPortDefinitionDto> graph_outputs = {})
    {
        GraphDocumentDto doc;
        doc.document_id = doc_id;
        doc.version = 1;
        doc.fingerprint = "sha256:test";
        doc.nodes = std::move(nodes);
        doc.edges = std::move(edges);
        doc.graph_inputs = std::move(graph_inputs);
        doc.graph_outputs = std::move(graph_outputs);
        return doc;
    }

    Dto::CompiledGraphPlanDto MakeCompiledPlan(int64_t entry_id)
    {
        Dto::CompiledGraphPlanDto plan;
        plan.document_id = "child_doc_" + std::to_string(entry_id);
        plan.source_revision = 1;
        plan.compiled_fingerprint = "fp_" + std::to_string(entry_id);
        return plan;
    }

    // --- Fixture ---

    class ExtractSubgraphTest : public ::testing::Test
    {
    protected:
        ExtractSubgraph extractor;

        // Counter for mock factory IDs
        int64_t next_entry_id = 1000;

        ExtractSubgraph::EntryFactory MakeMockFactory()
        {
            return [this](const TaskDto& entry) -> GraphEntryId
            {
                auto id = next_entry_id++;
                // Verify the entry has a graph_document set
                EXPECT_FALSE(entry.graph_document.is_null());
                return id;
            };
        }

        ExtractSubgraph::CompileCallback MakeMockCompile()
        {
            return [](GraphEntryId entry_id) -> Dto::CompiledGraphPlanDto
            { return MakeCompiledPlan(entry_id); };
        }
    };

    // --- Tests ---

    TEST_F(ExtractSubgraphTest, ExtractTwoNodesOneInternalEdge)
    {
        // Parent: nodeA → nodeB (selected), nodeC (outside, prevents all-nodes
        // guard)
        auto parent_doc = MakeGraphDocument(
            "parent_1",
            {MakeComponentRefNode("nodeA", "{guid-a}"),
             MakeComponentRefNode("nodeB", "{guid-b}"),
             MakeComponentRefNode("nodeC", "{guid-c}")},
            {MakeEdge("e1", "nodeA", "out1", "nodeB", "in1")});

        auto result = extractor.Extract(
            1,
            {"nodeA", "nodeB"},
            parent_doc,
            MakeMockFactory(),
            MakeMockCompile());

        // Child entry was created
        EXPECT_NE(result.child_entry_id, 0);
        EXPECT_NE(result.child_entry_id, 1); // different from parent

        // Revision and fingerprint
        EXPECT_EQ(result.revision, 1);
        EXPECT_FALSE(result.fingerprint.empty());

        // node_id_mapping has 2 entries (selected nodes only)
        EXPECT_EQ(result.node_id_mapping.size(), 2u);
        EXPECT_EQ(result.node_id_mapping.count("nodeA"), 1u);
        EXPECT_EQ(result.node_id_mapping.count("nodeB"), 1u);

        // Child has 2 nodes with remapped IDs
        EXPECT_EQ(result.child_graph_document.nodes.size(), 2u);
        EXPECT_EQ(
            result.child_graph_document.nodes[0].node_id,
            result.node_id_mapping["nodeA"]);
        EXPECT_EQ(
            result.child_graph_document.nodes[1].node_id,
            result.node_id_mapping["nodeB"]);

        // Child has 1 internal edge with remapped source/target
        EXPECT_EQ(result.child_graph_document.edges.size(), 1u);
        EXPECT_EQ(
            result.child_graph_document.edges[0].source_node_id,
            result.node_id_mapping["nodeA"]);
        EXPECT_EQ(
            result.child_graph_document.edges[0].target_node_id,
            result.node_id_mapping["nodeB"]);

        // No boundary ports
        EXPECT_TRUE(result.input_port_mappings.empty());
        EXPECT_TRUE(result.output_port_mappings.empty());

        // No diagnostics
        EXPECT_TRUE(result.diagnostics.empty());
    }

    TEST_F(ExtractSubgraphTest, ExtractSingleNodeNoEdges)
    {
        // Parent: 2 nodes, select only nodeA
        auto parent_doc = MakeGraphDocument(
            "parent_2",
            {MakeComponentRefNode("nodeA", "{guid-a}"),
             MakeComponentRefNode("nodeB", "{guid-b}")},
            {});

        auto result = extractor.Extract(
            1,
            {"nodeA"},
            parent_doc,
            MakeMockFactory(),
            MakeMockCompile());

        EXPECT_NE(result.child_entry_id, 0);
        EXPECT_EQ(result.node_id_mapping.size(), 1u);
        EXPECT_EQ(result.node_id_mapping.count("nodeA"), 1u);
        EXPECT_EQ(result.child_graph_document.nodes.size(), 1u);
        EXPECT_TRUE(result.child_graph_document.edges.empty());
    }

    TEST_F(ExtractSubgraphTest, ExtractInboundEdgeCreatesInputPort)
    {
        // nodeA (outside) → nodeB (selected)
        auto parent_doc = MakeGraphDocument(
            "parent_3",
            {MakeComponentRefNode("nodeA", "{guid-a}"),
             MakeComponentRefNode("nodeB", "{guid-b}")},
            {MakeEdge("e1", "nodeA", "out1", "nodeB", "in1")},
            {MakePortDef("in1", "number")});

        auto result = extractor.Extract(
            1,
            {"nodeB"},
            parent_doc,
            MakeMockFactory(),
            MakeMockCompile());

        EXPECT_EQ(result.input_port_mappings.size(), 1u);
        const auto& mapping = result.input_port_mappings[0];
        EXPECT_EQ(mapping.old_parent_node_id, "nodeA");
        EXPECT_EQ(mapping.old_parent_port_id, "out1");
        // child_port_id starts with "in_"
        EXPECT_EQ(mapping.child_port_id.substr(0, 3), "in_");

        // Child graph_inputs has 1 entry
        EXPECT_EQ(result.child_graph_document.graph_inputs.size(), 1u);
        EXPECT_EQ(
            result.child_graph_document.graph_inputs[0].port_id,
            mapping.child_port_id);
    }

    TEST_F(ExtractSubgraphTest, ExtractOutboundEdgeCreatesOutputPort)
    {
        // nodeA (selected) → nodeB (outside)
        auto parent_doc = MakeGraphDocument(
            "parent_4",
            {MakeComponentRefNode("nodeA", "{guid-a}"),
             MakeComponentRefNode("nodeB", "{guid-b}")},
            {MakeEdge("e1", "nodeA", "out1", "nodeB", "in1")},
            {},
            {MakePortDef("out1", "string")});

        auto result = extractor.Extract(
            1,
            {"nodeA"},
            parent_doc,
            MakeMockFactory(),
            MakeMockCompile());

        EXPECT_EQ(result.output_port_mappings.size(), 1u);
        const auto& mapping = result.output_port_mappings[0];
        EXPECT_EQ(mapping.old_parent_node_id, "nodeB");
        EXPECT_EQ(mapping.old_parent_port_id, "in1");
        // child_port_id starts with "out_"
        EXPECT_EQ(mapping.child_port_id.substr(0, 4), "out_");

        // Child graph_outputs has 1 entry
        EXPECT_EQ(result.child_graph_document.graph_outputs.size(), 1u);
        EXPECT_EQ(
            result.child_graph_document.graph_outputs[0].port_id,
            mapping.child_port_id);
    }

    TEST_F(ExtractSubgraphTest, ExtractExternalEdgesExcluded)
    {
        // nodeA→nodeB (both outside), nodeC (selected alone)
        auto parent_doc = MakeGraphDocument(
            "parent_5",
            {MakeComponentRefNode("nodeA", "{guid-a}"),
             MakeComponentRefNode("nodeB", "{guid-b}"),
             MakeComponentRefNode("nodeC", "{guid-c}")},
            {MakeEdge("e1", "nodeA", "out1", "nodeB", "in1")});

        auto result = extractor.Extract(
            1,
            {"nodeC"},
            parent_doc,
            MakeMockFactory(),
            MakeMockCompile());

        // Child has only nodeC
        EXPECT_EQ(result.child_graph_document.nodes.size(), 1u);
        EXPECT_EQ(
            result.child_graph_document.nodes[0].node_id,
            result.node_id_mapping["nodeC"]);

        // No edges in child (e1 is external)
        EXPECT_TRUE(result.child_graph_document.edges.empty());
        EXPECT_TRUE(result.input_port_mappings.empty());
        EXPECT_TRUE(result.output_port_mappings.empty());
    }

    TEST_F(ExtractSubgraphTest, ExtractMixedTargetKinds)
    {
        // 1 componentRef node + 1 entryRef node + 1 outside node, select first
        // two
        auto parent_doc = MakeGraphDocument(
            "parent_6",
            {MakeComponentRefNode("compNode", "{guid-comp}"),
             MakeEntryRefNode("entryNode", 42),
             MakeComponentRefNode("outsideNode", "{guid-out}")},
            {MakeEdge("e1", "compNode", "out1", "entryNode", "in1")});

        auto result = extractor.Extract(
            1,
            {"compNode", "entryNode"},
            parent_doc,
            MakeMockFactory(),
            MakeMockCompile());

        // Child has 2 nodes
        EXPECT_EQ(result.child_graph_document.nodes.size(), 2u);

        // componentRef node preserves component_guid
        auto& child_comp = result.child_graph_document.nodes[0];
        EXPECT_TRUE(child_comp.target.component_ref.has_value());
        EXPECT_EQ(
            child_comp.target.component_ref->component_guid,
            "{guid-comp}");

        // entryRef node preserves entry_id (same value, not changed)
        auto& child_entry = result.child_graph_document.nodes[1];
        EXPECT_TRUE(child_entry.target.entry_ref.has_value());
        EXPECT_EQ(child_entry.target.entry_ref->entry_id, 42);
    }

    TEST_F(ExtractSubgraphTest, ExtractNodeIdMappingAccuracy)
    {
        // 4 nodes, select 3 (n1, n2, n3), leave n4 outside
        auto parent_doc = MakeGraphDocument(
            "parent_7",
            {MakeComponentRefNode("n1", "{g1}"),
             MakeComponentRefNode("n2", "{g2}"),
             MakeComponentRefNode("n3", "{g3}"),
             MakeComponentRefNode("n4", "{g4}")},
            {MakeEdge("e1", "n1", "out", "n2", "in"),
             MakeEdge("e2", "n2", "out", "n3", "in")});

        auto result = extractor.Extract(
            1,
            {"n1", "n2", "n3"},
            parent_doc,
            MakeMockFactory(),
            MakeMockCompile());

        // 3 mapping entries
        EXPECT_EQ(result.node_id_mapping.size(), 3u);

        // Each old→new pair maps the correct node
        for (const auto& [old_id, new_id] : result.node_id_mapping)
        {
            // Find the child node by new_id
            bool found = false;
            for (const auto& child_node : result.child_graph_document.nodes)
            {
                if (child_node.node_id == new_id)
                {
                    found = true;
                    // Verify the underlying target matches the original
                    // (component_guid should match)
                    std::string expected_guid = "{" + old_id.substr(1) + "}";
                    EXPECT_TRUE(child_node.target.component_ref.has_value());
                    break;
                }
            }
            EXPECT_TRUE(found) << "No child node found for mapping " << old_id
                               << " -> " << new_id;
        }
    }

    TEST_F(ExtractSubgraphTest, ExtractPreservesNodeSettings)
    {
        // componentRef node with settings + 2 outside nodes
        auto parent_doc = MakeGraphDocument(
            "parent_8",
            {MakeComponentRefNodeWithSettings(
                 "n1",
                 "{g1}",
                 R"({"threshold": 0.5, "mode": "auto"})"),
             MakeComponentRefNode("n2", "{g2}"),
             MakeComponentRefNode("n3", "{g3}")},
            {MakeEdge("e1", "n1", "out", "n2", "in")});

        auto result = extractor.Extract(
            1,
            {"n1", "n2"},
            parent_doc,
            MakeMockFactory(),
            MakeMockCompile());

        // Find child node corresponding to n1
        auto child_n1_id = result.node_id_mapping["n1"];
        bool found = false;
        for (const auto& node : result.child_graph_document.nodes)
        {
            if (node.node_id == child_n1_id)
            {
                found = true;
                // Settings should be preserved
                EXPECT_FALSE(node.settings.is_null());
                break;
            }
        }
        EXPECT_TRUE(found);
    }

    TEST_F(ExtractSubgraphTest, ExtractEmptySelection)
    {
        auto parent_doc = MakeGraphDocument(
            "parent_9",
            {MakeComponentRefNode("n1", "{g1}")},
            {});

        auto result = extractor.Extract(
            1,
            {}, // empty selection
            parent_doc,
            MakeMockFactory(),
            MakeMockCompile());

        EXPECT_EQ(result.child_entry_id, 0);
        ASSERT_FALSE(result.diagnostics.empty());
        EXPECT_NE(
            result.diagnostics[0].find("No nodes selected"),
            std::string::npos);
    }

    TEST_F(ExtractSubgraphTest, ExtractAllNodesSelected)
    {
        auto parent_doc = MakeGraphDocument(
            "parent_10",
            {MakeComponentRefNode("n1", "{g1}"),
             MakeComponentRefNode("n2", "{g2}")},
            {});

        auto result = extractor.Extract(
            1,
            {"n1", "n2"}, // all nodes
            parent_doc,
            MakeMockFactory(),
            MakeMockCompile());

        ASSERT_FALSE(result.diagnostics.empty());
        EXPECT_NE(
            result.diagnostics[0].find("Cannot extract entire graph"),
            std::string::npos);
    }

    TEST_F(ExtractSubgraphTest, ExtractCompileCallbackReceivesCorrectEntryId)
    {
        auto parent_doc = MakeGraphDocument(
            "parent_11",
            {MakeComponentRefNode("n1", "{g1}"),
             MakeComponentRefNode("n2", "{g2}"),
             MakeComponentRefNode("n3", "{g3}")},
            {MakeEdge("e1", "n1", "out", "n2", "in")});

        GraphEntryId captured_entry_id = 0;
        auto         factory = [this](const TaskDto& entry) -> GraphEntryId
        { return next_entry_id++; };

        auto compile_cb = [&captured_entry_id](GraphEntryId entry_id)
            -> Dto::CompiledGraphPlanDto
        {
            captured_entry_id = entry_id;
            return MakeCompiledPlan(entry_id);
        };

        auto result =
            extractor.Extract(1, {"n1", "n2"}, parent_doc, factory, compile_cb);

        // compile_callback received the same entry_id as entry_factory returned
        EXPECT_EQ(captured_entry_id, result.child_entry_id);
    }

    TEST_F(ExtractSubgraphTest, ExtractPortDefinitionTypePreservation)
    {
        // Inbound edge → child input port type matches the source edge's port
        // type
        auto parent_doc = MakeGraphDocument(
            "parent_12",
            {MakeComponentRefNode("outside", "{g1}"),
             MakeComponentRefNode("selected", "{g2}")},
            {MakeEdge(
                "e1",
                "outside",
                "output_port",
                "selected",
                "input_port")},
            {MakePortDef("input_port", "number")}); // port type = "number"

        auto result = extractor.Extract(
            1,
            {"selected"},
            parent_doc,
            MakeMockFactory(),
            MakeMockCompile());

        ASSERT_EQ(result.input_port_mappings.size(), 1u);
        // port_type should be "number" (from the port definition lookup)
        EXPECT_EQ(result.input_port_mappings[0].port_type, "number");

        // Also verify the child graph_inputs port definition has the type
        ASSERT_EQ(result.child_graph_document.graph_inputs.size(), 1u);
        EXPECT_EQ(
            result.child_graph_document.graph_inputs[0].port_type,
            "number");
    }

    // ========================================================================
    // Replace + Undo tests (02-20)
    // ========================================================================

    class ExtractSubgraphReplaceTest : public ::testing::Test
    {
    protected:
        ExtractSubgraph extractor;
        int64_t         next_entry_id = 2000;

        ExtractSubgraph::EntryFactory MakeMockFactory()
        {
            return [this](const TaskDto& entry) -> GraphEntryId
            { return next_entry_id++; };
        }

        ExtractSubgraph::CompileCallback MakeMockCompile()
        {
            return [](GraphEntryId entry_id) -> Dto::CompiledGraphPlanDto
            { return MakeCompiledPlan(entry_id); };
        }
    };

    TEST_F(ExtractSubgraphReplaceTest, ReplaceWithSubgraphRemovesSelectedNodes)
    {
        auto parent = MakeGraphDocument(
            "p1",
            {MakeComponentRefNode("A", "{gA}"),
             MakeComponentRefNode("B", "{gB}"),
             MakeComponentRefNode("C", "{gC}")},
            {});

        auto extract_result = extractor.Extract(
            1, {"B"}, parent, MakeMockFactory(), MakeMockCompile());

        auto replace_result = extractor.ReplaceWithSubgraph(
            parent, extract_result, {"B"});

        // Updated doc has 3 nodes: A, replacement, C
        EXPECT_EQ(replace_result.updated_graph_document.nodes.size(), 3u);

        // Node B is gone
        for (const auto& n : replace_result.updated_graph_document.nodes)
        {
            EXPECT_NE(n.node_id, "B");
        }

        // Replacement node exists with entryRef target
        bool found_replacement = false;
        for (const auto& n : replace_result.updated_graph_document.nodes)
        {
            if (n.node_id == replace_result.replacement_node_id)
            {
                found_replacement = true;
                EXPECT_EQ(n.target.target_kind, "entryRef");
                ASSERT_TRUE(n.target.entry_ref.has_value());
            }
        }
        EXPECT_TRUE(found_replacement);
    }

    TEST_F(ExtractSubgraphReplaceTest, ReplaceWithSubgraphEntryRefHasCorrectFields)
    {
        auto parent = MakeGraphDocument(
            "p2",
            {MakeComponentRefNode("A", "{gA}"),
             MakeComponentRefNode("B", "{gB}"),
             MakeComponentRefNode("C", "{gC}")},
            {});

        auto extract_result = extractor.Extract(
            1, {"A", "B"}, parent, MakeMockFactory(), MakeMockCompile());

        auto replace_result = extractor.ReplaceWithSubgraph(
            parent, extract_result, {"A", "B"});

        // Find replacement node and verify entryRef fields
        bool found = false;
        for (const auto& n : replace_result.updated_graph_document.nodes)
        {
            if (n.node_id == replace_result.replacement_node_id)
            {
                found = true;
                ASSERT_TRUE(n.target.entry_ref.has_value());
                const auto& ref = n.target.entry_ref.value();
                EXPECT_EQ(ref.entry_id, extract_result.child_entry_id);
                ASSERT_TRUE(ref.expected_revision.has_value());
                EXPECT_EQ(ref.expected_revision.value(), 1);
                ASSERT_TRUE(ref.source_fingerprint.has_value());
                EXPECT_FALSE(ref.source_fingerprint.value().empty());
            }
        }
        EXPECT_TRUE(found);
    }

    TEST_F(ExtractSubgraphReplaceTest, ReplaceWithSubgraphRewiresInboundEdge)
    {
        // A(outside) → B(selected), C(outside)
        auto parent = MakeGraphDocument(
            "p3",
            {MakeComponentRefNode("A", "{gA}"),
             MakeComponentRefNode("B", "{gB}"),
             MakeComponentRefNode("C", "{gC}")},
            {MakeEdge("e1", "A", "out1", "B", "in1")},
            {MakePortDef("in1", "number")});

        auto extract_result = extractor.Extract(
            1, {"B"}, parent, MakeMockFactory(), MakeMockCompile());

        auto replace_result = extractor.ReplaceWithSubgraph(
            parent, extract_result, {"B"});

        // Edge rewired: A → replacement_node, target_port from mapping
        ASSERT_EQ(replace_result.updated_graph_document.edges.size(), 1u);
        const auto& edge = replace_result.updated_graph_document.edges[0];
        EXPECT_EQ(edge.source_node_id, "A");
        EXPECT_EQ(edge.source_port_id, "out1");
        EXPECT_EQ(edge.target_node_id, replace_result.replacement_node_id);

        ASSERT_EQ(extract_result.input_port_mappings.size(), 1u);
        EXPECT_EQ(
            edge.target_port_id,
            extract_result.input_port_mappings[0].child_port_id);
    }

    TEST_F(ExtractSubgraphReplaceTest, ReplaceWithSubgraphRewiresOutboundEdge)
    {
        // B(selected) → C(outside), A(outside)
        auto parent = MakeGraphDocument(
            "p4",
            {MakeComponentRefNode("A", "{gA}"),
             MakeComponentRefNode("B", "{gB}"),
             MakeComponentRefNode("C", "{gC}")},
            {MakeEdge("e1", "B", "out1", "C", "in1")},
            {},
            {MakePortDef("out1", "string")});

        auto extract_result = extractor.Extract(
            1, {"B"}, parent, MakeMockFactory(), MakeMockCompile());

        auto replace_result = extractor.ReplaceWithSubgraph(
            parent, extract_result, {"B"});

        // Edge rewired: replacement_node → C, source_port from mapping
        ASSERT_EQ(replace_result.updated_graph_document.edges.size(), 1u);
        const auto& edge = replace_result.updated_graph_document.edges[0];
        EXPECT_EQ(edge.source_node_id, replace_result.replacement_node_id);
        EXPECT_EQ(edge.target_node_id, "C");
        EXPECT_EQ(edge.target_port_id, "in1");

        ASSERT_EQ(extract_result.output_port_mappings.size(), 1u);
        EXPECT_EQ(
            edge.source_port_id,
            extract_result.output_port_mappings[0].child_port_id);
    }

    TEST_F(ExtractSubgraphReplaceTest, ReplaceWithSubgraphPreservesExternalEdge)
    {
        // A→B (both outside), C(selected)
        auto parent = MakeGraphDocument(
            "p5",
            {MakeComponentRefNode("A", "{gA}"),
             MakeComponentRefNode("B", "{gB}"),
             MakeComponentRefNode("C", "{gC}")},
            {MakeEdge("e1", "A", "out1", "B", "in1")});

        auto extract_result = extractor.Extract(
            1, {"C"}, parent, MakeMockFactory(), MakeMockCompile());

        auto replace_result = extractor.ReplaceWithSubgraph(
            parent, extract_result, {"C"});

        // External edge A→B unchanged
        ASSERT_EQ(replace_result.updated_graph_document.edges.size(), 1u);
        const auto& edge = replace_result.updated_graph_document.edges[0];
        EXPECT_EQ(edge.source_node_id, "A");
        EXPECT_EQ(edge.source_port_id, "out1");
        EXPECT_EQ(edge.target_node_id, "B");
        EXPECT_EQ(edge.target_port_id, "in1");
    }

    TEST_F(ExtractSubgraphReplaceTest, ReplaceResultIncludesUndoSnapshot)
    {
        auto parent = MakeGraphDocument(
            "p6",
            {MakeComponentRefNode("A", "{gA}"),
             MakeComponentRefNode("B", "{gB}"),
             MakeComponentRefNode("C", "{gC}")},
            {MakeEdge("e1", "A", "out1", "B", "in1")});

        auto extract_result = extractor.Extract(
            1, {"A", "B"}, parent, MakeMockFactory(), MakeMockCompile());

        auto replace_result = extractor.ReplaceWithSubgraph(
            parent, extract_result, {"A", "B"});

        // Snapshot captures pre-replacement state
        EXPECT_EQ(replace_result.undo_snapshot.removed_nodes.size(), 2u);
        EXPECT_FALSE(replace_result.undo_snapshot.removed_edges.empty());
        EXPECT_EQ(
            replace_result.undo_snapshot.replacement_node_id,
            replace_result.replacement_node_id);
    }

    TEST_F(ExtractSubgraphReplaceTest, UndoExtractRestoresOriginalNodes)
    {
        auto parent = MakeGraphDocument(
            "p7",
            {MakeComponentRefNode("A", "{gA}"),
             MakeComponentRefNode("B", "{gB}"),
             MakeComponentRefNode("C", "{gC}")},
            {MakeEdge("e1", "A", "out1", "B", "in1")});

        auto extract_result = extractor.Extract(
            1, {"A", "B"}, parent, MakeMockFactory(), MakeMockCompile());

        auto replace_result = extractor.ReplaceWithSubgraph(
            parent, extract_result, {"A", "B"});

        bool delete_called = false;
        auto delete_cb = [&delete_called](GraphEntryId) -> bool
        {
            delete_called = true;
            return true;
        };
        auto ref_count_cb = [](GraphEntryId) -> int { return 0; };

        auto restored = extractor.UndoExtract(
            replace_result.undo_snapshot, delete_cb, ref_count_cb);

        // Restored doc has all original nodes with same IDs
        EXPECT_EQ(restored.nodes.size(), parent.nodes.size());
        for (const auto& orig : parent.nodes)
        {
            bool found = false;
            for (const auto& r : restored.nodes)
            {
                if (r.node_id == orig.node_id)
                {
                    found = true;
                    break;
                }
            }
            EXPECT_TRUE(found) << "Node " << orig.node_id
                               << " not restored";
        }
    }

    TEST_F(ExtractSubgraphReplaceTest, UndoExtractRestoresOriginalEdgeStructure)
    {
        // X(outside) → A(selected), B(outside)
        auto parent = MakeGraphDocument(
            "p8",
            {MakeComponentRefNode("X", "{gX}"),
             MakeComponentRefNode("A", "{gA}"),
             MakeComponentRefNode("B", "{gB}")},
            {MakeEdge("e1", "X", "out1", "A", "in1")},
            {MakePortDef("in1", "number")});

        auto extract_result = extractor.Extract(
            1, {"A"}, parent, MakeMockFactory(), MakeMockCompile());

        auto replace_result = extractor.ReplaceWithSubgraph(
            parent, extract_result, {"A"});

        auto delete_cb = [](GraphEntryId) -> bool { return true; };
        auto ref_count_cb = [](GraphEntryId) -> int { return 0; };

        auto restored = extractor.UndoExtract(
            replace_result.undo_snapshot, delete_cb, ref_count_cb);

        // Edge restored: X → A with original ports
        ASSERT_EQ(restored.edges.size(), 1u);
        EXPECT_EQ(restored.edges[0].source_node_id, "X");
        EXPECT_EQ(restored.edges[0].source_port_id, "out1");
        EXPECT_EQ(restored.edges[0].target_node_id, "A");
        EXPECT_EQ(restored.edges[0].target_port_id, "in1");
    }

    TEST_F(ExtractSubgraphReplaceTest, UndoExtractDeletesChildEntryWhenRefCountZero)
    {
        auto parent = MakeGraphDocument(
            "p9",
            {MakeComponentRefNode("A", "{gA}"),
             MakeComponentRefNode("B", "{gB}"),
             MakeComponentRefNode("C", "{gC}")},
            {});

        auto extract_result = extractor.Extract(
            1, {"B"}, parent, MakeMockFactory(), MakeMockCompile());

        auto replace_result = extractor.ReplaceWithSubgraph(
            parent, extract_result, {"B"});

        bool         delete_called = false;
        GraphEntryId deleted_id    = 0;
        auto         delete_cb =
            [&delete_called, &deleted_id](GraphEntryId id) -> bool
        {
            delete_called = true;
            deleted_id    = id;
            return true;
        };
        auto ref_count_cb = [](GraphEntryId) -> int { return 0; };

        extractor.UndoExtract(
            replace_result.undo_snapshot, delete_cb, ref_count_cb);

        EXPECT_TRUE(delete_called);
        EXPECT_EQ(deleted_id, extract_result.child_entry_id);
    }

    TEST_F(ExtractSubgraphReplaceTest, UndoExtractSkipsDeleteWhenRefCountPositive)
    {
        auto parent = MakeGraphDocument(
            "p10",
            {MakeComponentRefNode("A", "{gA}"),
             MakeComponentRefNode("B", "{gB}"),
             MakeComponentRefNode("C", "{gC}")},
            {});

        auto extract_result = extractor.Extract(
            1, {"B"}, parent, MakeMockFactory(), MakeMockCompile());

        auto replace_result = extractor.ReplaceWithSubgraph(
            parent, extract_result, {"B"});

        bool delete_called = false;
        auto delete_cb = [&delete_called](GraphEntryId) -> bool
        {
            delete_called = true;
            return true;
        };
        auto ref_count_cb = [](GraphEntryId) -> int { return 2; };

        extractor.UndoExtract(
            replace_result.undo_snapshot, delete_cb, ref_count_cb);

        EXPECT_FALSE(delete_called);
    }

    TEST_F(ExtractSubgraphReplaceTest, UndoExtractSkipsDeleteWhenRefCountCallbackAbsent)
    {
        auto parent = MakeGraphDocument(
            "p11",
            {MakeComponentRefNode("A", "{gA}"),
             MakeComponentRefNode("B", "{gB}"),
             MakeComponentRefNode("C", "{gC}")},
            {});

        auto extract_result = extractor.Extract(
            1, {"B"}, parent, MakeMockFactory(), MakeMockCompile());

        auto replace_result = extractor.ReplaceWithSubgraph(
            parent, extract_result, {"B"});

        bool delete_called = false;
        auto delete_cb = [&delete_called](GraphEntryId) -> bool
        {
            delete_called = true;
            return true;
        };

        // Empty / null ref_count_callback
        ExtractSubgraph::RefCountCallback empty_ref_cb;

        extractor.UndoExtract(
            replace_result.undo_snapshot, delete_cb, empty_ref_cb);

        EXPECT_FALSE(delete_called);
    }

    TEST_F(ExtractSubgraphReplaceTest, ReplaceWithSubgraphHandlesNoInboundEdges)
    {
        // B(selected) → C(outside), A(outside, no edge to B)
        auto parent = MakeGraphDocument(
            "p12",
            {MakeComponentRefNode("A", "{gA}"),
             MakeComponentRefNode("B", "{gB}"),
             MakeComponentRefNode("C", "{gC}")},
            {MakeEdge("e1", "B", "out1", "C", "in1")},
            {},
            {MakePortDef("out1", "string")});

        auto extract_result = extractor.Extract(
            1, {"B"}, parent, MakeMockFactory(), MakeMockCompile());

        auto replace_result = extractor.ReplaceWithSubgraph(
            parent, extract_result, {"B"});

        // One outbound edge: replacement → C
        ASSERT_EQ(replace_result.updated_graph_document.edges.size(), 1u);
        const auto& edge = replace_result.updated_graph_document.edges[0];
        EXPECT_EQ(edge.source_node_id, replace_result.replacement_node_id);
        EXPECT_EQ(edge.target_node_id, "C");
    }

    TEST_F(ExtractSubgraphReplaceTest, ReplaceWithSubgraphHandlesNoOutboundEdges)
    {
        // A(outside) → B(selected), C(outside, no edge from B)
        auto parent = MakeGraphDocument(
            "p13",
            {MakeComponentRefNode("A", "{gA}"),
             MakeComponentRefNode("B", "{gB}"),
             MakeComponentRefNode("C", "{gC}")},
            {MakeEdge("e1", "A", "out1", "B", "in1")},
            {MakePortDef("in1", "number")});

        auto extract_result = extractor.Extract(
            1, {"B"}, parent, MakeMockFactory(), MakeMockCompile());

        auto replace_result = extractor.ReplaceWithSubgraph(
            parent, extract_result, {"B"});

        // One inbound edge: A → replacement
        ASSERT_EQ(replace_result.updated_graph_document.edges.size(), 1u);
        const auto& edge = replace_result.updated_graph_document.edges[0];
        EXPECT_EQ(edge.source_node_id, "A");
        EXPECT_EQ(edge.target_node_id, replace_result.replacement_node_id);
    }

    TEST_F(ExtractSubgraphReplaceTest, UndoExtractWithEmptySnapshot)
    {
        auto doc = MakeGraphDocument(
            "original_doc",
            {MakeComponentRefNode("X", "{gX}")},
            {});

        ParentSnapshot empty_snapshot;
        empty_snapshot.original_graph_document = doc;
        // replacement_node_id is empty, removed_nodes is empty

        auto delete_cb = [](GraphEntryId) -> bool { return true; };
        auto ref_count_cb = [](GraphEntryId) -> int { return 0; };

        auto result = extractor.UndoExtract(
            empty_snapshot, delete_cb, ref_count_cb);

        // Returns original_graph_document unchanged
        EXPECT_EQ(result.document_id, "original_doc");
        ASSERT_EQ(result.nodes.size(), 1u);
        EXPECT_EQ(result.nodes[0].node_id, "X");
    }

} // namespace
