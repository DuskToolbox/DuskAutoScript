#include <das/Core/GraphRuntime/RuntimeExecutionCache.h>
#include <das/Core/Logger/Logger.h>

#include <algorithm>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

const std::vector<SlotBinding> RuntimeExecutionCache::empty_slots_;
const std::vector<RuntimeExecutionCache::Route>
    RuntimeExecutionCache::empty_routes_;

void RuntimeExecutionCache::BuildFrom(const Dto::CompiledGraphPlanDto& plan)
{
    Clear();

    // Build input/output slot arrays from node snapshots
    for (const auto& snapshot : plan.node_snapshots)
    {
        std::vector<SlotBinding> inputs;
        std::vector<SlotBinding> outputs;

        for (const auto& port : snapshot.resolved_ports)
        {
            SlotBinding binding;
            binding.port_id = port.port_id;
            binding.expected_type = port.port_type;

            // Classify ports based on binding plan: a port is an input
            // if it appears as a target_port in the binding plan, and an
            // output if it appears as a source_port.
            bool is_input = false;
            bool is_output = false;
            for (const auto& b : plan.binding_plan.bindings)
            {
                if (b.target_node_id == snapshot.node_id
                    && b.target_port_id == port.port_id)
                {
                    is_input = true;
                }
                if (b.source_node_id == snapshot.node_id
                    && b.source_port_id == port.port_id)
                {
                    is_output = true;
                }
            }

            if (is_input)
            {
                inputs.push_back(std::move(binding));
            }
            else if (is_output)
            {
                outputs.push_back(std::move(binding));
            }
        }

        // Sort alphabetically by port_id for deterministic ordering
        std::sort(
            inputs.begin(),
            inputs.end(),
            [](const SlotBinding& a, const SlotBinding& b)
            { return a.port_id < b.port_id; });
        std::sort(
            outputs.begin(),
            outputs.end(),
            [](const SlotBinding& a, const SlotBinding& b)
            { return a.port_id < b.port_id; });

        // Assign dense slot indices
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            inputs[i].slot_index = static_cast<uint32_t>(i);
        }
        for (size_t i = 0; i < outputs.size(); ++i)
        {
            outputs[i].slot_index = static_cast<uint32_t>(i);
        }

        input_slots_[snapshot.node_id] = std::move(inputs);
        output_slots_[snapshot.node_id] = std::move(outputs);
    }

    // Build routes from binding plan
    for (const auto& binding : plan.binding_plan.bindings)
    {
        auto src_it = output_slots_.find(binding.source_node_id);
        auto tgt_it = input_slots_.find(binding.target_node_id);
        if (src_it == output_slots_.end() || tgt_it == input_slots_.end())
        {
            continue;
        }

        auto source_slot_it = std::find_if(
            src_it->second.begin(),
            src_it->second.end(),
            [&](const SlotBinding& s)
            { return s.port_id == binding.source_port_id; });
        auto target_slot_it = std::find_if(
            tgt_it->second.begin(),
            tgt_it->second.end(),
            [&](const SlotBinding& s)
            { return s.port_id == binding.target_port_id; });

        if (source_slot_it != src_it->second.end()
            && target_slot_it != tgt_it->second.end())
        {
            routes_[{binding.source_node_id, binding.target_node_id}].push_back(
                Route{source_slot_it->slot_index, target_slot_it->slot_index});
        }
    }

    is_built_ = true;

    DAS_CORE_LOG_INFO(
        "RuntimeExecutionCache built: nodes = {}, routes = {}",
        input_slots_.size(),
        routes_.size());
}

const std::vector<SlotBinding>& RuntimeExecutionCache::GetInputSlots(
    const std::string& node_id) const
{
    auto it = input_slots_.find(node_id);
    return it != input_slots_.end() ? it->second : empty_slots_;
}

const std::vector<SlotBinding>& RuntimeExecutionCache::GetOutputSlots(
    const std::string& node_id) const
{
    auto it = output_slots_.find(node_id);
    return it != output_slots_.end() ? it->second : empty_slots_;
}

const std::vector<RuntimeExecutionCache::Route>&
RuntimeExecutionCache::GetRoutes(
    const std::string& source_node_id,
    const std::string& target_node_id) const
{
    auto it = routes_.find({source_node_id, target_node_id});
    return it != routes_.end() ? it->second : empty_routes_;
}

void RuntimeExecutionCache::Clear()
{
    input_slots_.clear();
    output_slots_.clear();
    routes_.clear();
    is_built_ = false;
}

DAS_CORE_GRAPHRUNTIME_NS_END
