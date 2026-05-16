#ifndef DAS_CORE_TASK_SCHEDULER_REPOSITORY_INVOKE_GRAPH_H
#define DAS_CORE_TASK_SCHEDULER_REPOSITORY_INVOKE_GRAPH_H

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Das::Core::TaskScheduler::RepositoryInvoke
{
    struct RepositoryDependencyEdge
    {
        int64_t                    source_entry_id = 0;
        int64_t                    target_entry_id = 0;
        std::optional<std::string> source_node_id;
        std::optional<std::string> source_node_label;
    };

    struct RepositoryCyclePathItem
    {
        int64_t                    entry_id = 0;
        std::optional<std::string> source_node_id;
        std::optional<std::string> source_node_label;
    };

    struct RepositoryAcyclicValidationResult
    {
        bool                                 ok = true;
        std::vector<RepositoryCyclePathItem> cycle_path;
    };

    RepositoryAcyclicValidationResult ValidateRepositoryInvokeAcyclic(
        std::span<const RepositoryDependencyEdge> edges);
} // namespace Das::Core::TaskScheduler::RepositoryInvoke

#endif // DAS_CORE_TASK_SCHEDULER_REPOSITORY_INVOKE_GRAPH_H
