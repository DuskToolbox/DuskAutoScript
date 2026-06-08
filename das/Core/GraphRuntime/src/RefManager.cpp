#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/GraphRuntime/RefManager.h>
#include <das/Core/Logger/Logger.h>

#include <cpp_yyjson.hpp>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

std::map<GraphEntryId, std::vector<std::string>> RefManager::ScanEntryRefs(
    const std::vector<
        Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto>& entries)
    const
{
    std::map<GraphEntryId, std::vector<std::string>> ref_map;

    for (const auto& entry : entries)
    {
        // Skip entries without a graph_document
        if (entry.graph_document.is_null())
        {
            continue;
        }

        // Deserialize graph_document to GraphDocumentDto
        Dto::GraphDocumentDto graph_doc;
        try
        {
            graph_doc =
                yyjson::cast<Dto::GraphDocumentDto>(entry.graph_document);
        }
        catch (const std::exception& e)
        {
            DAS_CORE_LOG_ERROR(
                "RefManager: failed to deserialize graph_document for "
                "entry_id = {}: {}",
                entry.entry_id,
                e.what());
            continue;
        }

        // Iterate nodes, collect entryRef targets
        for (const auto& node : graph_doc.nodes)
        {
            if (node.target.target_kind == "entryRef"
                && node.target.entry_ref.has_value())
            {
                GraphEntryId ref_entry_id = node.target.entry_ref->entry_id;
                ref_map[ref_entry_id].push_back(graph_doc.document_id);
            }
        }
    }

    return ref_map;
}

int RefManager::ComputeRefCount(
    GraphEntryId entry_id,
    const std::vector<
        Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto>& entries)
    const
{
    auto ref_counts = ComputeRefCounts(entries);
    auto it = ref_counts.find(entry_id);
    return it != ref_counts.end() ? it->second : 0;
}

std::map<GraphEntryId, int> RefManager::ComputeRefCounts(
    const std::vector<
        Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto>& entries)
    const
{
    auto                        ref_map = ScanEntryRefs(entries);
    std::map<GraphEntryId, int> ref_counts;

    for (const auto& [id, docs] : ref_map)
    {
        ref_counts[id] = static_cast<int>(docs.size());
    }

    return ref_counts;
}

DAS_CORE_GRAPHRUNTIME_NS_END
