#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>
#include <das/Core/GraphRuntime/SubgraphCompiler.h>

#include <das/Core/GraphRuntime/GraphCompiler.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/TaskScheduler/TaskRepositoryDtos.h>
#include <das/Utils/fmt.h>

#include <cpp_yyjson.hpp>

#include <algorithm>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// ---------------------------------------------------------------------------
// Construction / injection
// ---------------------------------------------------------------------------

SubgraphCompiler::SubgraphCompiler()
    : graph_compiler_(std::make_shared<GraphCompiler>())
{
}

void SubgraphCompiler::SetGraphCompiler(std::shared_ptr<GraphCompiler> compiler)
{
    graph_compiler_ = std::move(compiler);
}

// ---------------------------------------------------------------------------
// CompileEntryRef — single entryRef compilation
// ---------------------------------------------------------------------------

Dto::SubgraphCompileResultDto SubgraphCompiler::CompileEntryRef(
    GraphEntryId                 entry_id,
    const Dto::GraphDocumentDto& outer_document,
    EntryAccessor                entry_accessor) const
{
    Dto::SubgraphCompileResultDto result;
    result.entry_id = entry_id;

    // Load referenced entry from repository
    auto entry_opt = entry_accessor(entry_id);
    if (!entry_opt.has_value())
    {
        auto msg = DAS_FMT_NS::format(
            "SubgraphCompiler: entry_id = {} not found in repository",
            entry_id);
        DAS_CORE_LOG_ERROR(msg.c_str());
        result.diagnostics.push_back(std::move(msg));
        return result;
    }

    const auto& entry = entry_opt.value();

    // Deserialize graph_document from yyjson
    Dto::GraphDocumentDto inner_document;
    try
    {
        inner_document =
            yyjson::cast<Dto::GraphDocumentDto>(entry.graph_document);
    }
    catch (const std::exception& e)
    {
        auto msg = DAS_FMT_NS::format(
            "SubgraphCompiler: failed to deserialize graph_document "
            "for entry_id = {}: {}",
            entry_id,
            e.what());
        DAS_CORE_LOG_ERROR(msg.c_str());
        result.diagnostics.push_back(std::move(msg));
        return result;
    }

    // Compile inner graph via GraphCompiler
    result.compiled_plan = graph_compiler_->Compile(inner_document);

    // Pin revision and fingerprint
    result.revision = inner_document.version;
    result.source_fingerprint = inner_document.fingerprint;

    // Build port projection mappings
    result.input_mapping =
        BuildInputMapping(outer_document, inner_document, result.compiled_plan);
    result.output_mapping = BuildOutputMapping(
        outer_document,
        inner_document,
        result.compiled_plan);

    DAS_CORE_LOG_INFO(
        "SubgraphCompiler::CompileEntryRef: entry_id = {}, "
        "nodes = {}, edges = {}, input_mappings = {}, "
        "output_mappings = {}",
        entry_id,
        inner_document.nodes.size(),
        inner_document.edges.size(),
        result.input_mapping.size(),
        result.output_mapping.size());

    return result;
}

// ---------------------------------------------------------------------------
// CompileRecursive — public entry point
// ---------------------------------------------------------------------------

Dto::SubgraphCompileResultDto SubgraphCompiler::CompileRecursive(
    const Dto::GraphDocumentDto& document,
    EntryAccessor                entry_accessor) const
{
    std::set<GraphEntryId> visited_entry_ids;
    return CompileRecursiveImpl(document, entry_accessor, visited_entry_ids, 0);
}

// ---------------------------------------------------------------------------
// CompileRecursiveImpl — internal recursive with visited set
// ---------------------------------------------------------------------------

Dto::SubgraphCompileResultDto SubgraphCompiler::CompileRecursiveImpl(
    const Dto::GraphDocumentDto& document,
    EntryAccessor                entry_accessor,
    std::set<GraphEntryId>&      visited_entry_ids,
    int                          depth) const
{
    Dto::SubgraphCompileResultDto result;

    // Collect all entryRef nodes
    std::vector<GraphEntryId> entry_ref_ids;
    for (const auto& node : document.nodes)
    {
        if (node.target.target_kind == "entryRef"
            && node.target.entry_ref.has_value())
        {
            entry_ref_ids.push_back(node.target.entry_ref->entry_id);
        }
    }

    // No entryRefs → return empty result
    if (entry_ref_ids.empty())
    {
        return result;
    }

    for (GraphEntryId ref_entry_id : entry_ref_ids)
    {
        // Cycle detection: already visited this entry
        if (visited_entry_ids.count(ref_entry_id))
        {
            auto msg = DAS_FMT_NS::format(
                "SubgraphCompiler: cyclic entryRef detected — "
                "entry_id = {} already visited at depth = {}",
                ref_entry_id,
                depth);
            DAS_CORE_LOG_WARN(msg.c_str());
            result.diagnostics.push_back(std::move(msg));
            continue;
        }

        visited_entry_ids.insert(ref_entry_id);

        // Compile this entryRef
        auto sub_result =
            CompileEntryRef(ref_entry_id, document, entry_accessor);

        // Load the inner document for recursive compilation of its entryRefs
        auto entry_opt = entry_accessor(ref_entry_id);
        if (entry_opt.has_value())
        {
            try
            {
                auto inner_doc = yyjson::cast<Dto::GraphDocumentDto>(
                    entry_opt->graph_document);

                // Recursively compile entryRefs within the inner document
                auto inner_nested = CompileRecursiveImpl(
                    inner_doc,
                    entry_accessor,
                    visited_entry_ids,
                    depth + 1);

                // Merge nested results into the sub_result
                for (auto& ns : inner_nested.nested_snapshots)
                {
                    sub_result.nested_snapshots.push_back(std::move(ns));
                }
                sub_result.diagnostics.insert(
                    sub_result.diagnostics.end(),
                    inner_nested.diagnostics.begin(),
                    inner_nested.diagnostics.end());
            }
            catch (const std::exception& e)
            {
                auto msg = DAS_FMT_NS::format(
                    "SubgraphCompiler::CompileRecursive: failed to "
                    "deserialize inner document for entry_id = {}: {}",
                    ref_entry_id,
                    e.what());
                DAS_CORE_LOG_ERROR(msg.c_str());
                result.diagnostics.push_back(std::move(msg));
            }
        }

        // Add the sub_result to the current level
        result.nested_snapshots.push_back(std::move(sub_result));

        // NOTE: visited_entry_ids is NOT cleared between siblings.
        // This means shared references (A→B, C→B) will be treated as a
        // cycle on the second encounter. For MVP this is acceptable —
        // shared subgraph references are unusual. A future wave can
        // refine to path-based cycle detection.
    }

    return result;
}

// ---------------------------------------------------------------------------
// BuildInputMapping — outer graph_inputs → inner root node input ports
// ---------------------------------------------------------------------------

std::vector<Dto::PortMappingDto> SubgraphCompiler::BuildInputMapping(
    const Dto::GraphDocumentDto&     outer_document,
    const Dto::GraphDocumentDto&     inner_document,
    const Dto::CompiledGraphPlanDto& inner_plan) const
{
    std::vector<Dto::PortMappingDto> mappings;
    auto                             root_nodes = FindRootNodes(inner_document);

    // Build lookup: node_id → resolved_ports from compiled plan
    std::map<std::string, std::vector<Dto::GraphPortDefinitionDto>> node_ports;
    for (const auto& snapshot : inner_plan.node_snapshots)
    {
        if (root_nodes.count(snapshot.node_id))
        {
            node_ports[snapshot.node_id] = snapshot.resolved_ports;
        }
    }

    // Also use dynamic_ports from the inner document nodes
    std::map<std::string, const Dto::GraphNodeDto*> node_by_id;
    for (const auto& node : inner_document.nodes)
    {
        node_by_id[node.node_id] = &node;
    }

    // Match outer graph_input ports to inner root node ports by port_type
    for (const auto& outer_input : outer_document.graph_inputs)
    {
        bool matched = false;

        for (const auto& [nid, ports] : node_ports)
        {
            for (const auto& port : ports)
            {
                if (port.port_type == outer_input.port_type)
                {
                    Dto::PortMappingDto mapping;
                    mapping.outer_port_id = outer_input.port_id;
                    mapping.inner_port_id = port.port_id;
                    mapping.node_id = nid;
                    mapping.port_type = outer_input.port_type;
                    mappings.push_back(std::move(mapping));
                    matched = true;
                    break;
                }
            }
            if (matched)
                break;
        }

        // Fallback: try dynamic_ports on root nodes if compiled plan
        // resolved_ports is empty
        if (!matched)
        {
            for (const auto& root_id : root_nodes)
            {
                auto it = node_by_id.find(root_id);
                if (it == node_by_id.end())
                    continue;

                for (const auto& port : it->second->dynamic_ports)
                {
                    if (port.port_type == outer_input.port_type)
                    {
                        Dto::PortMappingDto mapping;
                        mapping.outer_port_id = outer_input.port_id;
                        mapping.inner_port_id = port.port_id;
                        mapping.node_id = root_id;
                        mapping.port_type = outer_input.port_type;
                        mappings.push_back(std::move(mapping));
                        matched = true;
                        break;
                    }
                }
                if (matched)
                    break;
            }
        }
    }

    return mappings;
}

// ---------------------------------------------------------------------------
// BuildOutputMapping — inner terminal node outputs → outer graph_outputs
// ---------------------------------------------------------------------------

std::vector<Dto::PortMappingDto> SubgraphCompiler::BuildOutputMapping(
    const Dto::GraphDocumentDto&     outer_document,
    const Dto::GraphDocumentDto&     inner_document,
    const Dto::CompiledGraphPlanDto& inner_plan) const
{
    std::vector<Dto::PortMappingDto> mappings;
    auto terminal_nodes = FindTerminalNodes(inner_document);

    // Build lookup: node_id → resolved_ports from compiled plan
    std::map<std::string, std::vector<Dto::GraphPortDefinitionDto>> node_ports;
    for (const auto& snapshot : inner_plan.node_snapshots)
    {
        if (terminal_nodes.count(snapshot.node_id))
        {
            node_ports[snapshot.node_id] = snapshot.resolved_ports;
        }
    }

    // Also use dynamic_ports from the inner document nodes
    std::map<std::string, const Dto::GraphNodeDto*> node_by_id;
    for (const auto& node : inner_document.nodes)
    {
        node_by_id[node.node_id] = &node;
    }

    // Match outer graph_output ports to inner terminal node output ports
    for (const auto& outer_output : outer_document.graph_outputs)
    {
        bool matched = false;

        for (const auto& [nid, ports] : node_ports)
        {
            for (const auto& port : ports)
            {
                if (port.port_type == outer_output.port_type)
                {
                    Dto::PortMappingDto mapping;
                    mapping.outer_port_id = outer_output.port_id;
                    mapping.inner_port_id = port.port_id;
                    mapping.node_id = nid;
                    mapping.port_type = outer_output.port_type;
                    mappings.push_back(std::move(mapping));
                    matched = true;
                    break;
                }
            }
            if (matched)
                break;
        }

        // Fallback: try dynamic_ports on terminal nodes
        if (!matched)
        {
            for (const auto& term_id : terminal_nodes)
            {
                auto it = node_by_id.find(term_id);
                if (it == node_by_id.end())
                    continue;

                for (const auto& port : it->second->dynamic_ports)
                {
                    if (port.port_type == outer_output.port_type)
                    {
                        Dto::PortMappingDto mapping;
                        mapping.outer_port_id = outer_output.port_id;
                        mapping.inner_port_id = port.port_id;
                        mapping.node_id = term_id;
                        mapping.port_type = outer_output.port_type;
                        mappings.push_back(std::move(mapping));
                        matched = true;
                        break;
                    }
                }
                if (matched)
                    break;
            }
        }
    }

    return mappings;
}

// ---------------------------------------------------------------------------
// Topology helpers
// ---------------------------------------------------------------------------

std::map<std::string, int> SubgraphCompiler::ComputeInDegrees(
    const Dto::GraphDocumentDto& document) const
{
    std::map<std::string, int> in_degree;
    for (const auto& node : document.nodes)
    {
        in_degree[node.node_id] = 0;
    }
    for (const auto& edge : document.edges)
    {
        in_degree[edge.target_node_id]++;
    }
    return in_degree;
}

std::map<std::string, int> SubgraphCompiler::ComputeOutDegrees(
    const Dto::GraphDocumentDto& document) const
{
    std::map<std::string, int> out_degree;
    for (const auto& node : document.nodes)
    {
        out_degree[node.node_id] = 0;
    }
    for (const auto& edge : document.edges)
    {
        out_degree[edge.source_node_id]++;
    }
    return out_degree;
}

std::set<std::string> SubgraphCompiler::FindRootNodes(
    const Dto::GraphDocumentDto& document) const
{
    auto                  in_degree = ComputeInDegrees(document);
    std::set<std::string> roots;
    for (const auto& [node_id, deg] : in_degree)
    {
        if (deg == 0)
        {
            roots.insert(node_id);
        }
    }
    if (roots.empty() && !document.nodes.empty())
    {
        // All nodes have in_degree > 0 (cycle) — treat all as roots
        for (const auto& node : document.nodes)
        {
            roots.insert(node.node_id);
        }
    }
    return roots;
}

std::set<std::string> SubgraphCompiler::FindTerminalNodes(
    const Dto::GraphDocumentDto& document) const
{
    auto                  out_degree = ComputeOutDegrees(document);
    std::set<std::string> terminals;
    for (const auto& [node_id, deg] : out_degree)
    {
        if (deg == 0)
        {
            terminals.insert(node_id);
        }
    }
    if (terminals.empty() && !document.nodes.empty())
    {
        // All nodes have outgoing edges — treat all as terminals
        for (const auto& node : document.nodes)
        {
            terminals.insert(node.node_id);
        }
    }
    return terminals;
}

DAS_CORE_GRAPHRUNTIME_NS_END
