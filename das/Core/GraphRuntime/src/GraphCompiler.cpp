#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>
#include <das/Core/GraphRuntime/GraphCompiler.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/fmt.h>

#include <queue>
#include <unordered_map>
#include <unordered_set>

#include <algorithm>
#include <cctype>
#include <functional>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// ---------------------------------------------------------------------------
// Sentinel source_node_id marking a broadcast (graph-input) binding in the
// port binding plan, as opposed to an ordinary point-to-point data edge.
// (DAS-60 Stage 2 graph_inputs broadcast binding.)
// ---------------------------------------------------------------------------
namespace
{
    constexpr std::string_view GRAPH_INPUT_BROADCAST_SOURCE = "$graph_input";
}

// ---------------------------------------------------------------------------
// Known port types for the DAS type system
// ---------------------------------------------------------------------------

static const std::unordered_set<std::string>& GetKnownTypes()
{
    static const std::unordered_set<std::string> known_types = {
        "int",
        "float",
        "string",
        "bool",
        "image",
        "base",
        "component",
        "signal",
        "json",
        "null"};
    return known_types;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

GraphCompiler::GraphCompiler() = default;

// ---------------------------------------------------------------------------
// SetFactoryManager
// ---------------------------------------------------------------------------

void GraphCompiler::SetFactoryManager(
    TaskComponentFactoryManager* factory_manager)
{
    factory_manager_ = factory_manager;
}

// ---------------------------------------------------------------------------
// SetPortSurfaceProvider
// ---------------------------------------------------------------------------

void GraphCompiler::SetPortSurfaceProvider(
    ComponentPortSurfaceProvider provider)
{
    port_surface_provider_ = std::move(provider);
}

// ---------------------------------------------------------------------------
// ResolveDeclaredPorts / ResolveNodePorts
// ---------------------------------------------------------------------------

GraphCompiler::ManifestPorts GraphCompiler::ResolveDeclaredPorts(
    const std::string&                              component_guid,
    std::unordered_map<std::string, ManifestPorts>& cache) const
{
    auto cached = cache.find(component_guid);
    if (cached != cache.end())
    {
        return cached->second;
    }

    ManifestPorts ports;
    if (factory_manager_)
    {
        ports = ReadManifest(component_guid);
    }
    if (!ports.was_resolved && port_surface_provider_)
    {
        auto surface = port_surface_provider_(component_guid);
        if (surface.has_value())
        {
            ports.inputs.clear();
            ports.outputs.clear();
            for (const auto& port : surface->inputs)
            {
                ports.inputs.push_back({port.port_id, port.port_type});
            }
            for (const auto& port : surface->outputs)
            {
                ports.outputs.push_back({port.port_id, port.port_type});
            }
            ports.was_resolved = true;
        }
    }

    cache[component_guid] = ports;
    return ports;
}

GraphCompiler::ManifestPorts GraphCompiler::ResolveNodePorts(
    const Dto::GraphNodeDto&                        node,
    std::unordered_map<std::string, ManifestPorts>& cache) const
{
    if (node.target.target_kind != "componentRef"
        || !node.target.component_ref.has_value())
    {
        return {};
    }

    ManifestPorts ports =
        ResolveDeclaredPorts(node.target.component_ref->component_guid, cache);
    if (!ports.was_resolved)
    {
        return ports;
    }

    for (const auto& dynamic_port : node.dynamic_ports)
    {
        // 静态清单不含此端口时，边角色是 dynamic port 唯一的方向证据——
        // 校验按边位置判方向（source 侧=输出、target 侧=输入），因此两侧
        // 端点集都并入（存在性与类型判定都认）。
        ports.inputs.push_back({dynamic_port.port_id, dynamic_port.port_type});
        ports.outputs.push_back({dynamic_port.port_id, dynamic_port.port_type});
    }
    return ports;
}

// ---------------------------------------------------------------------------
// ReadManifest
// ---------------------------------------------------------------------------

GraphCompiler::ManifestPorts GraphCompiler::ReadManifest(
    const std::string& component_guid) const
{
    ManifestPorts result;

    if (!factory_manager_)
    {
        DAS_CORE_LOG_WARN("Factory manager not set");
        return result;
    }

    DasGuid target_guid;
    try
    {
        target_guid =
            Das::Core::ForeignInterfaceHost::MakeDasGuid(component_guid);
    }
    catch (const std::exception&)
    {
        DAS_CORE_LOG_WARN("Invalid GUID string: {}", component_guid);
        return result;
    }

    auto definitions = factory_manager_->EnumerateDefinitions();

    for (const auto& def : definitions)
    {
        if (def.component_guid == target_guid)
        {
            result.inputs = PortsFromDefinitionList(def.definition, "inputs");
            result.outputs = PortsFromDefinitionList(def.definition, "outputs");
            result.was_resolved = true;
            return result;
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// PortsFromDefinitionList
// ---------------------------------------------------------------------------

std::vector<GraphCompiler::PortEntry> GraphCompiler::PortsFromDefinitionList(
    const yyjson::value& definition,
    const std::string&   list_key)
{
    std::vector<PortEntry> ports;

    auto def_obj = definition.as_object();
    if (!def_obj.has_value())
    {
        return ports;
    }

    if (!def_obj->contains(std::string_view(list_key)))
    {
        return ports;
    }

    auto list_val = (*def_obj)[std::string_view(list_key)];
    if (list_val.is_null())
    {
        return ports;
    }

    auto arr = list_val.as_array();
    if (!arr.has_value())
    {
        return ports;
    }

    for (const auto& entry : *arr)
    {
        auto entry_obj = entry.as_object();
        if (!entry_obj.has_value())
        {
            DAS_CORE_LOG_WARN(
                "Non-object entry in {} array, skipping",
                list_key.c_str());
            continue;
        }

        if (!entry_obj->contains("id") || !entry_obj->contains("type"))
        {
            DAS_CORE_LOG_WARN(
                "Missing id or type in {} entry, skipping",
                list_key.c_str());
            continue;
        }

        auto id_val = (*entry_obj)[std::string_view("id")];
        auto type_val = (*entry_obj)[std::string_view("type")];

        if (id_val.is_null() || type_val.is_null())
        {
            DAS_CORE_LOG_WARN(
                "Null id or type in {} entry, skipping",
                list_key.c_str());
            continue;
        }

        PortEntry port;

        auto id_str = id_val.as_string();
        if (!id_str.has_value())
        {
            DAS_CORE_LOG_WARN(
                "Port id is not a string in {} entry, skipping",
                list_key.c_str());
            continue;
        }
        port.port_id = std::string(*id_str);

        auto type_str = type_val.as_string();
        if (!type_str.has_value())
        {
            DAS_CORE_LOG_WARN(
                "Port type is not a string in {} entry, skipping",
                list_key.c_str());
            continue;
        }
        port.port_type = std::string(*type_str);

        ports.push_back(std::move(port));
    }

    return ports;
}

// ---------------------------------------------------------------------------
// IsTypeCompatible
// ---------------------------------------------------------------------------

bool GraphCompiler::IsTypeCompatible(
    const std::string& source_type,
    const std::string& target_type)
{
    // Same type — always compatible
    if (source_type == target_type)
    {
        return true;
    }

    // base -> component: base can be QI'd to component
    if (source_type == "base" && target_type == "component")
    {
        return true;
    }

    // component -> base: component IS base (COM identity)
    if (source_type == "component" && target_type == "base")
    {
        return true;
    }

    // If either type is unknown (not in known set), treat as compatible
    // (permissive — the compiler emits a warning, not a rejection)
    const auto& known = GetKnownTypes();
    if (!known.contains(source_type) || !known.contains(target_type))
    {
        return true;
    }

    // All other known-type mismatches are incompatible
    return false;
}

// ---------------------------------------------------------------------------
// ValidateEdgePorts
// ---------------------------------------------------------------------------

std::vector<CompileEdgeDiagnostic> GraphCompiler::ValidateEdgePorts(
    const Dto::GraphDocumentDto& document)
{
    std::vector<CompileEdgeDiagnostic> diagnostics;

    if (!factory_manager_)
    {
        DAS_CORE_LOG_WARN("Factory manager not set");
        return diagnostics;
    }

    // Build node index for O(1) lookup by node_id
    std::unordered_map<std::string, const Dto::GraphNodeDto*> node_index;
    for (const auto& node : document.nodes)
    {
        node_index[node.node_id] = &node;
    }

    // Cache manifest lookups to avoid redundant EnumerateDefinitions calls
    std::unordered_map<std::string, ManifestPorts> manifest_cache;

    for (const auto& edge : document.edges)
    {
        // --- Node existence checks ---
        auto src_it = node_index.find(edge.source_node_id);
        if (src_it == node_index.end())
        {
            CompileEdgeDiagnostic d;
            d.kind = CompileDiagnosticKind::NodeNotFound;
            d.node_id = edge.source_node_id;
            d.edge_id = edge.edge_id;
            d.message = DAS_FMT_NS::format(
                "Source node '{}' not found in graph",
                edge.source_node_id);
            diagnostics.push_back(std::move(d));
            continue; // Can't validate ports without the node
        }

        auto tgt_it = node_index.find(edge.target_node_id);
        if (tgt_it == node_index.end())
        {
            CompileEdgeDiagnostic d;
            d.kind = CompileDiagnosticKind::NodeNotFound;
            d.node_id = edge.target_node_id;
            d.edge_id = edge.edge_id;
            d.message = DAS_FMT_NS::format(
                "Target node '{}' not found in graph",
                edge.target_node_id);
            diagnostics.push_back(std::move(d));
            continue;
        }

        const auto& src_node = *src_it->second;
        const auto& tgt_node = *tgt_it->second;

        // --- Source manifest resolution ---
        ManifestPorts src_manifest;
        if (src_node.target.target_kind == "componentRef")
        {
            if (!src_node.target.component_ref.has_value())
            {
                CompileEdgeDiagnostic d;
                d.kind = CompileDiagnosticKind::UnresolvableComponentGuid;
                d.node_id = edge.source_node_id;
                d.edge_id = edge.edge_id;
                d.message = DAS_FMT_NS::format(
                    "Source node '{}' has componentRef but missing "
                    "component_ref field",
                    edge.source_node_id);
                diagnostics.push_back(std::move(d));
                continue;
            }

            src_manifest = ResolveNodePorts(src_node, manifest_cache);

            // Check if manifest was resolved.  A component may legitimately
            // declare zero ports, so we use an explicit flag instead of
            // relying on empty port vectors.
            if (!src_manifest.was_resolved)
            {
                CompileEdgeDiagnostic d;
                d.kind = CompileDiagnosticKind::UnresolvableComponentGuid;
                d.node_id = edge.source_node_id;
                d.edge_id = edge.edge_id;
                d.message = DAS_FMT_NS::format(
                    "Cannot resolve manifest for source node '{}' "
                    "component_guid='{}'",
                    edge.source_node_id,
                    src_node.target.component_ref->component_guid);
                diagnostics.push_back(std::move(d));
                continue;
            }
        }

        // --- Target manifest resolution ---
        ManifestPorts tgt_manifest;
        if (tgt_node.target.target_kind == "componentRef")
        {
            if (!tgt_node.target.component_ref.has_value())
            {
                CompileEdgeDiagnostic d;
                d.kind = CompileDiagnosticKind::UnresolvableComponentGuid;
                d.node_id = edge.target_node_id;
                d.edge_id = edge.edge_id;
                d.message = DAS_FMT_NS::format(
                    "Target node '{}' has componentRef but missing "
                    "component_ref field",
                    edge.target_node_id);
                diagnostics.push_back(std::move(d));
                continue;
            }

            tgt_manifest = ResolveNodePorts(tgt_node, manifest_cache);

            if (!tgt_manifest.was_resolved)
            {
                CompileEdgeDiagnostic d;
                d.kind = CompileDiagnosticKind::UnresolvableComponentGuid;
                d.node_id = edge.target_node_id;
                d.edge_id = edge.edge_id;
                d.message = DAS_FMT_NS::format(
                    "Cannot resolve manifest for target node '{}' "
                    "component_guid='{}'",
                    edge.target_node_id,
                    tgt_node.target.component_ref->component_guid);
                diagnostics.push_back(std::move(d));
                continue;
            }
        }

        // --- Source port existence check ---
        const PortEntry* src_port = nullptr;
        for (const auto& p : src_manifest.outputs)
        {
            if (p.port_id == edge.source_port_id)
            {
                src_port = &p;
                break;
            }
        }

        if (!src_port)
        {
            CompileEdgeDiagnostic d;
            d.kind = CompileDiagnosticKind::PortNotFound;
            d.node_id = edge.source_node_id;
            d.port_id = edge.source_port_id;
            d.edge_id = edge.edge_id;
            d.direction = CompileEdgeDiagnostic::PortDirection::Output;
            d.message = DAS_FMT_NS::format(
                "Port '{}' not found in node '{}' manifest outputs",
                edge.source_port_id,
                edge.source_node_id);
            diagnostics.push_back(std::move(d));
            continue;
        }

        // --- Target port existence check ---
        const PortEntry* tgt_port = nullptr;
        for (const auto& p : tgt_manifest.inputs)
        {
            if (p.port_id == edge.target_port_id)
            {
                tgt_port = &p;
                break;
            }
        }

        if (!tgt_port)
        {
            CompileEdgeDiagnostic d;
            d.kind = CompileDiagnosticKind::PortNotFound;
            d.node_id = edge.target_node_id;
            d.port_id = edge.target_port_id;
            d.edge_id = edge.edge_id;
            d.direction = CompileEdgeDiagnostic::PortDirection::Input;
            d.message = DAS_FMT_NS::format(
                "Port '{}' not found in node '{}' manifest inputs",
                edge.target_port_id,
                edge.target_node_id);
            diagnostics.push_back(std::move(d));
            continue;
        }

        // --- Unknown type warning ---
        const auto& known = GetKnownTypes();
        if (!known.contains(src_port->port_type)
            || !known.contains(tgt_port->port_type))
        {
            CompileEdgeDiagnostic d;
            d.kind = CompileDiagnosticKind::UnknownPortType;
            d.node_id = edge.source_node_id;
            d.port_id = edge.source_port_id;
            d.edge_id = edge.edge_id;
            d.direction = CompileEdgeDiagnostic::PortDirection::Output;
            d.message = DAS_FMT_NS::format(
                "Unknown port type: source='{}' target='{}'",
                src_port->port_type,
                tgt_port->port_type);
            d.actual_type = src_port->port_type;
            d.expected_type = tgt_port->port_type;
            diagnostics.push_back(std::move(d));
            // Unknown types are warnings — don't block, but continue to
            // type check
        }

        // --- Type compatibility check ---
        if (!IsTypeCompatible(src_port->port_type, tgt_port->port_type))
        {
            CompileEdgeDiagnostic d;
            d.kind = CompileDiagnosticKind::TypeMismatch;
            d.node_id = edge.target_node_id;
            d.port_id = edge.target_port_id;
            d.edge_id = edge.edge_id;
            d.direction = CompileEdgeDiagnostic::PortDirection::Input;
            d.expected_type = tgt_port->port_type;
            d.actual_type = src_port->port_type;
            d.message = DAS_FMT_NS::format(
                "Type mismatch on edge '{}': source port '{}' has type "
                "'{}', target port '{}' expects type '{}'",
                edge.edge_id,
                edge.source_port_id,
                src_port->port_type,
                edge.target_port_id,
                tgt_port->port_type);
            diagnostics.push_back(std::move(d));
        }
    }

    return diagnostics;
}

// ---------------------------------------------------------------------------
// BuildNodeIndex
// ---------------------------------------------------------------------------

std::unordered_map<std::string, const Dto::GraphNodeDto*>
GraphCompiler::BuildNodeIndex(const Dto::GraphDocumentDto& document)
{
    std::unordered_map<std::string, const Dto::GraphNodeDto*> index;
    for (const auto& node : document.nodes)
    {
        index[node.node_id] = &node;
    }
    return index;
}

// ---------------------------------------------------------------------------
// ComputeExecutionOrder (signal-aware, deterministic Kahn)
// ---------------------------------------------------------------------------

std::vector<std::string> GraphCompiler::ComputeExecutionOrder(
    const Dto::GraphDocumentDto& document)
{
    return ComputeTopology(document).execution_order;
}

// ---------------------------------------------------------------------------
// ComputeTopology — DFS back-edge classification + deterministic Kahn
// ---------------------------------------------------------------------------

GraphCompiler::TopologyResult GraphCompiler::ComputeTopology(
    const Dto::GraphDocumentDto& document)
{
    TopologyResult result;

    if (document.nodes.empty())
    {
        return result;
    }

    // Stable per-node index → deterministic Kahn tie-breaking (signals the
    // unordered_map nondeterminism called out in DAS-60 Stage 2).
    std::unordered_map<std::string, std::size_t> node_index;
    for (std::size_t i = 0; i < document.nodes.size(); ++i)
    {
        node_index[document.nodes[i].node_id] = i;
    }

    // Adjacency in document edge order (deterministic DFS traversal).
    std::unordered_map<std::string, std::vector<const Dto::GraphEdgeDto*>>
        adjacency;
    for (const auto& edge : document.edges)
    {
        adjacency[edge.source_node_id].push_back(&edge);
    }

    // DFS back-edge classification. An edge to a node still on the recursion
    // stack (an ancestor) is a back-edge. Removing every DFS back-edge always
    // yields a DAG, so the Kahn run below is guaranteed to order every node.
    enum class Color
    {
        White,
        Gray,
        Black
    };
    std::unordered_map<std::string, Color> color;
    for (const auto& node : document.nodes)
    {
        color[node.node_id] = Color::White;
    }

    std::unordered_set<std::string> back_edge_ids;

    std::function<void(const std::string&)> dfs = [&](const std::string& u)
    {
        color[u] = Color::Gray;

        auto it = adjacency.find(u);
        if (it != adjacency.end())
        {
            for (const Dto::GraphEdgeDto* edge : it->second)
            {
                const std::string& v = edge->target_node_id;
                auto               cit = color.find(v);
                if (cit == color.end())
                {
                    // Edge to a node outside the graph — ValidateEdgePorts
                    // reports it; topology ignores it.
                    continue;
                }

                if (cit->second == Color::Gray)
                {
                    // Back-edge: u -> ancestor v closes a cycle.
                    back_edge_ids.insert(edge->edge_id);
                    if (edge->edge_type == "signal")
                    {
                        Dto::BackEdgeDto back;
                        back.edge_id = edge->edge_id;
                        back.source_node_id = edge->source_node_id;
                        back.source_port_id = edge->source_port_id;
                        back.target_node_id = edge->target_node_id;
                        back.target_port_id = edge->target_port_id;
                        back.loop_head_node_id = edge->target_node_id;
                        result.back_edges.push_back(std::move(back));
                    }
                    else
                    {
                        result.has_data_cycle = true;
                    }
                }
                else if (cit->second == Color::White)
                {
                    dfs(v);
                }
                // Black: forward/cross edge — not a back-edge.
            }
        }

        color[u] = Color::Black;
    };

    for (const auto& node : document.nodes)
    {
        if (color[node.node_id] == Color::White)
        {
            dfs(node.node_id);
        }
    }

    // Signal routes: every signal edge becomes a route (forward + back).
    // activation_condition names the source port whose emission fires the
    // route; Stage 4 may refine it to a concrete signal value via manifest
    // tags. Back-edges appear here too — they carry the loop-back signal.
    for (const auto& edge : document.edges)
    {
        if (edge.edge_type != "signal")
        {
            continue;
        }

        Dto::SignalRouteDto route;
        route.source_node_id = edge.source_node_id;
        route.source_port_id = edge.source_port_id;
        route.target_node_id = edge.target_node_id;
        route.target_port_id = edge.target_port_id;

        auto cond = Das::Utils::MakeYyjsonObject();
        auto cond_obj = *cond.as_object();
        cond_obj[std::string_view("signal")] = std::make_pair(
            std::string_view(edge.source_port_id),
            yyjson::copy_string);
        route.activation_condition = std::move(cond);

        result.signal_routes.push_back(std::move(route));
    }

    // Kahn over (data edges ∪ forward signal edges) — every back-edge removed
    // so this subgraph is a DAG and all nodes get ordered.
    std::unordered_map<std::string, std::vector<std::string>> forward_adj;
    std::unordered_map<std::string, int>                      in_degree;
    for (const auto& node : document.nodes)
    {
        in_degree[node.node_id] = 0;
    }

    for (const auto& edge : document.edges)
    {
        if (back_edge_ids.count(edge.edge_id) > 0)
        {
            continue;
        }
        if (node_index.count(edge.source_node_id) == 0
            || node_index.count(edge.target_node_id) == 0)
        {
            continue;
        }
        forward_adj[edge.source_node_id].push_back(edge.target_node_id);
        in_degree[edge.target_node_id]++;
    }

    auto cmp = [&](const std::string& a, const std::string& b)
    { return node_index[a] > node_index[b]; };
    std::priority_queue<std::string, std::vector<std::string>, decltype(cmp)>
        heap(cmp);
    for (const auto& node : document.nodes)
    {
        if (in_degree[node.node_id] == 0)
        {
            heap.push(node.node_id);
        }
    }

    while (!heap.empty())
    {
        const std::string current = heap.top();
        heap.pop();
        result.execution_order.push_back(current);

        auto it = forward_adj.find(current);
        if (it != forward_adj.end())
        {
            for (const auto& neighbor : it->second)
            {
                if (--in_degree[neighbor] == 0)
                {
                    heap.push(neighbor);
                }
            }
        }
    }

    // A cycle among data edges is the only legitimate reason to reject the
    // graph; surface it as an empty execution_order so Compile() reports
    // CyclicEdgeGraph. (Signal back-edges are tolerated, never fatal.)
    if (result.has_data_cycle)
    {
        result.execution_order.clear();
    }

    return result;
}

// ---------------------------------------------------------------------------
// GeneratePortBindingPlan
// ---------------------------------------------------------------------------

Dto::PortBindingPlanDto GraphCompiler::GeneratePortBindingPlan(
    const Dto::GraphDocumentDto& document)
{
    Dto::PortBindingPlanDto plan;

    if (!factory_manager_ && !port_surface_provider_)
    {
        return plan;
    }

    auto node_index = BuildNodeIndex(document);

    // Cache manifest lookups
    std::unordered_map<std::string, ManifestPorts> manifest_cache;

    for (const auto& edge : document.edges)
    {
        Dto::PortBindingDto binding;
        binding.source_node_id = edge.source_node_id;
        binding.source_port_id = edge.source_port_id;
        binding.target_node_id = edge.target_node_id;
        binding.target_port_id = edge.target_port_id;

        // Resolve expected_type from source manifest
        auto src_it = node_index.find(edge.source_node_id);
        if (src_it != node_index.end())
        {
            const auto& src_node = *src_it->second;
            if (src_node.target.target_kind == "componentRef")
            {
                auto src_manifest = ResolveNodePorts(src_node, manifest_cache);
                for (const auto& p : src_manifest.outputs)
                {
                    if (p.port_id == edge.source_port_id)
                    {
                        binding.expected_type = p.port_type;
                        break;
                    }
                }
            }
            // Non-componentRef sources keep an empty expected_type — the
            // binding plan never invents a type it cannot resolve.
        }

        plan.bindings.push_back(std::move(binding));
    }

    // Broadcast bindings: graph_inputs (graph-level settings, e.g. MAA
    // global_option) are bound to every node input port that declares using
    // them — by port_id name match. Unlike point-to-point data edges this is
    // a one-to-many binding; its source_node_id is the sentinel
    // GRAPH_INPUT_BROADCAST_SOURCE and it carries the graph input's default.
    std::unordered_set<std::string> explicit_graph_input_names;
    for (const auto& graph_input : document.graph_inputs)
    {
        explicit_graph_input_names.insert(graph_input.port_id);
        for (const auto& node : document.nodes)
        {
            if (node.target.target_kind != "componentRef"
                || !node.target.component_ref.has_value())
            {
                continue;
            }

            auto manifest = ResolveNodePorts(node, manifest_cache);
            if (!manifest.was_resolved)
            {
                continue;
            }

            for (const auto& in_port : manifest.inputs)
            {
                if (in_port.port_id != graph_input.port_id)
                {
                    continue;
                }

                Dto::PortBindingDto binding;
                binding.source_node_id =
                    std::string(GRAPH_INPUT_BROADCAST_SOURCE);
                binding.source_port_id = graph_input.port_id;
                binding.target_node_id = node.node_id;
                binding.target_port_id = in_port.port_id;
                binding.expected_type = graph_input.port_type;
                binding.default_value =
                    Das::Utils::CloneYyjsonValue(graph_input.default_value);
                plan.bindings.push_back(std::move(binding));
            }
        }
    }

    // Derived broadcast bindings: every dangling input port (no incoming
    // edge) is a boundary input of this graph — it receives the same
    // $graph_input sentinel binding carrying the port's declared default, so
    // the runtime materialises it and the caller-side value forwarding has a
    // patch target. Explicit graph_inputs win on name clashes — the derived
    // set never duplicates them.
    Dto::BoundaryPortsDto boundary = DeriveBoundaryPorts(document);
    for (const auto& boundary_input : boundary.inputs)
    {
        if (explicit_graph_input_names.count(boundary_input.port_id) > 0)
        {
            continue;
        }
        for (const auto& node_id : boundary_input.node_ids)
        {
            Dto::PortBindingDto binding;
            binding.source_node_id = std::string(GRAPH_INPUT_BROADCAST_SOURCE);
            binding.source_port_id = boundary_input.port_id;
            binding.target_node_id = node_id;
            binding.target_port_id = boundary_input.port_id;
            binding.expected_type = boundary_input.port_type;
            binding.default_value =
                Das::Utils::CloneYyjsonValue(boundary_input.default_value);
            plan.bindings.push_back(std::move(binding));
        }
    }

    return plan;
}

// ---------------------------------------------------------------------------
// ValidateRequiredFields
// ---------------------------------------------------------------------------

std::vector<CompileEdgeDiagnostic> GraphCompiler::ValidateRequiredFields(
    const Dto::GraphDocumentDto& document,
    const std::unordered_map<std::string, std::unordered_set<std::string>>&
        required_fields)
{
    std::vector<CompileEdgeDiagnostic> diagnostics;
    if (required_fields.empty())
    {
        return diagnostics;
    }
    for (const auto& node : document.nodes)
    {
        if (node.target.target_kind != "componentRef"
            || !node.target.component_ref.has_value())
        {
            continue;
        }
        const auto required_it =
            required_fields.find(node.target.component_ref->component_guid);
        if (required_it == required_fields.end())
        {
            continue;
        }
        auto settings_obj = node.settings.as_object();
        for (const auto& field_name : required_it->second)
        {
            bool satisfied = false;
            if (settings_obj.has_value()
                && settings_obj->contains(std::string_view(field_name)))
            {
                const auto& value =
                    (*settings_obj)[std::string_view(field_name)];
                if (!value.is_null())
                {
                    // 空值判定与运行时 launch exe_path.empty() 对齐：字符串
                    // 额外要求非纯空白；数值/布尔/结构类型键存在即满足。
                    if (auto text = value.as_string(); text.has_value())
                    {
                        satisfied = std::any_of(
                            text->begin(),
                            text->end(),
                            [](char ch)
                            {
                                return std::isspace(
                                           static_cast<unsigned char>(ch))
                                       == 0;
                            });
                    }
                    else
                    {
                        satisfied = true;
                    }
                }
            }
            if (!satisfied)
            {
                CompileEdgeDiagnostic diagnostic;
                diagnostic.kind = CompileDiagnosticKind::RequiredFieldMissing;
                diagnostic.node_id = node.node_id;
                diagnostic.message =
                    "Required field '" + field_name + "' has no value";
                diagnostics.push_back(std::move(diagnostic));
            }
        }
    }
    return diagnostics;
}

// ---------------------------------------------------------------------------
// DeriveBoundaryPorts
// ---------------------------------------------------------------------------

Dto::BoundaryPortsDto GraphCompiler::DeriveBoundaryPorts(
    const Dto::GraphDocumentDto& document) const
{
    ComponentPortSurfaceProvider provider;
    if (factory_manager_ || port_surface_provider_)
    {
        // 声明端口面统一经 ResolveDeclaredPorts（manifest 优先、catalog
        // provider 兜底），两条声明通道在推导侧不分先后语义。
        provider = [this](const std::string& component_guid)
            -> std::optional<ComponentPortSurface>
        {
            std::unordered_map<std::string, ManifestPorts> cache;
            const ManifestPorts                            ports =
                ResolveDeclaredPorts(component_guid, cache);
            if (!ports.was_resolved)
            {
                return std::nullopt;
            }
            ComponentPortSurface surface;
            for (const auto& port : ports.inputs)
            {
                Dto::GraphPortDefinitionDto definition;
                definition.port_id = port.port_id;
                definition.port_type = port.port_type;
                surface.inputs.push_back(std::move(definition));
            }
            for (const auto& port : ports.outputs)
            {
                Dto::GraphPortDefinitionDto definition;
                definition.port_id = port.port_id;
                definition.port_type = port.port_type;
                surface.outputs.push_back(std::move(definition));
            }
            return surface;
        };
    }
    return DeriveBoundaryPorts(document, provider);
}

Dto::BoundaryPortsDto GraphCompiler::DeriveBoundaryPorts(
    const Dto::GraphDocumentDto&        document,
    const ComponentPortSurfaceProvider& provider)
{
    // 边角色证据（与 authoring 投影层的 CollectPortIds 同思想）：文档内对
    // 端口方向唯一可用的证据是边的位置——target 侧引用=输入、source 侧
    // 引用=输出。悬空判定不需要仓库访问。
    std::unordered_map<std::string, std::unordered_set<std::string>>
        input_roles;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        output_roles;
    for (const auto& edge : document.edges)
    {
        input_roles[edge.target_node_id].insert(edge.target_port_id);
        output_roles[edge.source_node_id].insert(edge.source_port_id);
    }

    std::unordered_set<std::string> explicit_graph_input_names;
    for (const auto& graph_input : document.graph_inputs)
    {
        explicit_graph_input_names.insert(graph_input.port_id);
    }

    Dto::BoundaryPortsDto                        boundary;
    std::unordered_map<std::string, std::size_t> merged_input_index;
    std::unordered_map<std::string, std::size_t> merged_output_index;

    auto referenced_as = [](const std::unordered_map<
                                std::string,
                                std::unordered_set<std::string>>& roles,
                            const std::string&                    node_id,
                            const std::string& port_id) -> bool
    {
        auto it = roles.find(node_id);
        return it != roles.end() && it->second.count(port_id) > 0;
    };

    for (const auto& node : document.nodes)
    {
        std::optional<ComponentPortSurface> surface;
        if (provider && node.target.target_kind == "componentRef"
            && node.target.component_ref.has_value())
        {
            surface = provider(node.target.component_ref->component_guid);
        }

        auto add_boundary_input = [&](const Dto::GraphPortDefinitionDto& port)
        {
            if (explicit_graph_input_names.count(port.port_id) > 0)
            {
                // 显式 graph_inputs 同名优先：显式通道已覆盖该口，推导集
                // 不重复收编。
                return;
            }
            if (referenced_as(input_roles, node.node_id, port.port_id))
            {
                return; // 有入边，非悬空。
            }
            auto merged = merged_input_index.find(port.port_id);
            if (merged == merged_input_index.end())
            {
                Dto::BoundaryPortDto entry;
                entry.port_id = port.port_id;
                entry.display_label = port.display_label;
                entry.port_type = port.port_type;
                entry.is_required = port.is_required;
                entry.default_value =
                    Das::Utils::CloneYyjsonValue(port.default_value);
                entry.node_ids.push_back(node.node_id);
                merged_input_index[port.port_id] = boundary.inputs.size();
                boundary.inputs.push_back(std::move(entry));
                return;
            }
            // by-name 合并：同名悬空输入口共用一个边界口（广播语义），
            // 先出现者贡献元数据。
            auto& existing = boundary.inputs[merged->second];
            if (std::find(
                    existing.node_ids.begin(),
                    existing.node_ids.end(),
                    node.node_id)
                == existing.node_ids.end())
            {
                existing.node_ids.push_back(node.node_id);
            }
        };

        auto add_boundary_output = [&](const Dto::GraphPortDefinitionDto& port)
        {
            if (referenced_as(output_roles, node.node_id, port.port_id))
            {
                return; // 有出边，非悬空。
            }
            auto merged = merged_output_index.find(port.port_id);
            if (merged == merged_output_index.end())
            {
                Dto::BoundaryPortDto entry;
                entry.port_id = port.port_id;
                entry.display_label = port.display_label;
                entry.port_type = port.port_type;
                entry.is_required = port.is_required;
                entry.default_value =
                    Das::Utils::CloneYyjsonValue(port.default_value);
                entry.node_ids.push_back(node.node_id);
                merged_output_index[port.port_id] = boundary.outputs.size();
                boundary.outputs.push_back(std::move(entry));
                return;
            }
            auto& existing = boundary.outputs[merged->second];
            if (std::find(
                    existing.node_ids.begin(),
                    existing.node_ids.end(),
                    node.node_id)
                == existing.node_ids.end())
            {
                existing.node_ids.push_back(node.node_id);
            }
        };

        if (surface.has_value())
        {
            for (const auto& port : surface->inputs)
            {
                add_boundary_input(port);
            }
            for (const auto& port : surface->outputs)
            {
                add_boundary_output(port);
            }
        }

        for (const auto& dynamic_port : node.dynamic_ports)
        {
            // 非纯输出（无输出证据）→ 输入候选；非纯输入 → 输出候选。
            // 无证据端口两个方向都悬空（投影层同款双向处置）；纯输入/纯
            // 输出端口各只有一个方向的候选，且该方向的入边/出边证据使其
            // 天然非悬空。
            const bool referenced_as_input =
                referenced_as(input_roles, node.node_id, dynamic_port.port_id);
            const bool referenced_as_output =
                referenced_as(output_roles, node.node_id, dynamic_port.port_id);
            if (!referenced_as_output)
            {
                add_boundary_input(dynamic_port);
            }
            if (!referenced_as_input)
            {
                add_boundary_output(dynamic_port);
            }
        }
    }

    return boundary;
}

// ---------------------------------------------------------------------------
// DiagnosticToJson
// ---------------------------------------------------------------------------

yyjson::value GraphCompiler::DiagnosticToJson(const CompileEdgeDiagnostic& diag)
{
    auto payload = Das::Utils::MakeYyjsonObject();
    auto obj = *payload.as_object();

    auto kind_str = std::to_string(static_cast<int>(diag.kind));
    obj[std::string_view("kind")] =
        std::make_pair(std::string_view(kind_str), yyjson::copy_string);
    obj[std::string_view("node_id")] =
        std::make_pair(std::string_view(diag.node_id), yyjson::copy_string);
    obj[std::string_view("port_id")] =
        std::make_pair(std::string_view(diag.port_id), yyjson::copy_string);
    obj[std::string_view("edge_id")] =
        std::make_pair(std::string_view(diag.edge_id), yyjson::copy_string);
    obj[std::string_view("message")] =
        std::make_pair(std::string_view(diag.message), yyjson::copy_string);
    obj[std::string_view("expected_type")] = std::make_pair(
        std::string_view(diag.expected_type),
        yyjson::copy_string);
    obj[std::string_view("actual_type")] =
        std::make_pair(std::string_view(diag.actual_type), yyjson::copy_string);

    return payload;
}

// ---------------------------------------------------------------------------
// Compile
// ---------------------------------------------------------------------------

Dto::CompiledGraphPlanDto GraphCompiler::Compile(
    const Dto::GraphDocumentDto& document)
{
    static const std::
        unordered_map<std::string, std::unordered_set<std::string>>
            no_required_fields;
    return Compile(document, no_required_fields);
}

Dto::CompiledGraphPlanDto GraphCompiler::Compile(
    const Dto::GraphDocumentDto& document,
    const std::unordered_map<std::string, std::unordered_set<std::string>>&
        required_fields)
{
    Dto::CompiledGraphPlanDto plan;

    // Copy source metadata
    plan.document_id = document.document_id;
    plan.source_revision = document.version;
    plan.source_fingerprint = document.fingerprint;

    // Edge port validation
    auto edge_diagnostics = ValidateEdgePorts(document);
    for (const auto& d : edge_diagnostics)
    {
        plan.diagnostics.push_back(DiagnosticToJson(d));
    }

    // Required-field validation (the session layer injects the
    // catalog-derived map — the compiler has no catalog access).
    auto required_diagnostics =
        ValidateRequiredFields(document, required_fields);
    for (const auto& d : required_diagnostics)
    {
        plan.diagnostics.push_back(DiagnosticToJson(d));
    }

    // Signal-aware topological sort (single pass — also yields
    // signal_routes and back_edges for the compiled plan).
    auto topology = ComputeTopology(document);
    plan.execution_order = std::move(topology.execution_order);
    plan.signal_routes = std::move(topology.signal_routes);
    plan.back_edges = std::move(topology.back_edges);
    if (plan.execution_order.empty() && !document.nodes.empty())
    {
        auto payload = Das::Utils::MakeYyjsonObject();
        auto obj = *payload.as_object();
        obj[std::string_view("kind")] = std::make_pair(
            std::string_view(
                std::to_string(
                    static_cast<int>(CompileDiagnosticKind::CyclicEdgeGraph))),
            yyjson::copy_string);
        obj[std::string_view("message")] = std::make_pair(
            std::string_view(
                "Graph contains a cycle - topological sort failed"),
            yyjson::copy_string);
        plan.diagnostics.push_back(std::move(payload));
    }

    // Port binding plan
    plan.binding_plan = GeneratePortBindingPlan(document);

    // 推导边界口清单入编译产物：输入侧已由 GeneratePortBindingPlan 落成
    // broadcast 绑定（与显式 graph_inputs 合并、显式优先）；输出侧是执行
    // 侧 graphOutputs 导出的依据。
    plan.boundary_ports = DeriveBoundaryPorts(document);

    // Build node snapshots from graph document nodes. A node whose target is
    // not a componentRef keeps an empty component_guid; the runtime's
    // Configure step rejects it with INVALID_ARGUMENT (fail-closed handling
    // of unknown target kinds — no silent fallback).
    for (const auto& node : document.nodes)
    {
        Dto::CompiledNodeSnapshotDto snapshot;
        snapshot.node_id = node.node_id;

        // Extract component_guid from the node's target
        if (node.target.target_kind == "componentRef"
            && node.target.component_ref.has_value())
        {
            snapshot.component_guid = node.target.component_ref->component_guid;
        }

        // Copy settings if present
        if (!node.settings.is_null())
        {
            snapshot.compiled_settings =
                Das::Utils::CloneYyjsonValue(node.settings);
        }

        // Copy dynamic ports as resolved_ports
        for (const auto& dp : node.dynamic_ports)
        {
            snapshot.resolved_ports.push_back(dp);
        }

        plan.node_snapshots.push_back(std::move(snapshot));
    }

    return plan;
}

DAS_CORE_GRAPHRUNTIME_NS_END
