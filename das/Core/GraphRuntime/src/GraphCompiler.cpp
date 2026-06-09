#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>
#include <das/Core/GraphRuntime/GraphCompiler.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/fmt.h>

#include <queue>
#include <unordered_map>
#include <unordered_set>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

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

    // Helper: resolve manifest for a node
    auto resolve_manifest = [&](const Dto::GraphNodeDto& node) -> ManifestPorts
    {
        if (node.target.target_kind != "componentRef"
            || !node.target.component_ref.has_value())
        {
            // entryRef nodes: no manifest resolution in this wave
            return {};
        }

        const auto& guid = node.target.component_ref->component_guid;

        auto it = manifest_cache.find(guid);
        if (it != manifest_cache.end())
        {
            return it->second;
        }

        auto ports = ReadManifest(guid);
        manifest_cache[guid] = ports;
        return ports;
    };

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

            src_manifest = resolve_manifest(src_node);

            // Check if manifest was resolved (empty inputs+outputs means
            // unresolved)
            if (src_manifest.inputs.empty() && src_manifest.outputs.empty())
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
        else if (src_node.target.target_kind == "entryRef")
        {
            // entryRef nodes: skip port validation (Wave 20 responsibility)
            continue;
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

            tgt_manifest = resolve_manifest(tgt_node);

            if (tgt_manifest.inputs.empty() && tgt_manifest.outputs.empty())
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
        else if (tgt_node.target.target_kind == "entryRef")
        {
            // entryRef nodes: skip port validation
            continue;
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
// ComputeExecutionOrder (Kahn's algorithm)
// ---------------------------------------------------------------------------

std::vector<std::string> GraphCompiler::ComputeExecutionOrder(
    const Dto::GraphDocumentDto& document)
{
    std::vector<std::string> execution_order;

    if (document.nodes.empty())
    {
        return execution_order;
    }

    // Build adjacency list and in-degree map
    std::unordered_map<std::string, std::vector<std::string>> adjacency;
    std::unordered_map<std::string, int>                      in_degree;

    for (const auto& node : document.nodes)
    {
        in_degree[node.node_id] = 0;
    }

    for (const auto& edge : document.edges)
    {
        adjacency[edge.source_node_id].push_back(edge.target_node_id);
        in_degree[edge.target_node_id]++;
    }

    // Initialize queue with all zero in-degree nodes
    std::queue<std::string> queue;
    for (const auto& [node_id, degree] : in_degree)
    {
        if (degree == 0)
        {
            queue.push(node_id);
        }
    }

    // BFS
    while (!queue.empty())
    {
        std::string current = std::move(queue.front());
        queue.pop();
        execution_order.push_back(std::move(current));

        auto it = adjacency.find(execution_order.back());
        if (it != adjacency.end())
        {
            for (const auto& neighbor : it->second)
            {
                in_degree[neighbor]--;
                if (in_degree[neighbor] == 0)
                {
                    queue.push(neighbor);
                }
            }
        }
    }

    // Cycle detection: not all nodes visited
    if (execution_order.size() != document.nodes.size())
    {
        return {};
    }

    return execution_order;
}

// ---------------------------------------------------------------------------
// GeneratePortBindingPlan
// ---------------------------------------------------------------------------

Dto::PortBindingPlanDto GraphCompiler::GeneratePortBindingPlan(
    const Dto::GraphDocumentDto& document)
{
    Dto::PortBindingPlanDto plan;

    if (!factory_manager_)
    {
        return plan;
    }

    auto node_index = BuildNodeIndex(document);

    // Cache manifest lookups
    std::unordered_map<std::string, ManifestPorts> manifest_cache;

    auto resolve_manifest = [&](const Dto::GraphNodeDto& node) -> ManifestPorts
    {
        if (node.target.target_kind != "componentRef"
            || !node.target.component_ref.has_value())
        {
            return {};
        }

        const auto& guid = node.target.component_ref->component_guid;
        auto        it = manifest_cache.find(guid);
        if (it != manifest_cache.end())
        {
            return it->second;
        }

        auto ports = ReadManifest(guid);
        manifest_cache[guid] = ports;
        return ports;
    };

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
                auto src_manifest = resolve_manifest(src_node);
                for (const auto& p : src_manifest.outputs)
                {
                    if (p.port_id == edge.source_port_id)
                    {
                        binding.expected_type = p.port_type;
                        break;
                    }
                }
            }
            // entryRef nodes: expected_type remains empty
        }

        plan.bindings.push_back(std::move(binding));
    }

    return plan;
}

// ---------------------------------------------------------------------------
// DetectCyclicEntryRefs
// ---------------------------------------------------------------------------

std::vector<CompileEdgeDiagnostic> GraphCompiler::DetectCyclicEntryRefs(
    const Dto::GraphDocumentDto& document)
{
    std::vector<CompileEdgeDiagnostic> diagnostics;

    for (const auto& node : document.nodes)
    {
        if (node.target.target_kind != "entryRef"
            || !node.target.entry_ref.has_value())
        {
            continue;
        }

        const auto& entry_ref = *node.target.entry_ref;

        // Self-reference: source_fingerprint matches the document's
        // fingerprint
        if (entry_ref.source_fingerprint.has_value()
            && !entry_ref.source_fingerprint->empty()
            && entry_ref.source_fingerprint == document.fingerprint)
        {
            CompileEdgeDiagnostic d;
            d.kind = CompileDiagnosticKind::CyclicEntryRef;
            d.node_id = node.node_id;
            d.message = DAS_FMT_NS::format(
                "Cyclic entryRef detected: node '{}' references "
                "entry_id={} with matching fingerprint",
                node.node_id,
                entry_ref.entry_id);
            diagnostics.push_back(std::move(d));
        }
    }

    return diagnostics;
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
// GenerateCompiledFingerprint
// ---------------------------------------------------------------------------

std::string GraphCompiler::GenerateCompiledFingerprint(
    const std::string&              document_id,
    int32_t                         source_revision,
    const std::vector<std::string>& execution_order)
{
    std::string composite = document_id + ":" + std::to_string(source_revision);
    for (const auto& node_id : execution_order)
    {
        composite += ":" + node_id;
    }
    return std::to_string(std::hash<std::string>{}(composite));
}

// ---------------------------------------------------------------------------
// Compile
// ---------------------------------------------------------------------------

Dto::CompiledGraphPlanDto GraphCompiler::Compile(
    const Dto::GraphDocumentDto& document)
{
    Dto::CompiledGraphPlanDto plan;

    // Phase 1: Copy source metadata
    plan.document_id = document.document_id;
    plan.source_revision = document.version;
    plan.source_fingerprint = document.fingerprint;

    // Phase 2: Edge port validation
    auto edge_diagnostics = ValidateEdgePorts(document);
    for (const auto& d : edge_diagnostics)
    {
        plan.diagnostics.push_back(DiagnosticToJson(d));
    }

    // Phase 3: Topological sort
    plan.execution_order = ComputeExecutionOrder(document);
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

    // Phase 4: Port binding plan
    plan.binding_plan = GeneratePortBindingPlan(document);

    // Phase 5: entryRef cycle detection
    auto ref_diagnostics = DetectCyclicEntryRefs(document);
    for (const auto& d : ref_diagnostics)
    {
        plan.diagnostics.push_back(DiagnosticToJson(d));
    }

    // Phase 6: Generate compiled fingerprint
    plan.compiled_fingerprint = GenerateCompiledFingerprint(
        plan.document_id,
        plan.source_revision,
        plan.execution_order);

    // Phase 7: Build node snapshots from graph document nodes
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
