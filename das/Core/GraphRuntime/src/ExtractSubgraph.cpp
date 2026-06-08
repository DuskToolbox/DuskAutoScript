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
    const std::string& port_id,
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
    GraphEntryId parent_entry_id,
    const std::set<std::string>& selected_node_ids,
    const Dto::GraphDocumentDto& parent_graph_doc,
    EntryFactory entry_factory,
    CompileCallback compile_callback) const
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
            "in_{}_{}", edge.source_node_id, edge.source_port_id);
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
            "out_{}_{}", edge.target_node_id, edge.target_port_id);
        mapping.port_type = InferPortType(
            edge.source_port_id, parent_graph_doc.graph_outputs);

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
        child_edge.source_node_id =
            result.node_id_mapping[edge.source_node_id];
        child_edge.target_node_id =
            result.node_id_mapping[edge.target_node_id];
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

DAS_CORE_GRAPHRUNTIME_NS_END
