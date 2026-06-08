#ifndef DAS_CORE_GRAPHRUNTIME_REFMANAGER_H
#define DAS_CORE_GRAPHRUNTIME_REFMANAGER_H

#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphEntryId.h>
#include <das/Core/TaskScheduler/TaskRepositoryDtos.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

/// Result of a delete ref-check.
struct DeleteCheckResult
{
    bool                          allowed = true;
    int                           ref_count = 0;
    std::vector<std::string>      referencing_document_ids;
    std::string                   reason;
};

/// Callback for creating new repository entries during deep clone.
/// Receives the source entry (graph_document + compiled_artifact) and
/// returns the new entry_id assigned by the repository.
/// Tests inject a mock; production wires to TaskRepository::CreateEntry().
using EntryFactory = std::function<GraphEntryId(
    const Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto&
        source_entry)>;

/// Result of a deep clone operation.
struct DeepCloneResult
{
    GraphEntryId                            new_entry_id = 0;
    std::map<GraphEntryId, GraphEntryId>    id_mapping;
};

/// RefManager: Cross-GraphDocument entryRef scanner.
///
/// Scans all GraphDocument entries for entryRef nodes and computes
/// per-entry reference counts. The refCount is always recomputed from
/// the full document set — no mutable counter stored on entries.
///
/// Per v25:
///   - entryRef lives in GraphDocumentDto.nodes[].target
///   - refCount = number of entryRef nodes (across ALL GraphDocuments)
///     pointing to a given entry_id
///   - Self-references and cross-references are counted normally
class RefManager
{
public:
    RefManager() = default;

    /// Scan all entries and return raw entryRef data:
    ///   entry_id → list of document_ids that reference it.
    /// Useful for diagnostics and UI display of "used by" lists.
    std::map<GraphEntryId, std::vector<std::string>> ScanEntryRefs(
        const std::vector<
            Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto>&
            entries) const;

    /// Compute reference count for a single entry_id.
    /// Returns 0 if the entry_id is not referenced by any GraphDocument.
    int ComputeRefCount(
        GraphEntryId entry_id,
        const std::vector<
            Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto>&
            entries) const;

    /// Compute reference counts for ALL referenced entry_ids.
    /// Returns map: entry_id → refCount.
    /// Unreferenced entries do NOT appear in the map.
    std::map<GraphEntryId, int> ComputeRefCounts(
        const std::vector<
            Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto>&
            entries) const;

    /// Check if an entry can be safely deleted.
    /// Returns {allowed=false} if any other GraphDocument references
    /// this entry via entryRef. The reason includes referencing document IDs.
    /// This is a pure predicate — it does NOT modify entries.
    DeleteCheckResult CheckDelete(
        GraphEntryId entry_id,
        const std::vector<
            Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto>&
            entries) const;

    /// Shallow-copy an EntryRefDto — shares the same entry_id.
    /// Per v25: default copy behavior for entryRef nodes.
    static Das::Core::GraphRuntime::Dto::EntryRefDto ShallowCopyEntryRef(
        const Das::Core::GraphRuntime::Dto::EntryRefDto& source);

    /// Deep-clone a graph entry and ALL its entryRef descendants.
    /// Recursively creates new repository entries (via entry_factory),
    /// copies graph_document + compiled_artifact, assigns new entry_ids,
    /// and remaps entryRef nodes in the cloned documents.
    ///
    /// Parameters:
    ///   root_entry_id — the entry to deep-clone
    ///   all_entries   — all repository entries (for reading child docs)
    ///   entry_factory — callback to create new entries
    ///
    /// Returns: new root entry_id + old→new id_mapping for the full subtree.
    DeepCloneResult DeepCloneEntryRef(
        GraphEntryId root_entry_id,
        const std::vector<
            Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto>&
            all_entries,
        EntryFactory entry_factory) const;
};

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_REFMANAGER_H
