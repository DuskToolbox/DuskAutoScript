#include <das/Core/TaskScheduler/RepositoryInvokeGraph.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace
{
    using Das::Core::TaskScheduler::RepositoryInvoke::
        RepositoryDependencyEdge;
    using Das::Core::TaskScheduler::RepositoryInvoke::
        ValidateRepositoryInvokeAcyclic;

    void ExpectAcyclic(const std::vector<RepositoryDependencyEdge>& edges)
    {
        const auto result = ValidateRepositoryInvokeAcyclic(edges);
        EXPECT_TRUE(result.ok);
        EXPECT_TRUE(result.cycle_path.empty());
    }
} // namespace

TEST(RepositoryInvokeGraphTest, AcyclicEmptyGraphPasses)
{
    ExpectAcyclic({});
}

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
