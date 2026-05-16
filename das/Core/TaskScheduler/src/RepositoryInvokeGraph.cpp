#include <das/Core/TaskScheduler/RepositoryInvokeGraph.h>

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Das::Core::TaskScheduler::RepositoryInvoke
{
    namespace
    {
        using AdjacencyList =
            std::unordered_map<int64_t, std::vector<int64_t>>;

        AdjacencyList BuildAdjacency(
            std::span<const RepositoryDependencyEdge> edges)
        {
            AdjacencyList adjacency;
            for (const auto& edge : edges)
            {
                adjacency[edge.source_entry_id].push_back(edge.target_entry_id);
                adjacency.try_emplace(edge.target_entry_id);
            }

            for (auto& [_, targets] : adjacency)
            {
                std::ranges::sort(targets);
            }

            return adjacency;
        }
    } // namespace

    RepositoryAcyclicValidationResult ValidateRepositoryInvokeAcyclic(
        std::span<const RepositoryDependencyEdge> edges)
    {
        const auto adjacency = BuildAdjacency(edges);

        std::unordered_set<int64_t> visited;
        std::vector<int64_t>        stack;

        for (const auto& [entry_id, _] : adjacency)
        {
            if (visited.contains(entry_id))
            {
                continue;
            }

            stack.push_back(entry_id);
            visited.insert(entry_id);

            while (!stack.empty())
            {
                const auto current = stack.back();
                stack.pop_back();

                const auto iterator = adjacency.find(current);
                if (iterator == adjacency.end())
                {
                    continue;
                }

                for (const auto target : iterator->second)
                {
                    if (!visited.contains(target))
                    {
                        visited.insert(target);
                        stack.push_back(target);
                    }
                }
            }
        }

        return {};
    }
} // namespace Das::Core::TaskScheduler::RepositoryInvoke
