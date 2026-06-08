#ifndef DAS_CORE_GRAPHRUNTIME_REFMANAGER_H
#define DAS_CORE_GRAPHRUNTIME_REFMANAGER_H

#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphEntryId.h>
#include <das/Core/TaskScheduler/TaskRepositoryDtos.h>

#include <map>
#include <string>
#include <vector>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

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
};

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_REFMANAGER_H
