#include <das/Core/GraphRuntime/GraphRuntime.h>

#include <das/Core/Exceptions/InvalidGuidStringException.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/DoAdapter.h>
#include <das/Core/GraphRuntime/GraphCompiler.h>
#include <das/Core/GraphRuntime/PortFrame.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/TaskComponentPorts.h>

#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasSettingsChangeBuilder.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/header/IDasSettingsChangeBuilder.generated.h>

#include <algorithm>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

using IDasPortMap = Das::ExportInterface::IDasPortMap;

namespace
{
    // 把编译期 yyjson 值桥接为 IDasJson 实参：紧凑序列化后解析，与既有
    // IDasJsonImpl 字符串构造同形的数据通路，无额外别名。
    std::optional<DAS::DasPtr<Das::ExportInterface::IDasJson>>
    MakeDasJsonFromYyjsonValue(const yyjson::value& source)
    {
        const auto serialized = Das::Utils::SerializeYyjsonValue(source);
        if (!serialized.has_value())
        {
            return std::nullopt;
        }
        DAS::DasPtr<Das::ExportInterface::IDasJson> result;
        if (DAS::IsFailed(
                ParseDasJsonFromString(serialized->c_str(), result.Put())))
        {
            return std::nullopt;
        }
        return result;
    }
} // namespace

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
    {
        return DAS_S_OK;
    }

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

    const DasReadOnlyString         locale{"en"};
    DAS::DasPtr<IDasReadOnlyString> p_msg;
    const DasResult                 hr =
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

    // Nothing to apply — mark as applied anyway. Both inputs are optional
    // (may be null yyjson values).
    if (snapshot.compiled_settings.is_null()
        && snapshot.compiled_payload_json.is_null())
    {
        return DAS_S_OK;
    }

    // Assemble the {"settings": ..., "payload": ...} settings-change wire via
    // the protocol builder: the fixed wire field names live behind the builder
    // methods, and each compiled yyjson value is bridged into an IDasJson
    // argument by serializing and parsing it back.
    DAS::DasPtr<Das::ExportInterface::IDasSettingsChangeBuilder> builder;
    DasResult hr = CreateIDasSettingsChangeBuilder(builder.Put());
    if (DAS_S_OK != hr)
    {
        DAS_CORE_LOG_ERROR(
            "CreateIDasSettingsChangeBuilder failed: hr = {}",
            static_cast<int>(hr));
        return hr;
    }

    if (!snapshot.compiled_settings.is_null())
    {
        const auto settings_json =
            MakeDasJsonFromYyjsonValue(snapshot.compiled_settings);
        if (!settings_json.has_value())
        {
            DAS_CORE_LOG_ERROR(
                "Failed to bridge compiled settings for node = {}",
                snapshot.node_id);
            return DAS_E_FAIL;
        }
        hr = builder->SetSettings(settings_json->Get());
        if (DAS_S_OK != hr)
        {
            DAS_CORE_LOG_ERROR(
                "SetSettings failed for node = {}: hr = {}",
                snapshot.node_id,
                static_cast<int>(hr));
            return hr;
        }
    }

    if (!snapshot.compiled_payload_json.is_null())
    {
        const auto payload_json =
            MakeDasJsonFromYyjsonValue(snapshot.compiled_payload_json);
        if (!payload_json.has_value())
        {
            DAS_CORE_LOG_ERROR(
                "Failed to bridge compiled payload for node = {}",
                snapshot.node_id);
            return DAS_E_FAIL;
        }
        hr = builder->SetPayload(payload_json->Get());
        if (DAS_S_OK != hr)
        {
            DAS_CORE_LOG_ERROR(
                "SetPayload failed for node = {}: hr = {}",
                snapshot.node_id,
                static_cast<int>(hr));
            return hr;
        }
    }

    DAS::DasPtr<Das::ExportInterface::IDasJson> p_settings_json;
    hr = builder->ToJson(p_settings_json.Put());
    if (DAS_S_OK != hr)
    {
        DAS_CORE_LOG_ERROR(
            "ToJson failed for node = {}: hr = {}",
            snapshot.node_id,
            static_cast<int>(hr));
        return hr;
    }

    DAS::DasPtr<Das::ExportInterface::IDasJson> p_result_json;
    hr = p_component->ApplySettingsChange(
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
    const Dto::CompiledGraphPlanDto&                plan,
    Das::PluginInterface::IDasTaskComponentCreator* p_host)
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
// ExecuteNodeWithComponent — direct PortMap call to IDasTaskComponent::Do
// ===========================================================================

GraphRuntime::NodeExecutionOutcome GraphRuntime::ExecuteNodeWithComponent(
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
        return {DAS_E_NOT_FOUND, {}};
    }

    auto* p_component = it->second.component.Get();
    if (!p_component)
    {
        DAS_CORE_LOG_ERROR("Null component for node = {}", node_id);
        return {DAS_E_FAIL, {}};
    }

    // 2. Collect input bindings for this target node
    std::vector<Dto::PortBindingDto> input_bindings;
    for (const auto& b : plan.binding_plan.bindings)
    {
        if (b.target_node_id == node_id)
        {
            input_bindings.push_back(b);
        }
    }

    // 3. Build input PortMap from upstream PortFrame data
    DAS::DasPtr<IDasPortMap> input_portmap;

    if (!input_bindings.empty())
    {
        DasResult hr = BuildInputPortMap(
            frame,
            input_bindings,
            input_portmap.Put(),
            graph_inputs_);
        if (DAS_S_OK != hr)
        {
            DAS_CORE_LOG_ERROR(
                "BuildInputPortMap failed for node = {}: hr = {}",
                node_id,
                static_cast<int>(hr));
            return {hr, {}};
        }
    }
    else
    {
        const DasResult hr = CreateIDasPortMap(input_portmap.Put());
        if (DAS_S_OK != hr)
        {
            return {hr, {}};
        }
    }

    // 4. Call Do() directly with PortMap — no JSON conversion
    DAS::DasPtr<IDasPortMap> output_portmap;
    DasResult                hr = DAS_E_FAIL;
    try
    {
        hr = p_component->Do(
            p_stop_token,
            input_portmap.Get(),
            output_portmap.Put());
    }
    catch (const std::exception& exception)
    {
        // 组件 Do 是插件边界：插件抛出的异常必须就地转错误码，不允许
        // 穿透运行时击穿上层调度器状态机。
        DAS_CORE_LOG_ERROR(
            "IDasTaskComponent::Do() threw exception for node = {}: {}",
            node_id,
            exception.what());
        return {DAS_E_INTERNAL_FATAL_ERROR, {}};
    }
    catch (...)
    {
        DAS_CORE_LOG_ERROR(
            "IDasTaskComponent::Do() threw unknown exception for node = {}",
            node_id);
        return {DAS_E_INTERNAL_FATAL_ERROR, {}};
    }

    if (DAS_S_OK != hr)
    {
        DAS_CORE_LOG_ERROR(
            "IDasTaskComponent::Do() failed for node = {}: hr = {}",
            node_id,
            static_cast<int>(hr));
        // 失败组件可把人可读诊断写进 diagnostic 端口（JSON {code,
        // message}）；此处尽力提取进调用方失败消息——组件产出诊断但
        // 运行时丢弃会让上层只看到裸 hr。任何一步失败静默跳过（诊断
        // 是尽力而为，不影响错误码返回）。
        std::string failure_detail;
        if (output_portmap)
        {
            try
            {
                if (const auto diagnostic = Das::Utils::GetPortJson(
                        output_portmap.Get(),
                        std::string_view("diagnostic")))
                {
                    if (const auto diagnostic_object = diagnostic->as_object())
                    {
                        auto message =
                            (*diagnostic_object)[std::string_view("message")]
                                .as_string();
                        if (!message)
                        {
                            message =
                                (*diagnostic_object)[std::string_view("code")]
                                    .as_string();
                        }
                        if (message)
                        {
                            failure_detail = std::string(*message);
                        }
                    }
                }
            }
            catch (...)
            {
                // cpp_yyjson 缺键/类型不符会抛——诊断提取失败即无诊断。
            }
        }
        return {hr, std::move(failure_detail)};
    }

    // 5. Extract output PortMap → PortFrame
    if (output_portmap)
    {
        hr = ExtractOutputPortMap(output_portmap.Get(), node_id, frame);
        if (DAS_S_OK != hr)
        {
            DAS_CORE_LOG_ERROR(
                "ExtractOutputPortMap failed for node = {}: hr = {}",
                node_id,
                static_cast<int>(hr));
            return {hr, {}};
        }
    }

    return {};
}

// ===========================================================================
// RunSignalGated — signal-driven ready queue (DAS-60 Stage 3)
// ===========================================================================
//
// Replaces the linear `for (node : execution_order)` walk with a scheduler
// that activates a node only when:
//   1. every data predecessor has resolved (Done or Skipped), AND
//   2. its signal gate is open — i.e. it has no incoming signal routes, or at
//      least one such route's source port currently holds a Signal.
//
// Gate, skip, and loop semantics:
//   - gate:    a node whose gate is shut (source resolved but no signal fired)
//              is marked Skipped, so only the taken branch of an if runs.
//   - skip:    a Skipped node produces nothing, so any data successor that
//              depends on it is also Skipped (propagated along data edges).
//   - loop:    when a back-edge source (loop body terminal) fires its done
//              signal, the loop head + body are re-pended for another pass and
//              the loop SCC's stale Signal markers are cleared (iteration
//              boundary), so gates re-decide without cross-iteration residue.
//
// ExecuteNodeWithComponent (input assembly / Do / output writeback) is reused
// unchanged. No loop-limit guard: a component that never breaks loops forever
// by design (user responsibility).

namespace
{
    // Sentinel source marking a broadcast (graph-input) binding — not a real
    // node, so it creates no data dependency. Must match GraphCompiler.
    constexpr std::string_view BROADCAST_SOURCE = "$graph_input";

    // 可序列化端口值 → JSON（graphOutputs 导出通道）。Base/Component/
    // Image/Signal 不跨结果 JSON 序列化——调用方侧无从物化，跳过（TRACE）。
    std::optional<yyjson::value> PortValueToJson(const PortValue& value)
    {
        if (value.IsInt())
        {
            return yyjson::value(*value.AsInt());
        }
        if (value.IsFloat())
        {
            return yyjson::value(*value.AsFloat());
        }
        if (value.IsBool())
        {
            return yyjson::value(*value.AsBool());
        }
        if (value.IsString())
        {
            // JsonString 构造的不可变值自持字符串内容（与生产端口载荷同
            // 通道），导出对象生命周期独立于帧内源串。
            return Das::Utils::JsonString(*value.AsString());
        }
        if (value.IsJson())
        {
            return Das::Utils::CloneYyjsonValue(value.AsJson()->value);
        }
        return std::nullopt;
    }

    using EdgeKey =
        std::tuple<std::string, std::string, std::string, std::string>;

    struct EdgeKeyHash
    {
        std::size_t operator()(const EdgeKey& k) const noexcept
        {
            std::size_t seed = 0xcbf29ce484222325ULL;
            auto        mix = [&](const std::string& s)
            {
                seed ^= std::hash<std::string>{}(s) + 0x9e3779b9 + (seed << 6)
                        + (seed >> 2);
            };
            mix(std::get<0>(k));
            mix(std::get<1>(k));
            mix(std::get<2>(k));
            mix(std::get<3>(k));
            return seed;
        }
    };

    inline EdgeKey MakeEdgeKey(
        const std::string& sn,
        const std::string& sp,
        const std::string& tn,
        const std::string& tp)
    {
        return std::make_tuple(sn, sp, tn, tp);
    }
} // namespace

DasResult GraphRuntime::RunSignalGated(
    const Dto::CompiledGraphPlanDto&     plan,
    Das::PluginInterface::IDasStopToken* p_stop_token,
    PortFrame&                           frame)
{
    // -- edge classification: signal edges vs back edges (4-tuples) ----------
    std::unordered_set<EdgeKey, EdgeKeyHash> signal_edge_keys;
    std::unordered_set<EdgeKey, EdgeKeyHash> back_edge_keys;
    for (const auto& r : plan.signal_routes)
    {
        signal_edge_keys.insert(MakeEdgeKey(
            r.source_node_id,
            r.source_port_id,
            r.target_node_id,
            r.target_port_id));
    }
    for (const auto& be : plan.back_edges)
    {
        back_edge_keys.insert(MakeEdgeKey(
            be.source_node_id,
            be.source_port_id,
            be.target_node_id,
            be.target_port_id));
    }

    // -- data dependencies (point-to-point data edges only) -----------------
    // Broadcast bindings and signal edges carry no node-level data dependency.
    std::unordered_map<std::string, std::vector<std::string>> data_preds;
    std::unordered_map<std::string, std::vector<std::string>> data_succ;
    for (const auto& b : plan.binding_plan.bindings)
    {
        if (b.source_node_id == std::string(BROADCAST_SOURCE))
        {
            continue;
        }
        const auto key = MakeEdgeKey(
            b.source_node_id,
            b.source_port_id,
            b.target_node_id,
            b.target_port_id);
        // Back edges (signal AND data) carry no node-level data dependency:
        // a back edge's source resolves only after its target has run, so
        // keeping it here would deadlock the loop head waiting on the loop
        // tail. Value routing is unaffected — BuildInputPortMap walks
        // input_bindings directly and never consults data_preds.
        if (signal_edge_keys.count(key) > 0 || back_edge_keys.count(key) > 0)
        {
            continue;
        }
        data_preds[b.target_node_id].push_back(b.source_node_id);
        data_succ[b.source_node_id].push_back(b.target_node_id);
    }

    // -- gate routes (non-back-edge signal routes) + signal fan-out ---------
    std::unordered_map<std::string, std::vector<Dto::SignalRouteDto>>
        gate_routes;
    std::unordered_map<std::string, std::vector<Dto::SignalRouteDto>>
        signal_out;
    for (const auto& r : plan.signal_routes)
    {
        signal_out[r.source_node_id].push_back(r);
        const auto key = MakeEdgeKey(
            r.source_node_id,
            r.source_port_id,
            r.target_node_id,
            r.target_port_id);
        // Back-edges re-activate the loop head via the pass restart below; they
        // are NOT gates the head must wait on (the head runs first each pass).
        if (back_edge_keys.count(key) > 0)
        {
            continue;
        }
        gate_routes[r.target_node_id].push_back(r);
    }

    // -- loop structure: signal back-edge firing registry --------------------
    // Only signal back edges carry the iteration boundary; data back edges
    // move loop state and never fire (edge_kind split is explicit — do not
    // rely on the frame value's IsSignal marker as the discriminator).
    std::unordered_map<std::string, std::vector<Dto::BackEdgeDto>>
        back_edges_by_source;
    for (const auto& be : plan.back_edges)
    {
        if (be.edge_kind != "signal")
        {
            continue;
        }
        back_edges_by_source[be.source_node_id].push_back(be);
    }

    // Per-loop-head reset set: consumed verbatim from the compile-time loop
    // layers (cycle SCC ∪ signal-less producer closure). Recomputing
    // reachability here would lose the closure knowledge only the compiler
    // has, so an empty layer list alongside real back edges is a compiler
    // contract violation — logged loudly, resets disabled, never silently
    // recomputed.
    std::unordered_map<std::string, std::vector<std::string>> loop_scc;
    if (!plan.back_edges.empty() && plan.loop_layers.empty())
    {
        DAS_CORE_LOG_ERROR(
            "Compiled plan carries back edges but no loop layers (compiler "
            "contract violation); loop iteration resets are disabled.");
    }
    for (const auto& layer : plan.loop_layers)
    {
        loop_scc[layer.head_node_id] = layer.members;
    }

    // -- scheduling state ---------------------------------------------------
    enum class NState
    {
        Pending,
        Done,
        Skipped
    };
    std::unordered_map<std::string, NState> state;
    for (const auto& n : plan.execution_order)
    {
        state[n] = NState::Pending;
    }

    auto port_signaled = [&](const Dto::SignalRouteDto& r) -> bool
    {
        const auto* pv =
            frame.Find(PortKey{r.source_node_id, r.source_port_id});
        return pv != nullptr && pv->IsSignal();
    };
    auto can_decide = [&](const std::string& n) -> bool
    {
        for (const auto& p : data_preds[n])
        {
            if (state[p] == NState::Pending)
            {
                return false;
            }
        }
        for (const auto& r : gate_routes[n])
        {
            if (state[r.source_node_id] == NState::Pending)
            {
                return false;
            }
        }
        return true;
    };
    auto should_skip = [&](const std::string& n) -> bool
    {
        // Gate first. A node with incoming signal routes runs only when at
        // least one fired: if none fired it is skipped (the untaken branch of
        // an if); if one fired it is on a taken branch and must run even when a
        // non-taken-branch data predecessor was skipped — that is a φ-join such
        // as das.flow.merge, whose value_* inputs come from both branches but
        // only the taken one produced a value (DAS-74).
        const auto& gr = gate_routes[n];
        if (!gr.empty())
        {
            bool any_open = false;
            for (const auto& r : gr)
            {
                if (port_signaled(r))
                {
                    any_open = true;
                    break;
                }
            }
            return !any_open;
        }
        // No signal gate: skip-propagation — a data predecessor that was
        // skipped starves this node.
        for (const auto& p : data_preds[n])
        {
            if (state[p] == NState::Skipped)
            {
                return true;
            }
        }
        return false;
    };

    // -- multi-pass scheduling loop -----------------------------------------
    // A single pass walks execution_order once, deciding every node that is
    // ready. When a back-edge fires we restart the pass so the loop head
    // re-runs in its proper position. The loop ends when a full pass makes no
    // progress (every remaining node is waiting on something that will never
    // resolve — those stay unexecuted).
    bool progress = true;
    while (progress)
    {
        progress = false;
        for (const auto& n : plan.execution_order)
        {
            if (state[n] != NState::Pending || !can_decide(n))
            {
                continue;
            }
            progress = true;

            if (should_skip(n))
            {
                state[n] = NState::Skipped;
                continue;
            }

            DAS_CORE_LOG_INFO("Running node = {}", n);

            DasResult hr = CheckStopToken(p_stop_token);
            if (DAS_S_OK != hr)
            {
                SetError(
                    hr,
                    DAS_FMT_NS::format(
                        "Execution cancelled before node = {}",
                        n));
                return hr;
            }

            const auto outcome =
                ExecuteNodeWithComponent(n, p_stop_token, frame, plan);
            if (DAS_S_OK != outcome.hr)
            {
                SetError(
                    outcome.hr,
                    outcome.failure_detail.empty()
                        ? DAS_FMT_NS::format(
                              "Node '{}' execution failed: hr = {}",
                              n,
                              static_cast<int>(outcome.hr))
                        : DAS_FMT_NS::format(
                              "Node '{}' execution failed: hr = {} — {}",
                              n,
                              static_cast<int>(outcome.hr),
                              outcome.failure_detail));
                return outcome.hr;
            }

            // 规范口兜底补发：signal_out 的成功发射是运行时保证而非各组
            // 件自觉——编译器打标的规范口路由在组件未显式发射时补一发，
            // 下游信号门因此必然打开。组件显式发射优先（帧已有 Signal 不
            // 重复写）；status 口 "cancelled" 是 S_OK 合作取消约定
            // （BuildResultPortMap 组件协议），取消节点不代发、下游门保持
            // 关闭。补发先于回边发火检查，回边（循环尾的规范口形态）与组
            // 件自发语义一致；每圈 ClearSignalsByNode 清掉后下圈组件重跑
            // 再补发。
            {
                bool node_cancelled = false;
                if (const auto* status_value = frame.Find(PortKey{n, "status"});
                    status_value != nullptr && status_value->IsString()
                    && *status_value->AsString() == "cancelled")
                {
                    node_cancelled = true;
                }
                if (!node_cancelled)
                {
                    const auto routes_it = signal_out.find(n);
                    if (routes_it != signal_out.end())
                    {
                        for (const auto& route : routes_it->second)
                        {
                            if (!route.source_port_canonical)
                            {
                                continue;
                            }
                            const auto* emitted =
                                frame.Find(PortKey{n, route.source_port_id});
                            if (emitted != nullptr && emitted->IsSignal())
                            {
                                continue;
                            }
                            frame.Set(
                                PortKey{n, route.source_port_id},
                                PortValue::Signal());
                        }
                    }
                }
            }
            state[n] = NState::Done;
            last_executed_node_id_ = n;

            // back-edge firing? → loop iteration complete
            std::string fired_head;
            for (const auto& be : back_edges_by_source[n])
            {
                const auto* pv = frame.Find(PortKey{n, be.source_port_id});
                if (pv != nullptr && pv->IsSignal())
                {
                    fired_head = be.loop_head_node_id;
                    break;
                }
            }
            if (!fired_head.empty())
            {
                // 外层重开=内层从头来：清空严格内层数据回边源口的帧值，
                // 内层重开第一圈读不到旧值、经缺口语径回落组件缺省——
                // 这是「清信号留数据」原则的唯一精确例外，只发生在外环
                // 重启内环这一处。判据与编译期分层一致：内层头落在发火
                // 层的成员集内。
                const std::vector<std::string>& fired_members =
                    loop_scc[fired_head];
                std::unordered_set<std::string> fired_member_set(
                    fired_members.begin(),
                    fired_members.end());
                for (const auto& layer : plan.loop_layers)
                {
                    if (layer.head_node_id == fired_head
                        || fired_member_set.count(layer.head_node_id) == 0)
                    {
                        continue;
                    }
                    for (const auto& source : layer.data_back_edge_sources)
                    {
                        // 缺键是正常形态（被跳过的圈从未写过该口），布尔
                        // 返回值只报告是否真有条目被删——无需消费。
                        (void)frame.Remove(
                            PortKey{source.node_id, source.port_id});
                    }
                }
                // iteration boundary: clear stale signals, then re-pend head +
                // body
                for (const auto& m : fired_members)
                {
                    frame.ClearSignalsByNode(m);
                }
                state[fired_head] = NState::Pending;
                for (const auto& m : fired_members)
                {
                    state[m] = NState::Pending;
                }
                break; // restart pass so the head re-runs in execution order
            }
        }
    }

    DAS_CORE_LOG_INFO("RunSignalGated complete");
    return DAS_S_OK;
}

// ===========================================================================
// RunWithHost
// ===========================================================================

DasResult GraphRuntime::RunWithHost(
    const Dto::CompiledGraphPlanDto&                plan,
    const std::string&                              current_fingerprint,
    Das::PluginInterface::IDasStopToken*            p_stop_token,
    Das::PluginInterface::IDasTaskComponentCreator* p_host,
    Das::ExportInterface::IDasPortMap**             pp_out_typed_outputs)
{
    last_error_.clear();
    last_executed_node_id_.clear();
    last_graph_outputs_ = Das::Utils::MakeYyjsonObject();

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

    // 带节点却无执行序的计划是编译期否决产物（无信号回路承载的数据环会
    // 清空 execution_order）：放行会跑成「成功+零节点」，必须拒绝并把
    // fatal 诊断透出到 last_error。
    if (plan.execution_order.empty() && !plan.node_snapshots.empty())
    {
        std::string fatal_messages;
        const auto  unbacked_cycle_kind = std::to_string(
            static_cast<int>(CompileDiagnosticKind::UnbackedDataCycle));
        const auto cyclic_kind = std::to_string(
            static_cast<int>(CompileDiagnosticKind::CyclicEdgeGraph));
        for (const auto& diagnostic : plan.diagnostics)
        {
            auto diagnostic_object = diagnostic.as_object();
            if (!diagnostic_object.has_value())
            {
                continue;
            }
            const auto kind = (*diagnostic_object)[std::string_view("kind")]
                                  .as_string()
                                  .value_or("");
            if (kind != unbacked_cycle_kind && kind != cyclic_kind)
            {
                continue;
            }
            if (!fatal_messages.empty())
            {
                fatal_messages += "; ";
            }
            fatal_messages += (*diagnostic_object)[std::string_view("message")]
                                  .as_string()
                                  .value_or("");
        }
        SetError(
            DAS_E_FAIL,
            DAS_FMT_NS::format(
                "Compiled plan carries {} nodes but no executable order; "
                "compile was rejected. {}",
                plan.node_snapshots.size(),
                fatal_messages));
        return DAS_E_FAIL;
    }

    // Phase 4: Build RuntimeExecutionCache — reserved for future slot-optimized
    // execution.  TODO: Wire cache into ExecuteNodeWithComponent when
    // slot-based data transfer is implemented.

    // Phase 5: Create fresh PortFrame
    PortFrame frame;

    // Phase 6: Execute. Pure-data graphs (no signal routes, no back edges) use
    // the original linear execution_order walk — unchanged behaviour. Graphs
    // with control flow use the signal-driven ready queue (DAS-60 Stage 3).
    if (plan.signal_routes.empty() && plan.back_edges.empty())
    {
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

            const auto outcome =
                ExecuteNodeWithComponent(node_id, p_stop_token, frame, plan);
            if (DAS_S_OK != outcome.hr)
            {
                SetError(
                    outcome.hr,
                    outcome.failure_detail.empty()
                        ? DAS_FMT_NS::format(
                              "Node '{}' execution failed: hr = {}",
                              node_id,
                              static_cast<int>(outcome.hr))
                        : DAS_FMT_NS::format(
                              "Node '{}' execution failed: hr = {} — {}",
                              node_id,
                              static_cast<int>(outcome.hr),
                              outcome.failure_detail));
                return outcome.hr;
            }
            last_executed_node_id_ = node_id;

            DAS_CORE_LOG_TRACE("Node = {} completed successfully", node_id);
        }
    }
    else
    {
        hr = RunSignalGated(plan, p_stop_token, frame);
        if (DAS_S_OK != hr)
        {
            return hr;
        }
    }

    // Phase 7: Final stop token check
    hr = CheckStopToken(p_stop_token);
    if (DAS_S_OK != hr)
    {
        SetError(hr, "Execution cancelled after completing all nodes");
        return hr;
    }

    // 边界输出口导出——frame 尚在作用域内，按编译产物清单把悬空输出口
    // 的值收敛为 graphOutputs（同名多位置时后执行位置覆盖；不可序列化类
    // 型跳过）。
    {
        auto outputs_object = last_graph_outputs_.as_object();
        if (outputs_object.has_value())
        {
            for (const auto& boundary_output : plan.boundary_ports.outputs)
            {
                for (const auto& node_id : boundary_output.node_ids)
                {
                    const PortValue* port_value =
                        frame.Find(PortKey{node_id, boundary_output.port_id});
                    if (port_value == nullptr)
                    {
                        continue;
                    }
                    auto exported = PortValueToJson(*port_value);
                    if (!exported.has_value())
                    {
                        DAS_CORE_LOG_TRACE(
                            "Boundary output port = {} on node = {} carries "
                            "a non-serializable value; skipped from "
                            "graphOutputs",
                            boundary_output.port_id,
                            node_id);
                        continue;
                    }
                    (*outputs_object)[std::string_view(
                        boundary_output.port_id)] =
                        Das::Utils::CloneYyjsonValue(*exported);
                }
            }
        }
    }

    // 类型化边界导出（D-21 单通道的输出半）：调用方请求时按同一份编译
    // 产物边界清单把帧值以类型化条目直写——不可序列化对象引用（image/
    // base/component）不再跳过，这是 JSON graphOutputs 通道过不了的半段。
    // 同名多站点后写覆盖，与上方 JSON 导出的次序一致。
    if (pp_out_typed_outputs != nullptr)
    {
        DAS::DasOutPtr<Das::ExportInterface::IDasPortMap> typed_map(
            pp_out_typed_outputs);
        DasResult typed_create_result = CreateIDasPortMap(typed_map.Put());
        if (DAS_S_OK != typed_create_result)
        {
            SetError(
                typed_create_result,
                "Failed to create typed boundary output PortMap");
            return typed_create_result;
        }
        for (const auto& boundary_output : plan.boundary_ports.outputs)
        {
            for (const auto& node_id : boundary_output.node_ids)
            {
                const PortValue* port_value =
                    frame.Find(PortKey{node_id, boundary_output.port_id});
                if (port_value == nullptr)
                {
                    continue;
                }
                const DasResult write_result = PortValueToPortMap(
                    *port_value,
                    boundary_output.port_id,
                    typed_map.Get());
                if (DAS_S_OK != write_result)
                {
                    SetError(
                        write_result,
                        DAS_FMT_NS::format(
                            "Failed to export typed boundary output port = "
                            "{} on node = {}",
                            boundary_output.port_id,
                            node_id));
                    return write_result;
                }
            }
        }
        typed_map.Keep();
    }

    DAS_CORE_LOG_INFO(
        "RunWithHost complete: {} nodes processed",
        plan.execution_order.size());

    return DAS_S_OK;
}

DAS_CORE_GRAPHRUNTIME_NS_END
