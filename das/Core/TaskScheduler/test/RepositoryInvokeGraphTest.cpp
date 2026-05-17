#include <das/Core/TaskScheduler/RepositoryInvokeGraph.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace
{
    using Das::Core::TaskScheduler::RepositoryInvoke::RepositoryDependencyEdge;
    using Das::Core::TaskScheduler::RepositoryInvoke::
        ValidateRepositoryInvokeAcyclic;

    void ExpectAcyclic(const std::vector<RepositoryDependencyEdge>& edges)
    {
        const auto result = ValidateRepositoryInvokeAcyclic(edges);
        EXPECT_TRUE(result.ok);
        EXPECT_TRUE(result.cycle_path.empty());
    }

    std::vector<int64_t> EntryIds(
        const std::vector<Das::Core::TaskScheduler::RepositoryInvoke::
                              RepositoryCyclePathItem>& path)
    {
        std::vector<int64_t> ids;
        ids.reserve(path.size());
        std::ranges::transform(
            path,
            std::back_inserter(ids),
            [](const auto& item) { return item.entry_id; });
        return ids;
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
