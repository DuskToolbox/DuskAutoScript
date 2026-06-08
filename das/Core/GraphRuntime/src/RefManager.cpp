#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/GraphRuntime/RefManager.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/fmt.h>

#include <cpp_yyjson.hpp>

#include <algorithm>
#include <set>

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

// --- CheckDelete ---

DeleteCheckResult RefManager::CheckDelete(
    GraphEntryId entry_id,
    const std::vector<
        Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto>& entries)
    const
{
    DeleteCheckResult result;

    auto ref_map = ScanEntryRefs(entries);
    auto it = ref_map.find(entry_id);

    if (it != ref_map.end())
    {
        result.ref_count = static_cast<int>(it->second.size());
        result.referencing_document_ids = it->second;
        result.allowed = (result.ref_count == 0);

        if (!result.allowed)
        {
            std::string doc_ids;
            for (size_t i = 0; i < it->second.size(); ++i)
            {
                if (i > 0)
                {
                    doc_ids += ", ";
                }
                doc_ids += it->second[i];
            }
            result.reason = DAS_FMT_NS::format(
                "Referenced by {} document(s): [{}]",
                result.ref_count,
                doc_ids);
        }
        else
        {
            result.reason = "No references found — safe to delete";
        }
    }
    else
    {
        result.allowed = true;
        result.ref_count = 0;
        result.reason = "Entry not referenced by any GraphDocument";
    }

    return result;
}

// --- ShallowCopyEntryRef ---

Das::Core::GraphRuntime::Dto::EntryRefDto RefManager::ShallowCopyEntryRef(
    const Das::Core::GraphRuntime::Dto::EntryRefDto& source)
{
    Das::Core::GraphRuntime::Dto::EntryRefDto copy;
    copy.kind = source.kind;
    copy.entry_id = source.entry_id;
    copy.expected_revision = source.expected_revision;
    copy.source_fingerprint = source.source_fingerprint;
    return copy;
}

// --- DeepCloneEntryRef ---

DeepCloneResult RefManager::DeepCloneEntryRef(
    GraphEntryId root_entry_id,
    const std::vector<
        Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto>&
                 all_entries,
    EntryFactory entry_factory) const
{
    DeepCloneResult result;

    // Find the root entry
    auto root_it = std::find_if(
        all_entries.begin(),
        all_entries.end(),
        [root_entry_id](const auto& e) { return e.entry_id == root_entry_id; });

    if (root_it == all_entries.end())
    {
        DAS_CORE_LOG_ERROR(
            "DeepCloneEntryRef: root entry_id = {} not found",
            root_entry_id);
        return result;
    }

    // DFS: collect all reachable entry_ids via entryRef
    std::set<GraphEntryId>    visited;
    std::vector<GraphEntryId> to_clone;
    std::vector<GraphEntryId> frontier = {root_entry_id};

    while (!frontier.empty())
    {
        GraphEntryId current = frontier.back();
        frontier.pop_back();

        if (visited.count(current))
        {
            continue;
        }
        visited.insert(current);
        to_clone.push_back(current);

        // Find this entry in all_entries
        auto entry_it = std::find_if(
            all_entries.begin(),
            all_entries.end(),
            [current](const auto& e) { return e.entry_id == current; });

        if (entry_it == all_entries.end())
        {
            continue;
        }

        // Deserialize graph_document to find entryRef children
        if (!entry_it->graph_document.is_null())
        {
            try
            {
                auto graph_doc = yyjson::cast<Dto::GraphDocumentDto>(
                    entry_it->graph_document);

                for (const auto& node : graph_doc.nodes)
                {
                    if (node.target.target_kind == "entryRef"
                        && node.target.entry_ref.has_value())
                    {
                        GraphEntryId child_id = node.target.entry_ref->entry_id;
                        if (!visited.count(child_id))
                        {
                            frontier.push_back(child_id);
                        }
                    }
                }
            }
            catch (const std::exception& e)
            {
                DAS_CORE_LOG_ERROR(
                    "DeepCloneEntryRef: failed to deserialize "
                    "graph_document for entry_id = {}: {}",
                    current,
                    e.what());
            }
        }
    }

    // Clone leaves first (reverse topological order: children before parents)
    std::map<GraphEntryId, GraphEntryId> old_to_new;

    for (auto it = to_clone.rbegin(); it != to_clone.rend(); ++it)
    {
        GraphEntryId old_id = *it;

        auto entry_it = std::find_if(
            all_entries.begin(),
            all_entries.end(),
            [old_id](const auto& e) { return e.entry_id == old_id; });

        if (entry_it == all_entries.end())
        {
            continue;
        }

        // Prepare source entry for factory
        Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto
            new_entry = *entry_it;

        // Remap entryRef nodes in the graph_document
        if (!new_entry.graph_document.is_null())
        {
            try
            {
                auto graph_doc = yyjson::cast<Dto::GraphDocumentDto>(
                    new_entry.graph_document);

                for (auto& node : graph_doc.nodes)
                {
                    if (node.target.target_kind == "entryRef"
                        && node.target.entry_ref.has_value())
                    {
                        GraphEntryId old_child =
                            node.target.entry_ref->entry_id;
                        auto mapping_it = old_to_new.find(old_child);
                        if (mapping_it != old_to_new.end())
                        {
                            node.target.entry_ref->entry_id =
                                mapping_it->second;
                        }
                    }
                }

                new_entry.graph_document = yyjson::object(graph_doc);
            }
            catch (const std::exception& e)
            {
                DAS_CORE_LOG_ERROR(
                    "DeepCloneEntryRef: failed to remap entryRef for "
                    "old entry_id = {}: {}",
                    old_id,
                    e.what());
            }
        }

        // Create the new entry via factory
        GraphEntryId new_id = entry_factory(new_entry);
        old_to_new[old_id] = new_id;
    }

    result.id_mapping = old_to_new;
    result.new_entry_id = old_to_new[root_entry_id];

    return result;
}

DAS_CORE_GRAPHRUNTIME_NS_END
