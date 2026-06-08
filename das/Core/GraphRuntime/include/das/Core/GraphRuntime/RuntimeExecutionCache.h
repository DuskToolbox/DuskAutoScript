#ifndef DAS_CORE_GRAPHRUNTIME_RUNTIMEEXECUTIONCACHE_H
#define DAS_CORE_GRAPHRUNTIME_RUNTIMEEXECUTIONCACHE_H

#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

struct SlotBinding
{
    uint32_t    slot_index = 0;
    std::string port_id;
    std::string expected_type;
};

class RuntimeExecutionCache
{
public:
    RuntimeExecutionCache() = default;

    struct Route
    {
        uint32_t source_slot = 0;
        uint32_t target_slot = 0;
    };

    void BuildFrom(const Dto::CompiledGraphPlanDto& plan);

    const std::vector<SlotBinding>& GetInputSlots(
        const std::string& node_id) const;

    const std::vector<SlotBinding>& GetOutputSlots(
        const std::string& node_id) const;

    const std::vector<Route>& GetRoutes(
        const std::string& source_node_id,
        const std::string& target_node_id) const;

    void Clear();

    bool IsBuilt() const { return is_built_; }

private:
    std::unordered_map<std::string, std::vector<SlotBinding>> input_slots_;
    std::unordered_map<std::string, std::vector<SlotBinding>> output_slots_;
    std::map<std::pair<std::string, std::string>, std::vector<Route>> routes_;

    static const std::vector<SlotBinding> empty_slots_;
    static const std::vector<Route>       empty_routes_;

    bool is_built_ = false;
};

DAS_CORE_GRAPHRUNTIME_NS_END

#endif // DAS_CORE_GRAPHRUNTIME_RUNTIMEEXECUTIONCACHE_H
