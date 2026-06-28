#include <das/Core/GraphRuntime/GraphRuntime.h>

#include <das/Core/Exceptions/InvalidGuidStringException.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/DoAdapter.h>
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
#include <deque>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
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
// ExecuteNodeWithComponent — direct PortMap call to IDasTaskComponent::Do
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

    // 4. Call Do() directly with PortMap — no JSON conversion
    DAS::DasPtr<IDasPortMap> output_portmap;
    DasResult                hr = p_component->Do(
        p_stop_token,
        input_portmap.Get(),
        output_portmap.Put());

    if (DAS_S_OK != hr)
    {
        DAS_CORE_LOG_ERROR(
            "IDasTaskComponent::Do() failed for node = {}: hr = {}",
            node_id,
            static_cast<int>(hr));
        return hr;
    }

    // 5. Extract output PortMap → PortFrame
    if (output_portmap)
    {
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

    return DAS_S_OK;
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
    constexpr std::string_view kBroadcastSource = "$graph_input";

    using EdgeKey =
        std::tuple<std::string, std::string, std::string, std::string>;

    struct EdgeKeyHash
    {
        std::size_t operator()(const EdgeKey& k) const noexcept
        {
            std::size_t seed = 0xcbf29ce484222325ULL;
            auto mix = [&](const std::string& s)
            {
                seed ^=
                    std::hash<std::string>{}(s) + 0x9e3779b9 + (seed << 6)
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
            r.source_node_id, r.source_port_id, r.target_node_id, r.target_port_id));
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
        if (b.source_node_id == std::string(kBroadcastSource))
        {
            continue;
        }
        const auto key = MakeEdgeKey(
            b.source_node_id, b.source_port_id, b.target_node_id, b.target_port_id);
        if (signal_edge_keys.count(key) > 0)
        {
            continue;
        }
        data_preds[b.target_node_id].push_back(b.source_node_id);
        data_succ[b.source_node_id].push_back(b.target_node_id);
    }

    // -- gate routes (non-back-edge signal routes) + signal fan-out ---------
    std::unordered_map<std::string, std::vector<Dto::SignalRouteDto>> gate_routes;
    std::unordered_map<std::string, std::vector<Dto::SignalRouteDto>> signal_out;
    for (const auto& r : plan.signal_routes)
    {
        signal_out[r.source_node_id].push_back(r);
        const auto key = MakeEdgeKey(
            r.source_node_id, r.source_port_id, r.target_node_id, r.target_port_id);
        // Back-edges re-activate the loop head via the pass restart below; they
        // are NOT gates the head must wait on (the head runs first each pass).
        if (back_edge_keys.count(key) > 0)
        {
            continue;
        }
        gate_routes[r.target_node_id].push_back(r);
    }

    // -- loop structure: back-edge sources + loop heads ---------------------
    std::unordered_map<std::string, std::vector<Dto::BackEdgeDto>>
                                        back_edges_by_source;
    std::unordered_set<std::string>     loop_heads;
    for (const auto& be : plan.back_edges)
    {
        loop_heads.insert(be.loop_head_node_id);
        back_edges_by_source[be.source_node_id].push_back(be);
    }

    // -- unified adjacency (data + every signal route incl. back-edges) -----
    std::unordered_map<std::string, std::vector<std::string>> fwd_adj;
    std::unordered_map<std::string, std::vector<std::string>> bwd_adj;
    auto link = [&](const std::string& from, const std::string& to)
    {
        fwd_adj[from].push_back(to);
        bwd_adj[to].push_back(from);
    };
    for (const auto& [tgt, preds] : data_preds)
    {
        for (const auto& p : preds)
        {
            link(p, tgt);
        }
    }
    for (const auto& r : plan.signal_routes)
    {
        link(r.source_node_id, r.target_node_id);
    }

    // Nodes that participate in this graph (for reachability scoping).
    std::unordered_set<std::string> graph_nodes;
    for (const auto& n : plan.execution_order)
    {
        graph_nodes.insert(n);
    }

    auto reach =
        [&](const std::string& start,
            const std::unordered_map<std::string, std::vector<std::string>>& adj)
        -> std::unordered_set<std::string>
    {
        std::unordered_set<std::string> seen{start};
        std::deque<std::string>        q{start};
        while (!q.empty())
        {
            const std::string u = q.front();
            q.pop_front();
            auto it = adj.find(u);
            if (it == adj.end())
            {
                continue;
            }
            for (const auto& v : it->second)
            {
                if (graph_nodes.count(v) == 0)
                {
                    continue;
                }
                if (seen.insert(v).second)
                {
                    q.push_back(v);
                }
            }
        }
        return seen;
    };

    // Per-loop-head reset set = the SCC containing the head
    // (forward-reachable ∩ backward-reachable), whose Signal markers must be
    // cleared at each iteration boundary.
    std::unordered_map<std::string, std::vector<std::string>> loop_scc;
    for (const auto& head : loop_heads)
    {
        const auto fr = reach(head, fwd_adj);
        const auto br = reach(head, bwd_adj);
        std::vector<std::string> scc;
        for (const auto& n : fr)
        {
            if (br.count(n) > 0)
            {
                scc.push_back(n);
            }
        }
        loop_scc[head] = std::move(scc);
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

    auto guid_of = [](const std::string& id)
    {
        return Das::Core::ForeignInterfaceHost::MakeDasGuid(id);
    };
    auto port_signaled = [&](const Dto::SignalRouteDto& r) -> bool
    {
        const auto* pv = frame.Find(PortKey{guid_of(r.source_node_id), r.source_port_id});
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
        // Gate first. A node with incoming signal routes runs only when at least
        // one fired: if none fired it is skipped (the untaken branch of an if);
        // if one fired it is on a taken branch and must run even when a
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
        // No signal gate: skip-propagation — a data predecessor that was skipped
        // starves this node.
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
                    DAS_FMT_NS::format("Execution cancelled before node = {}", n));
                return hr;
            }

            hr = ExecuteNodeWithComponent(n, p_stop_token, frame, plan);
            if (DAS_S_OK != hr)
            {
                SetError(
                    hr,
                    DAS_FMT_NS::format(
                        "Node '{}' execution failed: hr = {}",
                        n,
                        static_cast<int>(hr)));
                return hr;
            }
            state[n] = NState::Done;

            // back-edge firing? → loop iteration complete
            std::string fired_head;
            for (const auto& be : back_edges_by_source[n])
            {
                const auto* pv =
                    frame.Find(PortKey{guid_of(n), be.source_port_id});
                if (pv != nullptr && pv->IsSignal())
                {
                    fired_head = be.loop_head_node_id;
                    break;
                }
            }
            if (!fired_head.empty())
            {
                // iteration boundary: clear stale signals, then re-pend head + body
                for (const auto& m : loop_scc[fired_head])
                {
                    frame.ClearSignalsByNode(guid_of(m));
                }
                state[fired_head] = NState::Pending;
                for (const auto& m : loop_scc[fired_head])
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

    DAS_CORE_LOG_INFO(
        "RunWithHost complete: {} nodes processed",
        plan.execution_order.size());

    return DAS_S_OK;
}

DAS_CORE_GRAPHRUNTIME_NS_END
