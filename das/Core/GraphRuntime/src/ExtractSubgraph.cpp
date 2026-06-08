#include <das/Core/GraphRuntime/ExtractSubgraph.h>

#include <cpp_yyjson.hpp>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/fmt.h>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

std::string ExtractSubgraph::MakeChildNodeId(const std::string& old_node_id)
{
    return "child_" + old_node_id;
}

std::string ExtractSubgraph::InferPortType(
    const std::string&                              port_id,
    const std::vector<Dto::GraphPortDefinitionDto>& port_defs)
{
    for (const auto& def : port_defs)
    {
        if (def.port_id == port_id)
        {
            return def.port_type;
        }
    }
    return {};
}

ExtractResult ExtractSubgraph::Extract(
    GraphEntryId                 parent_entry_id,
    const std::set<std::string>& selected_node_ids,
    const Dto::GraphDocumentDto& parent_graph_doc,
    EntryFactory                 entry_factory,
    CompileCallback              compile_callback) const
{
    ExtractResult result;

    // --- Pre-conditions ---
    if (selected_node_ids.empty())
    {
        result.diagnostics.push_back("No nodes selected for extract");
        return result;
    }

    if (selected_node_ids.size() == parent_graph_doc.nodes.size())
    {
        result.diagnostics.push_back(
            "Cannot extract entire graph — all nodes selected");
        return result;
    }

    // --- Build node_id mapping (old parent → new child) ---
    for (const auto& node : parent_graph_doc.nodes)
    {
        if (selected_node_ids.count(node.node_id))
        {
            result.node_id_mapping[node.node_id] =
                MakeChildNodeId(node.node_id);
        }
    }

    // --- Classify edges ---
    std::vector<Dto::GraphEdgeDto> internal_edges;
    std::vector<Dto::GraphEdgeDto> inbound_edges;
    std::vector<Dto::GraphEdgeDto> outbound_edges;

    for (const auto& edge : parent_graph_doc.edges)
    {
        bool src_in = selected_node_ids.count(edge.source_node_id) > 0;
        bool tgt_in = selected_node_ids.count(edge.target_node_id) > 0;

        if (src_in && tgt_in)
        {
            internal_edges.push_back(edge);
        }
        else if (!src_in && tgt_in)
        {
            inbound_edges.push_back(edge);
        }
        else if (src_in && !tgt_in)
        {
            outbound_edges.push_back(edge);
        }
        // else: external — ignore
    }

    // --- Generate child input ports (from inbound edges) ---
    for (const auto& edge : inbound_edges)
    {
        ExtractPortMapping mapping;
        mapping.old_parent_node_id = edge.source_node_id;
        mapping.old_parent_port_id = edge.source_port_id;
        mapping.child_port_id = DAS_FMT_NS::format(
            "in_{}_{}",
            edge.source_node_id,
            edge.source_port_id);
        mapping.port_type =
            InferPortType(edge.target_port_id, parent_graph_doc.graph_inputs);

        result.input_port_mappings.push_back(mapping);
    }

    // --- Generate child output ports (from outbound edges) ---
    for (const auto& edge : outbound_edges)
    {
        ExtractPortMapping mapping;
        mapping.old_parent_node_id = edge.target_node_id;
        mapping.old_parent_port_id = edge.target_port_id;
        mapping.child_port_id = DAS_FMT_NS::format(
            "out_{}_{}",
            edge.target_node_id,
            edge.target_port_id);
        mapping.port_type =
            InferPortType(edge.source_port_id, parent_graph_doc.graph_outputs);

        result.output_port_mappings.push_back(mapping);
    }

    // --- Build child GraphDocument ---
    Dto::GraphDocumentDto child_doc;
    child_doc.document_id =
        DAS_FMT_NS::format("child_of_{}", parent_graph_doc.document_id);

    // Copy selected nodes with new child node_ids
    for (const auto& node : parent_graph_doc.nodes)
    {
        if (!selected_node_ids.count(node.node_id))
        {
            continue;
        }

        Dto::GraphNodeDto child_node = node;
        child_node.node_id = result.node_id_mapping[node.node_id];
        child_doc.nodes.push_back(child_node);
    }

    // Move internal edges (remapped source/target to child node_ids)
    for (const auto& edge : internal_edges)
    {
        Dto::GraphEdgeDto child_edge = edge;
        child_edge.source_node_id = result.node_id_mapping[edge.source_node_id];
        child_edge.target_node_id = result.node_id_mapping[edge.target_node_id];
        child_doc.edges.push_back(child_edge);
    }

    // Add generated input ports
    for (const auto& mapping : result.input_port_mappings)
    {
        Dto::GraphPortDefinitionDto port_def;
        port_def.port_id = mapping.child_port_id;
        port_def.port_type = mapping.port_type;
        child_doc.graph_inputs.push_back(port_def);
    }

    // Add generated output ports
    for (const auto& mapping : result.output_port_mappings)
    {
        Dto::GraphPortDefinitionDto port_def;
        port_def.port_id = mapping.child_port_id;
        port_def.port_type = mapping.port_type;
        child_doc.graph_outputs.push_back(port_def);
    }

    result.child_graph_document = child_doc;

    // --- Create child entry via factory ---
    Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto child_entry;
    child_entry.graph_document = yyjson::object(child_doc);

    result.child_entry_id = entry_factory(child_entry);

    if (result.child_entry_id == 0)
    {
        result.diagnostics.push_back(
            "entry_factory returned 0 — child entry not created");
        return result;
    }

    // --- Compile child entry ---
    auto compiled_plan = compile_callback(result.child_entry_id);
    result.revision = 1;
    result.fingerprint = compiled_plan.compiled_fingerprint;

    DAS_CORE_LOG_INFO(
        "ExtractSubgraph: parent = {}, child = {}, "
        "nodes = {}, internal_edges = {}, "
        "inbound_ports = {}, outbound_ports = {}",
        parent_entry_id,
        result.child_entry_id,
        selected_node_ids.size(),
        internal_edges.size(),
        result.input_port_mappings.size(),
        result.output_port_mappings.size());

    return result;
}

ReplaceResult ExtractSubgraph::ReplaceWithSubgraph(
    const Dto::GraphDocumentDto& parent_graph_doc,
    const ExtractResult&         extract_result,
    const std::set<std::string>& selected_node_ids) const
{
    ReplaceResult result;

    // --- Validate ExtractResult ---
    if (extract_result.child_entry_id == 0)
    {
        result.diagnostics.push_back(
            "Invalid ExtractResult: child_entry_id = 0");
        return result;
    }

    if (selected_node_ids.empty())
    {
        result.diagnostics.push_back(
            "No nodes selected for replacement");
        return result;
    }

    // --- Generate replacement node_id ---
    result.replacement_node_id = DAS_FMT_NS::format(
        "extracted_{}_node", extract_result.child_entry_id);

    // --- Build undo snapshot ---
    result.undo_snapshot.replacement_node_id = result.replacement_node_id;
    result.undo_snapshot.original_graph_document = parent_graph_doc;

    // --- Build updated GraphDocument ---
    Dto::GraphDocumentDto updated;
    updated.document_id = parent_graph_doc.document_id;
    updated.version     = parent_graph_doc.version;
    updated.fingerprint = parent_graph_doc.fingerprint;
    updated.graph_inputs  = parent_graph_doc.graph_inputs;
    updated.graph_outputs = parent_graph_doc.graph_outputs;

    // Filter nodes: keep non-selected, collect selected into snapshot
    for (const auto& node : parent_graph_doc.nodes)
    {
        if (selected_node_ids.count(node.node_id))
        {
            result.undo_snapshot.removed_nodes.push_back(node);
        }
        else
        {
            updated.nodes.push_back(node);
        }
    }

    // Insert replacement entryRef node
    Dto::GraphNodeDto replacement_node;
    replacement_node.node_id = result.replacement_node_id;
    replacement_node.target  = Dto::GraphNodeTargetDto{
        "entryRef",
        std::nullopt,
        Dto::EntryRefDto{
            "entryRef",
            extract_result.child_entry_id,
            extract_result.revision,
            extract_result.fingerprint}};
    updated.nodes.push_back(replacement_node);

    // Classify and rewire edges
    for (const auto& edge : parent_graph_doc.edges)
    {
        bool src_in = selected_node_ids.count(edge.source_node_id) > 0;
        bool tgt_in = selected_node_ids.count(edge.target_node_id) > 0;

        if (src_in && tgt_in)
        {
            // Internal edge — moved to child, record in snapshot
            result.undo_snapshot.removed_edges.push_back(edge);
        }
        else if (!src_in && tgt_in)
        {
            // Inbound edge: external source → replacement_node
            Dto::GraphEdgeDto new_edge = edge;
            new_edge.target_node_id = result.replacement_node_id;

            for (const auto& mapping : extract_result.input_port_mappings)
            {
                if (mapping.old_parent_node_id == edge.source_node_id &&
                    mapping.old_parent_port_id == edge.source_port_id)
                {
                    new_edge.target_port_id = mapping.child_port_id;
                    break;
                }
            }

            updated.edges.push_back(new_edge);
            result.undo_snapshot.removed_edges.push_back(edge);
        }
        else if (src_in && !tgt_in)
        {
            // Outbound edge: replacement_node → external target
            Dto::GraphEdgeDto new_edge = edge;
            new_edge.source_node_id = result.replacement_node_id;

            for (const auto& mapping : extract_result.output_port_mappings)
            {
                if (mapping.old_parent_node_id == edge.target_node_id &&
                    mapping.old_parent_port_id == edge.target_port_id)
                {
                    new_edge.source_port_id = mapping.child_port_id;
                    break;
                }
            }

            updated.edges.push_back(new_edge);
            result.undo_snapshot.removed_edges.push_back(edge);
        }
        else
        {
            // External edge — pass through unchanged
            updated.edges.push_back(edge);
        }
    }

    result.updated_graph_document = updated;

    DAS_CORE_LOG_INFO(
        "ReplaceWithSubgraph: replacement = {}, "
        "removed_nodes = {}, removed_edges = {}, "
        "updated_nodes = {}, updated_edges = {}",
        result.replacement_node_id,
        result.undo_snapshot.removed_nodes.size(),
        result.undo_snapshot.removed_edges.size(),
        updated.nodes.size(),
        updated.edges.size());

    return result;
}

Dto::GraphDocumentDto ExtractSubgraph::UndoExtract(
    const ParentSnapshot& undo_snapshot,
    DeleteCallback        delete_callback,
    RefCountCallback      ref_count_callback) const
{
    // --- Validate snapshot ---
    if (undo_snapshot.replacement_node_id.empty() ||
        undo_snapshot.removed_nodes.empty())
    {
        DAS_CORE_LOG_INFO(
            "UndoExtract: invalid snapshot — "
            "replacement_node_id empty or removed_nodes empty");
        return undo_snapshot.original_graph_document;
    }

    // Restore from the original parent document
    Dto::GraphDocumentDto restored =
        undo_snapshot.original_graph_document;

    // --- Delete child entry if refCount == 0 ---
    if (ref_count_callback)
    {
        GraphEntryId child_entry_id = 0;

        // Parse child_entry_id from replacement_node_id format
        // "extracted_{child_entry_id}_node"
        const auto& prefix = std::string("extracted_");
        const auto& suffix = std::string("_node");
        const auto& rid    = undo_snapshot.replacement_node_id;

        if (rid.size() > prefix.size() + suffix.size() &&
            rid.substr(0, prefix.size()) == prefix &&
            rid.substr(rid.size() - suffix.size()) == suffix)
        {
            auto id_str = rid.substr(
                prefix.size(),
                rid.size() - prefix.size() - suffix.size());
            child_entry_id = std::stoll(id_str);
        }

        if (child_entry_id != 0)
        {
            int ref_count = ref_count_callback(child_entry_id);
            if (ref_count == 0)
            {
                if (delete_callback)
                {
                    delete_callback(child_entry_id);
                    DAS_CORE_LOG_INFO(
                        "UndoExtract: deleted orphan child entry = {}",
                        child_entry_id);
                }
            }
            else
            {
                DAS_CORE_LOG_INFO(
                    "UndoExtract: child entry = {} still has "
                    "refCount = {}, skipping deletion",
                    child_entry_id,
                    ref_count);
            }
        }
    }

    DAS_CORE_LOG_INFO(
        "UndoExtract: restored nodes = {}, edges = {}",
        restored.nodes.size(),
        restored.edges.size());

    return restored;
}

DAS_CORE_GRAPHRUNTIME_NS_END
