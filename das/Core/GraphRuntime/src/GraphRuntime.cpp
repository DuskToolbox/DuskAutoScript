#include <das/Core/GraphRuntime/GraphRuntime.h>

#include <das/Core/Exceptions/InvalidGuidStringException.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/DoAdapter.h>
#include <das/Core/GraphRuntime/LegacyJsonAdapter.h>
#include <das/Core/GraphRuntime/PortFrame.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/Utils/DasJsonCore.h>

#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

using IDasPortMap = Das::ExportInterface::IDasPortMap;

// ---------------------------------------------------------------------------
// ValidateFingerprint
// ---------------------------------------------------------------------------
DasResult GraphRuntime::ValidateFingerprint(
    const std::string& compiled_source_fingerprint,
    const std::string& current_fingerprint)
{
    if (compiled_source_fingerprint.empty())
    {
        DAS_CORE_LOG_WARN(
            "Compiled plan has no source_fingerprint — skipping validation");
        return DAS_S_OK;
    }

    if (compiled_source_fingerprint != current_fingerprint)
    {
        DAS_CORE_LOG_ERROR(
            "Fingerprint mismatch: compiled = {}, current = {}",
            compiled_source_fingerprint,
            current_fingerprint);
        return DAS_E_FAIL;
    }

    DAS_CORE_LOG_TRACE(
        "Fingerprint validated: {} = {}",
        compiled_source_fingerprint,
        current_fingerprint);
    return DAS_S_OK;
}

// ---------------------------------------------------------------------------
// CheckStopToken
// ---------------------------------------------------------------------------
DasResult GraphRuntime::CheckStopToken(
    Das::PluginInterface::IDasStopToken* p_stop_token)
{
    if (!p_stop_token)
        return DAS_S_OK;

    bool      stop_requested = false;
    DasResult hr = p_stop_token->StopRequested(&stop_requested);
    if (DAS_S_OK != hr)
    {
        DAS_CORE_LOG_ERROR(
            "StopToken::StopRequested failed: hr = {}",
            static_cast<int>(hr));
        return hr;
    }

    if (stop_requested)
    {
        DAS_CORE_LOG_INFO("Execution cancelled by stop token");
        return DAS_E_FAIL;
    }

    return DAS_S_OK;
}

// ===========================================================================
// SetError / GetStructuredError
// ===========================================================================

void GraphRuntime::SetError(DasResult error_code, const std::string& message)
{
    last_error_ = message;
    DAS_CORE_LOG_ERROR("{}", message);

    if (p_error_lens_ && DAS::IsFailed(error_code))
    {
        std::string lens_msg;
        if (DAS_S_OK == GetStructuredError(error_code, lens_msg))
        {
            DAS_CORE_LOG_ERROR("ErrorLens detail: {}", lens_msg);
        }
    }
}

void GraphRuntime::SetErrorLens(Das::PluginInterface::IDasErrorLens* p_lens)
{
    p_error_lens_ = p_lens;
}

DasResult GraphRuntime::GetStructuredError(
    DasResult    error_code,
    std::string& out_message) const
{
    if (!p_error_lens_)
    {
        return DAS_E_NOT_FOUND;
    }

    DasReadOnlyString               locale{"en"};
    DAS::DasPtr<IDasReadOnlyString> p_msg;
    DasResult                       hr =
        p_error_lens_->GetErrorMessage(locale.Get(), error_code, p_msg.Put());
    if (DAS_S_OK != hr || !p_msg.Get())
    {
        return DAS_E_NOT_FOUND;
    }

    const char* utf8 = nullptr;
    p_msg->GetUtf8(&utf8);
    out_message = utf8 ? std::string{utf8} : std::string{};
    return DAS_S_OK;
}

// ===========================================================================
// ResetNodeComponents
// ===========================================================================

void GraphRuntime::ResetNodeComponents()
{
    node_components_.clear();
    configured_ = false;
}

// ===========================================================================
// ApplyNodeSettings — v17 data-sep: bind settings/payload pre-Do
// ===========================================================================

DasResult GraphRuntime::ApplyNodeSettings(
    const Dto::CompiledNodeSnapshotDto&      snapshot,
    Das::PluginInterface::IDasTaskComponent* p_component)
{
    if (!p_component)
    {
        return DAS_E_INVALID_POINTER;
    }

    // Build a combined settings JSON from compiled_settings and
    // compiled_payload_json.  Both are optional (may be null yyjson values).
    auto root = Das::Utils::MakeYyjsonObject();
    auto obj = *root.as_object();

    bool has_data = false;

    // Serialize compiled_settings if present
    if (!snapshot.compiled_settings.is_null())
    {
        auto cloned = Das::Utils::CloneYyjsonValue(snapshot.compiled_settings);
        obj[std::string_view{"settings"}] = std::move(cloned);
        has_data = true;
    }

    // Serialize compiled_payload_json if present
    if (!snapshot.compiled_payload_json.is_null())
    {
        auto cloned =
            Das::Utils::CloneYyjsonValue(snapshot.compiled_payload_json);
        obj[std::string_view{"payload"}] = std::move(cloned);
        has_data = true;
    }

    if (!has_data)
    {
        // Nothing to apply — mark as applied anyway.
        return DAS_S_OK;
    }

    auto serialized = Das::Utils::SerializeYyjsonValue(root);
    if (!serialized.has_value())
    {
        DAS_CORE_LOG_ERROR(
            "Failed to serialize settings for node = {}",
            snapshot.node_id);
        return DAS_E_FAIL;
    }

    // Create IDasJson from serialized string.
    // IDasJsonImpl starts with refcount 0.  DasPtr(T*) calls AddRef()
    // on construction (DasPtr.hpp:61), bringing the refcount to 1.
    // The DasPtr destructor will Release when it goes out of scope.
    auto p_settings_json = DAS::DasPtr<Das::ExportInterface::IDasJson>(
        new Das::Core::Utils::IDasJsonImpl(serialized->c_str()));

    DAS::DasPtr<Das::ExportInterface::IDasJson> p_result_json;
    DasResult hr = p_component->ApplySettingsChange(
        p_settings_json.Get(),
        p_result_json.Put());
    if (DAS_S_OK != hr)
    {
        DAS_CORE_LOG_ERROR(
            "ApplySettingsChange failed for node = {}: hr = {}",
            snapshot.node_id,
            static_cast<int>(hr));
        return hr;
    }

    return DAS_S_OK;
}

// ===========================================================================
// Configure
// ===========================================================================

DasResult GraphRuntime::Configure(
    const Dto::CompiledGraphPlanDto&             plan,
    Das::PluginInterface::IDasTaskComponentHost* p_host)
{
    last_error_.clear();
    ResetNodeComponents();

    if (!p_host)
    {
        SetError(DAS_E_INVALID_POINTER, "Configure: p_host is null");
        return DAS_E_INVALID_POINTER;
    }

    for (const auto& snapshot : plan.node_snapshots)
    {
        // Parse component_guid string → DasGuid
        DasGuid guid_result;
        try
        {
            guid_result = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                snapshot.component_guid);
        }
        catch (const Das::Core::Exceptions::InvalidGuidStringSizeException&)
        {
            auto msg = DAS_FMT_NS::format(
                "Configure: invalid GUID format '{}' for node = {}",
                snapshot.component_guid,
                snapshot.node_id);
            SetError(DAS_E_INVALID_ARGUMENT, msg);
            return DAS_E_INVALID_ARGUMENT;
        }
        catch (const Das::Core::Exceptions::InvalidGuidStringException&)
        {
            auto msg = DAS_FMT_NS::format(
                "Configure: invalid GUID string '{}' for node = {}",
                snapshot.component_guid,
                snapshot.node_id);
            SetError(DAS_E_INVALID_ARGUMENT, msg);
            return DAS_E_INVALID_ARGUMENT;
        }

        // Create the task component via host
        DAS::DasPtr<Das::PluginInterface::IDasTaskComponent> component;
        DasResult                                            hr =
            p_host->CreateTaskComponent(guid_result, component.Put());
        if (DAS_S_OK != hr)
        {
            auto msg = DAS_FMT_NS::format(
                "CreateTaskComponent failed for node = {}, guid = {}: hr = {}",
                snapshot.node_id,
                snapshot.component_guid,
                static_cast<int>(hr));
            SetError(hr, msg);
            return hr;
        }

        // v17 data-sep: apply settings/payload pre-Do
        hr = ApplyNodeSettings(snapshot, component.Get());
        if (DAS_S_OK != hr)
        {
            auto msg = DAS_FMT_NS::format(
                "ApplyNodeSettings failed for node = {}",
                snapshot.node_id);
            SetError(hr, msg);
            return hr;
        }

        // Store in per-node cache
        NodeComponentEntry entry;
        entry.node_id = snapshot.node_id;
        entry.component_guid = snapshot.component_guid;
        entry.component = std::move(component);
        entry.settings_applied = true;
        node_components_[snapshot.node_id] = std::move(entry);

        DAS_CORE_LOG_TRACE(
            "Configured node = {}, component_guid = {}",
            snapshot.node_id,
            snapshot.component_guid);
    }

    DAS_CORE_LOG_INFO(
        "Configure complete: {} nodes configured",
        node_components_.size());

    configured_ = true;
    return DAS_S_OK;
}

// ===========================================================================
// Prepare
// ===========================================================================

DasResult GraphRuntime::Prepare() const
{
    if (!configured_)
    {
        DAS_CORE_LOG_WARN("Prepare called before Configure");
        return DAS_E_FAIL;
    }

    if (node_components_.empty())
    {
        DAS_CORE_LOG_INFO("Prepare: no nodes to configure (empty graph)");
        return DAS_S_OK;
    }

    for (const auto& [node_id, entry] : node_components_)
    {
        if (!entry.component.Get())
        {
            auto msg = DAS_FMT_NS::format(
                "Prepare: node = {} has null component",
                node_id);
            DAS_CORE_LOG_ERROR("{}", msg);
            return DAS_E_FAIL;
        }

        if (!entry.settings_applied)
        {
            auto msg = DAS_FMT_NS::format(
                "Prepare: node = {} has pending settings",
                node_id);
            DAS_CORE_LOG_ERROR("{}", msg);
            return DAS_E_FAIL;
        }
    }

    DAS_CORE_LOG_INFO(
        "Prepare complete: {} nodes validated",
        node_components_.size());

    return DAS_S_OK;
}

// ===========================================================================
// ExecuteNodeWithComponent — PortMap ↔ JSON bridge for IDasTaskComponent::Do
// ===========================================================================

DasResult GraphRuntime::ExecuteNodeWithComponent(
    const std::string&                   node_id,
    Das::PluginInterface::IDasStopToken* p_stop_token,
    PortFrame&                           frame,
    const Dto::CompiledGraphPlanDto&     plan)
{
    // 1. Look up pre-configured component
    auto it = node_components_.find(node_id);
    if (it == node_components_.end())
    {
        DAS_CORE_LOG_ERROR("No configured component for node = {}", node_id);
        return DAS_E_NOT_FOUND;
    }

    auto* p_component = it->second.component.Get();
    if (!p_component)
    {
        DAS_CORE_LOG_ERROR("Null component for node = {}", node_id);
        return DAS_E_FAIL;
    }

    // 2. Collect input bindings for this target node
    std::vector<Dto::PortBindingDto> input_bindings;
    for (const auto& b : plan.binding_plan.bindings)
    {
        if (b.target_node_id == node_id)
            input_bindings.push_back(b);
    }

    // 3. Build input PortMap from upstream PortFrame data
    DAS::DasPtr<IDasPortMap> input_portmap;

    if (!input_bindings.empty())
    {
        DasResult hr =
            BuildInputPortMap(frame, input_bindings, input_portmap.Put());
        if (DAS_S_OK != hr)
        {
            DAS_CORE_LOG_ERROR(
                "BuildInputPortMap failed for node = {}: hr = {}",
                node_id,
                static_cast<int>(hr));
            return hr;
        }
    }
    else
    {
        DasResult hr = CreateIDasPortMap(input_portmap.Put());
        if (DAS_S_OK != hr)
            return hr;
    }

    // 4. Convert input PortMap → JSON string → IDasJson
    std::string input_json_str;
    DasResult   hr = ConvertPortMapToJson(input_portmap.Get(), input_json_str);
    if (DAS_S_OK != hr)
    {
        DAS_CORE_LOG_ERROR(
            "ConvertPortMapToJson failed for node = {}: hr = {}",
            node_id,
            static_cast<int>(hr));
        return hr;
    }

    auto p_input_json = DAS::DasPtr<Das::ExportInterface::IDasJson>(
        new Das::Core::Utils::IDasJsonImpl(input_json_str.c_str()));

    // 5. Create empty environment and settings JSON
    //    v17 data-sep: settings already applied in Configure via
    //    ApplySettingsChange, so p_settings_json is empty here.
    auto        env_json = Das::Utils::MakeYyjsonObject();
    auto        env_serialized = Das::Utils::SerializeYyjsonValue(env_json);
    std::string env_str = env_serialized.value_or("{}");

    auto p_env_json = DAS::DasPtr<Das::ExportInterface::IDasJson>(
        new Das::Core::Utils::IDasJsonImpl(env_str.c_str()));

    auto settings_json = Das::Utils::MakeYyjsonObject();
    auto settings_serialized = Das::Utils::SerializeYyjsonValue(settings_json);
    std::string settings_str = settings_serialized.value_or("{}");

    auto p_settings_json = DAS::DasPtr<Das::ExportInterface::IDasJson>(
        new Das::Core::Utils::IDasJsonImpl(settings_str.c_str()));

    // 6. Call IDasTaskComponent::Do()
    DAS::DasPtr<Das::ExportInterface::IDasJson> p_result_json;
    hr = p_component->Do(
        p_stop_token,
        p_env_json.Get(),
        p_settings_json.Get(),
        p_input_json.Get(),
        p_result_json.Put());

    if (DAS_S_OK != hr)
    {
        DAS_CORE_LOG_ERROR(
            "IDasTaskComponent::Do() failed for node = {}: hr = {}",
            node_id,
            static_cast<int>(hr));
        return hr;
    }

    // 7. Convert result JSON → PortMap → PortFrame
    if (p_result_json.Get())
    {
        // Serialize result IDasJson → string
        DAS::DasPtr<IDasReadOnlyString> p_result_str;
        hr = p_result_json->ToString(0, p_result_str.Put());
        if (DAS_S_OK != hr || !p_result_str.Get())
        {
            DAS_CORE_LOG_WARN(
                "Failed to serialize result JSON for node = {}",
                node_id);
            return DAS_S_OK; // Non-fatal: empty output
        }

        const char* utf8 = nullptr;
        p_result_str->GetUtf8(&utf8);
        std::string result_str(utf8 ? utf8 : "");

        if (!result_str.empty() && result_str != "{}")
        {
            // Convert JSON → PortMap
            DAS::DasPtr<IDasPortMap> output_portmap;
            hr = CreateIDasPortMap(output_portmap.Put());
            if (DAS_S_OK != hr)
                return hr;

            std::string error_msg;
            hr = ConvertJsonToPortMap(
                result_str,
                output_portmap.Get(),
                &error_msg);
            if (DAS_S_OK != hr)
            {
                DAS_CORE_LOG_WARN(
                    "ConvertJsonToPortMap failed for node = {}: {}",
                    node_id,
                    error_msg);
                return DAS_S_OK; // Non-fatal
            }

            // Extract PortMap → PortFrame
            auto guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(node_id);
            hr = ExtractOutputPortMap(output_portmap.Get(), guid, frame);
            if (DAS_S_OK != hr)
            {
                DAS_CORE_LOG_ERROR(
                    "ExtractOutputPortMap failed for node = {}: hr = {}",
                    node_id,
                    static_cast<int>(hr));
                return hr;
            }
        }
    }

    return DAS_S_OK;
}

// ===========================================================================
// RunWithHost
// ===========================================================================

DasResult GraphRuntime::RunWithHost(
    const Dto::CompiledGraphPlanDto&             plan,
    const std::string&                           current_fingerprint,
    Das::PluginInterface::IDasStopToken*         p_stop_token,
    Das::PluginInterface::IDasTaskComponentHost* p_host)
{
    last_error_.clear();

    // Phase 1: Fingerprint validation
    DasResult hr =
        ValidateFingerprint(plan.source_fingerprint, current_fingerprint);
    if (DAS_S_OK != hr)
    {
        auto msg = DAS_FMT_NS::format(
            "Stale compiled artifact: source_fingerprint '{}' != current '{}'. "
            "Recompile the graph before execution.",
            plan.source_fingerprint,
            current_fingerprint);
        SetError(hr, msg);
        return hr;
    }

    // Phase 2: Configure — create components, apply settings (v17 data-sep)
    hr = Configure(plan, p_host);
    if (DAS_S_OK != hr)
    {
        return hr;
    }

    // Phase 3: Prepare — validate all components ready
    hr = Prepare();
    if (DAS_S_OK != hr)
    {
        return hr;
    }

    // Phase 4: Build RuntimeExecutionCache — reserved for future slot-optimized
    // execution.  TODO: Wire cache into ExecuteNodeWithComponent when
    // slot-based data transfer is implemented.

    // Phase 5: Create fresh PortFrame
    PortFrame frame;

    // Phase 6: Per-node sequential execution using pre-configured components
    for (const auto& node_id : plan.execution_order)
    {
        DAS_CORE_LOG_INFO("Running node = {}", node_id);

        hr = CheckStopToken(p_stop_token);
        if (DAS_S_OK != hr)
        {
            SetError(
                hr,
                DAS_FMT_NS::format(
                    "Execution cancelled before node = {}",
                    node_id));
            return hr;
        }

        hr = ExecuteNodeWithComponent(node_id, p_stop_token, frame, plan);
        if (DAS_S_OK != hr)
        {
            SetError(
                hr,
                DAS_FMT_NS::format(
                    "Node '{}' execution failed: hr = {}",
                    node_id,
                    static_cast<int>(hr)));
            return hr;
        }

        DAS_CORE_LOG_TRACE("Node = {} completed successfully", node_id);
    }

    // Phase 7: Final stop token check
    hr = CheckStopToken(p_stop_token);
    if (DAS_S_OK != hr)
    {
        SetError(hr, "Execution cancelled after completing all nodes");
        return hr;
    }

    DAS_CORE_LOG_INFO(
        "RunWithHost complete: {} nodes processed",
        plan.execution_order.size());

    return DAS_S_OK;
}

DAS_CORE_GRAPHRUNTIME_NS_END
