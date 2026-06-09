#ifndef DAS_CORE_GRAPHRUNTIME_SUBGRAPHCOMPILER_H
#define DAS_CORE_GRAPHRUNTIME_SUBGRAPHCOMPILER_H

#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/GraphRuntime/GraphEntryId.h>
#include <das/Core/TaskScheduler/TaskRepositoryDtos.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

/// Callback to access repository entries by entry_id.
/// Returns std::nullopt if the entry does not exist.
using EntryAccessor = std::function<std::optional<
    Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto>(
    GraphEntryId entry_id)>;

/// Compiles entryRef nodes into SubgraphCompileResultDto entries.
///
/// Walks each entryRef target: loads the referenced graph document,
/// compiles it via GraphCompiler, computes port projection mappings
/// (outer↔inner), and recursively compiles any nested entryRefs.
class SubgraphCompiler
{
public:
    SubgraphCompiler();

    /// Compile a single entryRef target into a SubgraphCompileResultDto.
    ///
    /// Loads the referenced entry, deserializes its graph_document,
    /// delegates to GraphCompiler::Compile(), and computes port
    /// projection mappings between the outer calling graph and the
    /// inner referenced graph.
    ///
    /// Parameters:
    ///   entry_id       — the referenced entry to compile
    ///   outer_document — the calling (outer) graph for port projection
    ///   entry_accessor — callback to read repository entries
    Dto::SubgraphCompileResultDto CompileEntryRef(
        GraphEntryId                 entry_id,
        const Dto::GraphDocumentDto& outer_document,
        EntryAccessor                entry_accessor) const;

    /// Recursively compile ALL entryRef nodes in a graph document.
    ///
    /// For each node with target_kind == "entryRef", calls
    /// CompileEntryRef(). For each resulting subgraph result,
    /// recurses into the subgraph's own entryRef nodes.
    /// Maintains a visited set to detect cycles.
    Dto::SubgraphCompileResultDto CompileRecursive(
        const Dto::GraphDocumentDto& document,
        EntryAccessor                entry_accessor) const;

    /// Set the GraphCompiler instance to use for subgraph compilation.
    /// If not set, a default GraphCompiler is constructed internally.
    void SetGraphCompiler(std::shared_ptr<class GraphCompiler> compiler);

private:
    std::shared_ptr<class GraphCompiler> graph_compiler_;

    // --- Port Projection Helpers ---

    /// Build input_mapping: outer graph_inputs → inner root node input ports
    std::vector<Dto::PortMappingDto> BuildInputMapping(
        const Dto::GraphDocumentDto&     outer_document,
        const Dto::GraphDocumentDto&     inner_document,
        const Dto::CompiledGraphPlanDto& inner_plan) const;

    /// Build output_mapping: inner terminal node outputs → outer graph_outputs
    std::vector<Dto::PortMappingDto> BuildOutputMapping(
        const Dto::GraphDocumentDto&     outer_document,
        const Dto::GraphDocumentDto&     inner_document,
        const Dto::CompiledGraphPlanDto& inner_plan) const;

    /// Find root nodes in a graph: nodes with in_degree == 0
    std::set<std::string> FindRootNodes(
        const Dto::GraphDocumentDto& document) const;

    /// Find terminal (leaf) nodes in a graph: nodes with out_degree == 0
    std::set<std::string> FindTerminalNodes(
        const Dto::GraphDocumentDto& document) const;

    /// Compute in-degree map for all nodes
    std::map<std::string, int> ComputeInDegrees(
        const Dto::GraphDocumentDto& document) const;

    /// Compute out-degree map for all nodes
    std::map<std::string, int> ComputeOutDegrees(
        const Dto::GraphDocumentDto& document) const;

    /// Internal recursive compilation with visited set for cycle guard
    /// @param compiled_cache tracks fully-compiled entries to avoid
    ///        redundant recompilation of shared subgraph references
    Dto::SubgraphCompileResultDto CompileRecursiveImpl(
        const Dto::GraphDocumentDto& document,
        EntryAccessor                entry_accessor,
        std::set<GraphEntryId>&      visited_entry_ids,
        std::set<GraphEntryId>&      compiled_cache,
        int                          depth) const;
};

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_SUBGRAPHCOMPILER_H
