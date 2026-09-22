#include <das/Core/GraphRuntime/RepositoryInvokeGraph.h>

#include <das/Utils/DasJsonCore.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

// Migrated from TaskScheduler RepositoryInvokeGraphTest / the algorithm
// half of RepositoryInvokeCompilerTest — namespace rewritten to the
// GraphRuntime home, cycle cases restated as pure edge-extraction +
// validation calls (the snapshot-resolution orchestration was dead
// production code and was retired with the old home).

namespace
{
    using Das::Core::GraphRuntime::RepositoryInvoke::RepositoryDependencyEdge;
    using Das::Core::GraphRuntime::RepositoryInvoke::
        ValidateRepositoryInvokeAcyclic;

    void ExpectAcyclic(const std::vector<RepositoryDependencyEdge>& edges)
    {
        const auto result = ValidateRepositoryInvokeAcyclic(edges);
        EXPECT_TRUE(result.ok);
        EXPECT_TRUE(result.cycle_path.empty());
    }

    std::vector<int64_t> EntryIds(
        const std::vector<
            Das::Core::GraphRuntime::RepositoryInvoke::RepositoryCyclePathItem>&
            path)
    {
        std::vector<int64_t> ids;
        ids.reserve(path.size());
        std::ranges::transform(
            path,
            std::back_inserter(ids),
            [](const auto& item) { return item.entry_id; });
        return ids;
    }

    yyjson::value ParseJson(std::string_view json)
    {
        auto parsed = Das::Utils::ParseYyjsonFromString(json);
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : yyjson::value{};
    }
} // namespace

TEST(RepositoryInvokeGraphTest, AcyclicEmptyGraphPasses) { ExpectAcyclic({}); }

TEST(RepositoryInvokeGraphTest, AcyclicSingleChainPasses)
{
    ExpectAcyclic({
        {.source_entry_id = 1, .target_entry_id = 2},
        {.source_entry_id = 2, .target_entry_id = 3},
        {.source_entry_id = 3, .target_entry_id = 4},
    });
}

TEST(RepositoryInvokeGraphTest, AcyclicMultipleRootsPass)
{
    ExpectAcyclic({
        {.source_entry_id = 1, .target_entry_id = 10},
        {.source_entry_id = 2, .target_entry_id = 20},
        {.source_entry_id = 3, .target_entry_id = 30},
    });
}

TEST(RepositoryInvokeGraphTest, AcyclicSharedSubtreePasses)
{
    ExpectAcyclic({
        {.source_entry_id = 1, .target_entry_id = 3},
        {.source_entry_id = 2, .target_entry_id = 3},
        {.source_entry_id = 3, .target_entry_id = 4},
    });
}

TEST(RepositoryInvokeGraphTest, AcyclicResultIsStableAcrossEdgeOrder)
{
    std::vector<RepositoryDependencyEdge> edges{
        {.source_entry_id = 1, .target_entry_id = 3},
        {.source_entry_id = 2, .target_entry_id = 3},
        {.source_entry_id = 3, .target_entry_id = 4},
        {.source_entry_id = 5, .target_entry_id = 6},
    };

    ExpectAcyclic(edges);

    std::ranges::reverse(edges);

    ExpectAcyclic(edges);
}

TEST(RepositoryInvokeGraphTest, DirectCycleReturnsClosedPathWithSourceLabel)
{
    const std::vector<RepositoryDependencyEdge> edges{
        {
            .source_entry_id = 7,
            .target_entry_id = 7,
            .source_node_id = "node-self",
            .source_node_label = "Invoke self",
        },
    };

    const auto result = ValidateRepositoryInvokeAcyclic(edges);

    ASSERT_FALSE(result.ok);
    ASSERT_EQ(EntryIds(result.cycle_path), (std::vector<int64_t>{7, 7}));
    ASSERT_TRUE(result.cycle_path.front().source_node_label.has_value());
    EXPECT_EQ(result.cycle_path.front().source_node_label, "Invoke self");
}

TEST(
    RepositoryInvokeGraphTest,
    IndirectCycleReturnsClosedPathWithSourceMetadata)
{
    const std::vector<RepositoryDependencyEdge> edges{
        {
            .source_entry_id = 1,
            .target_entry_id = 2,
            .source_node_id = "node-1",
            .source_node_label = "Invoke two",
        },
        {
            .source_entry_id = 2,
            .target_entry_id = 3,
            .source_node_id = "node-2",
            .source_node_label = "Invoke three",
        },
        {
            .source_entry_id = 3,
            .target_entry_id = 1,
            .source_node_id = "node-3",
            .source_node_label = "Invoke one",
        },
    };

    const auto result = ValidateRepositoryInvokeAcyclic(edges);

    ASSERT_FALSE(result.ok);
    ASSERT_EQ(EntryIds(result.cycle_path), (std::vector<int64_t>{1, 2, 3, 1}));
    ASSERT_GE(result.cycle_path.size(), 4);
    EXPECT_EQ(result.cycle_path[0].source_node_id, "node-1");
    EXPECT_EQ(result.cycle_path[0].source_node_label, "Invoke two");
    EXPECT_EQ(result.cycle_path[1].source_node_id, "node-2");
    EXPECT_EQ(result.cycle_path[1].source_node_label, "Invoke three");
    EXPECT_EQ(result.cycle_path[2].source_node_id, "node-3");
    EXPECT_EQ(result.cycle_path[2].source_node_label, "Invoke one");
}

TEST(RepositoryInvokeGraphTest, DisconnectedGraphReportsCycleComponent)
{
    const std::vector<RepositoryDependencyEdge> edges{
        {.source_entry_id = 1, .target_entry_id = 2},
        {.source_entry_id = 2, .target_entry_id = 3},
        {
            .source_entry_id = 10,
            .target_entry_id = 11,
            .source_node_label = "Invoke eleven",
        },
        {
            .source_entry_id = 11,
            .target_entry_id = 10,
            .source_node_label = "Invoke ten",
        },
    };

    const auto result = ValidateRepositoryInvokeAcyclic(edges);

    ASSERT_FALSE(result.ok);
    ASSERT_EQ(EntryIds(result.cycle_path), (std::vector<int64_t>{10, 11, 10}));
    EXPECT_EQ(result.cycle_path[0].source_node_label, "Invoke eleven");
    EXPECT_EQ(result.cycle_path[1].source_node_label, "Invoke ten");
}

// ---------------------------------------------------------------------
// Edge extraction (migrated from RepositoryInvokeCompilerTest).
// ---------------------------------------------------------------------

TEST(RepositoryInvokeGraphTest, ExtractsRepositoryRefEdgesFromSourceNodes)
{
    auto graph = ParseJson(R"json({
        "nodes": [
            {
                "id": "node-a",
                "label": "Invoke child",
                "taskRepositoryRef": {
                    "kind": "taskRepositoryRef",
                    "entryId": 20
                }
            }
        ]
    })json");

    auto edges = Das::Core::GraphRuntime::RepositoryInvoke::
        ExtractRepositoryInvokeDependencyEdges(10, graph);

    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0].source_entry_id, 10);
    EXPECT_EQ(edges[0].target_entry_id, 20);
    EXPECT_EQ(edges[0].source_node_id, "node-a");
    EXPECT_EQ(edges[0].source_node_label, "Invoke child");
}

// Reference values of the plain integer shape (settings.repositoryRef =
// entryId, the only shape settings writers emit today) must join cycle
// detection too — an integer ref closing a cycle is detected with the same
// closed path as the wrapped shape.
TEST(RepositoryInvokeGraphTest, IntegerRepositoryRefShapeJoinsCycleDetection)
{
    auto graph = ParseJson(R"json({
        "nodes": [
            {
                "id": "node-self",
                "label": "Invoke self",
                "settings": {
                    "repositoryRef": 42
                }
            }
        ]
    })json");

    auto edges = Das::Core::GraphRuntime::RepositoryInvoke::
        ExtractRepositoryInvokeDependencyEdges(42, graph);
    const auto result = ValidateRepositoryInvokeAcyclic(edges);

    ASSERT_FALSE(result.ok);
    EXPECT_EQ(EntryIds(result.cycle_path), (std::vector<int64_t>{42, 42}));
    ASSERT_FALSE(result.cycle_path.empty());
    EXPECT_EQ(result.cycle_path.front().source_node_label, "Invoke self");
}

TEST(RepositoryInvokeGraphTest, SelfReferenceEdgesYieldClosedCyclePath)
{
    auto graph = ParseJson(R"json({
        "nodes": [
            {
                "id": "node-self",
                "label": "Invoke self",
                "taskRepositoryRef": {
                    "kind": "taskRepositoryRef",
                    "entryId": 42
                }
            }
        ]
    })json");

    auto edges = Das::Core::GraphRuntime::RepositoryInvoke::
        ExtractRepositoryInvokeDependencyEdges(42, graph);
    const auto result = ValidateRepositoryInvokeAcyclic(edges);

    ASSERT_FALSE(result.ok);
    EXPECT_EQ(EntryIds(result.cycle_path), (std::vector<int64_t>{42, 42}));
    ASSERT_GE(result.cycle_path.size(), 2u);
    EXPECT_EQ(result.cycle_path.front().source_node_label, "Invoke self");
}

TEST(RepositoryInvokeGraphTest, IndirectReferenceCycleYieldsFullPath)
{
    auto graph = ParseJson(R"json({
        "nodes": [
            {
                "sourceEntryId": 1,
                "id": "node-1",
                "label": "Invoke two",
                "taskRepositoryRef": {
                    "kind": "taskRepositoryRef",
                    "entryId": 2
                }
            },
            {
                "sourceEntryId": 2,
                "id": "node-2",
                "label": "Invoke one",
                "settings": {
                    "repositoryRef": {
                        "kind": "taskRepositoryRef",
                        "entryId": 1
                    }
                }
            }
        ]
    })json");

    auto edges = Das::Core::GraphRuntime::RepositoryInvoke::
        ExtractRepositoryInvokeDependencyEdges(1, graph);
    const auto result = ValidateRepositoryInvokeAcyclic(edges);

    ASSERT_FALSE(result.ok);
    EXPECT_EQ(EntryIds(result.cycle_path), (std::vector<int64_t>{1, 2, 1}));
    ASSERT_GE(result.cycle_path.size(), 3u);
    EXPECT_EQ(result.cycle_path[0].source_node_label, "Invoke two");
    EXPECT_EQ(result.cycle_path[1].source_node_label, "Invoke one");
}
