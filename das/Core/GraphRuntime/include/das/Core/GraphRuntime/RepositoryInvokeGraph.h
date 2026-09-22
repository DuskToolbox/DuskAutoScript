#ifndef DAS_CORE_GRAPH_RUNTIME_REPOSITORY_INVOKE_GRAPH_H
#define DAS_CORE_GRAPH_RUNTIME_REPOSITORY_INVOKE_GRAPH_H

#include <cassert>
#include <cpp_yyjson.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Das::Core::GraphRuntime::RepositoryInvoke
{
    // Repository-invoke dependency-edge and cycle validation algorithms.
    // Home is GraphRuntime (compile algorithms live with the closure-driven
    // graph compiler); moved here from the retired TaskScheduler
    // RepositoryInvokeCompiler / RepositoryInvokeGraph location.
    // The compile-time recursion stack inside GraphCompiler is the primary
    // cycle detector for the closure-injection chain; these pure functions
    // remain the static dependency-edge vocabulary for authoring-time
    // graph checks.

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

    // Extracts repository reference edges (taskRepositoryRef /
    // settings.repositoryRef keys — wrapped-object {kind, entryId} or plain
    // integer entryId shape) from an authoring graph document.
    std::vector<RepositoryDependencyEdge>
    ExtractRepositoryInvokeDependencyEdges(
        int64_t              source_entry_id,
        const yyjson::value& source_graph);
} // namespace Das::Core::GraphRuntime::RepositoryInvoke

#endif // DAS_CORE_GRAPH_RUNTIME_REPOSITORY_INVOKE_GRAPH_H
