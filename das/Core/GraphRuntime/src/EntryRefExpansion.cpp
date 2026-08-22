#include <das/Core/GraphRuntime/EntryRefExpansion.h>

#include <das/Core/Logger/Logger.h>
#include <das/Utils/DasJsonCore.h>

#include <string>
#include <string_view>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

namespace
{
    // Sentinel broadcast source — must match
    // GraphCompiler::GRAPH_INPUT_BROADCAST_SOURCE and
    // GraphRuntime::BROADCAST_SOURCE. A broadcast binding has no upstream
    // node, so the runtime materialises its default_value directly onto the
    // target port.
    constexpr std::string_view BROADCAST_SOURCE = "$graph_input";

    // Prefix namespacing the synthesized broadcast port id so it cannot
    // collide with a real graph_input port id.
    constexpr std::string_view ENTRY_SNAPSHOT_PORT_PREFIX = "$entry::";

    // Input port the repository-invoke wrapper reads its compiled child
    // snapshot from.
    constexpr std::string_view COMPILED_SNAPSHOT_PORT = "compiledSnapshot";

    bool TryReadEntryMarker(
        const Dto::CompiledNodeSnapshotDto& snapshot,
        int64_t&                            out_entry_id,
        std::optional<int64_t>&             out_expected_revision)
    {
        if (!snapshot.component_guid.empty()
            || snapshot.compiled_payload_json.is_null())
        {
            return false;
        }
        auto payload = snapshot.compiled_payload_json.as_object();
        if (!payload || !payload->contains(std::string_view("entryId")))
        {
            return false;
        }
        auto entry_id = (*payload)[std::string_view("entryId")].as_int();
        if (!entry_id)
        {
            return false;
        }
        out_entry_id = *entry_id;
        if (payload->contains(std::string_view("expectedRevision")))
        {
            if (auto revision =
                    (*payload)[std::string_view("expectedRevision")].as_int())
            {
                out_expected_revision = revision;
            }
        }
        return true;
    }
} // namespace

DasResult ExpandEntryRefNodes(
    Dto::CompiledGraphPlanDto& plan,
    const EntryRefResolver&    resolver,
    std::vector<std::string>*  out_diagnostics)
{
    if (!resolver)
    {
        DAS_CORE_LOG_ERROR("ExpandEntryRefNodes: resolver is not callable");
        return DAS_E_INVALID_ARGUMENT;
    }

    bool resolution_failed = false;
    for (auto& snapshot : plan.node_snapshots)
    {
        int64_t                entry_id = 0;
        std::optional<int64_t> expected_revision;
        if (!TryReadEntryMarker(snapshot, entry_id, expected_revision))
        {
            continue;
        }

        EntryRefResolution resolution = resolver(entry_id, expected_revision);
        if (!resolution.ok)
        {
            // Fail-closed: the marker node stays as-is so the failure is
            // visible in the plan, and the call reports it.
            resolution_failed = true;
            for (const auto& diagnostic : resolution.diagnostics)
            {
                DAS_CORE_LOG_WARN(
                    "ExpandEntryRefNodes: entry {} resolution failed: {}",
                    entry_id,
                    diagnostic);
                if (out_diagnostics != nullptr)
                {
                    out_diagnostics->push_back(diagnostic);
                }
            }
            continue;
        }

        auto snapshot_text =
            Das::Utils::SerializeYyjsonValue(resolution.snapshot_json);
        if (!snapshot_text)
        {
            resolution_failed = true;
            DAS_CORE_LOG_WARN(
                "ExpandEntryRefNodes: entry {} snapshot serialization failed",
                entry_id);
            if (out_diagnostics != nullptr)
            {
                out_diagnostics->push_back(
                    "entry snapshot serialization failed");
            }
            continue;
        }

        snapshot.component_guid = resolution.component_guid;
        snapshot.compiled_payload_json = yyjson::value{};

        Dto::PortBindingDto binding;
        binding.source_node_id = std::string(BROADCAST_SOURCE);
        binding.source_port_id =
            std::string(ENTRY_SNAPSHOT_PORT_PREFIX) + snapshot.node_id;
        binding.target_node_id = snapshot.node_id;
        binding.target_port_id = std::string(COMPILED_SNAPSHOT_PORT);
        binding.expected_type = "object";
        // The broadcast materialiser only handles scalar defaults, so the
        // snapshot rides as a JSON string.
        binding.default_value = yyjson::value(std::move(*snapshot_text));
        plan.binding_plan.bindings.push_back(std::move(binding));
    }

    return resolution_failed ? DAS_E_FAIL : DAS_S_OK;
}

DAS_CORE_GRAPHRUNTIME_NS_END
