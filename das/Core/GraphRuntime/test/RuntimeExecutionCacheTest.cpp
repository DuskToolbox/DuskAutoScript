#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/GraphRuntime/RuntimeExecutionCache.h>
#include <gtest/gtest.h>

#include <algorithm>

namespace
{
    using namespace Das::Core::GraphRuntime;
    using namespace Das::Core::GraphRuntime::Dto;

    Dto::GraphPortDefinitionDto MakePortDef(
        const std::string& port_id,
        const std::string& port_type)
    {
        Dto::GraphPortDefinitionDto port;
        port.port_id = port_id;
        port.port_type = port_type;
        return port;
    }

    Dto::CompiledNodeSnapshotDto MakeNodeSnapshot(
        const std::string&                         node_id,
        const std::vector<GraphPortDefinitionDto>& inputs,
        const std::vector<GraphPortDefinitionDto>& outputs)
    {
        Dto::CompiledNodeSnapshotDto snap;
        snap.node_id = node_id;
        snap.component_guid = "test-guid";
        // Merge all ports into resolved_ports; we'll use port_id prefix
        // to distinguish inputs vs outputs for testing purposes.
        for (const auto& p : inputs)
        {
            snap.resolved_ports.push_back(p);
        }
        for (const auto& p : outputs)
        {
            snap.resolved_ports.push_back(p);
        }
        return snap;
    }

    Dto::PortBindingDto MakeBinding(
        const std::string& src_node,
        const std::string& src_port,
        const std::string& tgt_node,
        const std::string& tgt_port,
        const std::string& expected_type)
    {
        Dto::PortBindingDto binding;
        binding.source_node_id = src_node;
        binding.source_port_id = src_port;
        binding.target_node_id = tgt_node;
        binding.target_port_id = tgt_port;
        binding.expected_type = expected_type;
        return binding;
    }

} // namespace

// ===================================================================
// Test Suite 9: RuntimeExecutionCacheTest
// ===================================================================

TEST(RuntimeExecutionCacheTest, SlotDerivationFromPlan)
{
    // Build a plan with two nodes: A (outputs: out1, out2) and B (inputs:
    // in1)
    Dto::CompiledGraphPlanDto plan;

    auto snap_a = MakeNodeSnapshot(
        "A",
        {}, // no inputs
        {MakePortDef("out1", "int"), MakePortDef("out2", "string")});
    auto snap_b = MakeNodeSnapshot(
        "B",
        {MakePortDef("in1", "int")},
        {} // no outputs
    );
    plan.node_snapshots.push_back(std::move(snap_a));
    plan.node_snapshots.push_back(std::move(snap_b));

    // Both outputs of A must appear in bindings to be classified
    plan.binding_plan.bindings.push_back(
        MakeBinding("A", "out1", "B", "in1", "int"));
    plan.binding_plan.bindings.push_back(
        MakeBinding("A", "out2", "B", "in1", "string"));

    RuntimeExecutionCache cache;
    cache.BuildFrom(plan);

    EXPECT_TRUE(cache.IsBuilt());

    // Node A should have output slots (both out1 and out2)
    const auto& a_outputs = cache.GetOutputSlots("A");
    EXPECT_EQ(a_outputs.size(), 2u);

    // Node B should have input slots
    const auto& b_inputs = cache.GetInputSlots("B");
    EXPECT_EQ(b_inputs.size(), 1u);
    EXPECT_EQ(b_inputs[0].port_id, "in1");
}

TEST(RuntimeExecutionCacheTest, DeterministicSlotOrder)
{
    Dto::CompiledGraphPlanDto plan;

    // Create node with input ports in non-alphabetical order
    auto snap_a = MakeNodeSnapshot(
        "A",
        {MakePortDef("z_port", "int"),
         MakePortDef("a_port", "string"),
         MakePortDef("m_port", "bool")},
        {});
    auto snap_b = MakeNodeSnapshot(
        "B",
        {},
        {MakePortDef("out1", "int"),
         MakePortDef("out2", "string"),
         MakePortDef("out3", "bool")});
    plan.node_snapshots.push_back(std::move(snap_a));
    plan.node_snapshots.push_back(std::move(snap_b));

// All input ports of A must appear as targets in bindings
plan.binding_plan.bindings.push_back(
    MakeBinding("B", "out1", "A", "z_port", "int"));
plan.binding_plan.bindings.push_back(
    MakeBinding("B", "out2", "A", "a_port", "string"));
plan.binding_plan.bindings.push_back(
    MakeBinding("B", "out3", "A", "m_port", "bool"));

RuntimeExecutionCache cache1, cache2;
cache1.BuildFrom(plan);
cache2.BuildFrom(plan);

const auto& slots1 = cache1.GetInputSlots("A");
const auto& slots2 = cache2.GetInputSlots("A");

ASSERT_EQ(slots1.size(), slots2.size());
for (size_t i = 0; i < slots1.size(); ++i)
{
    EXPECT_EQ(slots1[i].port_id, slots2[i].port_id);
    EXPECT_EQ(slots1[i].slot_index, slots2[i].slot_index);
}

// Verify alphabetical ordering
ASSERT_EQ(slots1.size(), 3u);
EXPECT_EQ(slots1[0].port_id, "a_port");
EXPECT_EQ(slots1[1].port_id, "m_port");
EXPECT_EQ(slots1[2].port_id, "z_port");
}

TEST(RuntimeExecutionCacheTest, RoutesFromBindingPlan)
{
    Dto::CompiledGraphPlanDto plan;

    auto snap_a = MakeNodeSnapshot(
        "A",
        {},
        {MakePortDef("out1", "int"), MakePortDef("out2", "string")});
    auto snap_b = MakeNodeSnapshot(
        "B",
        {MakePortDef("in1", "int"), MakePortDef("in2", "string")},
        {});
    plan.node_snapshots.push_back(std::move(snap_a));
    plan.node_snapshots.push_back(std::move(snap_b));

    plan.binding_plan.bindings.push_back(
        MakeBinding("A", "out1", "B", "in1", "int"));
    plan.binding_plan.bindings.push_back(
        MakeBinding("A", "out2", "B", "in2", "string"));

    RuntimeExecutionCache cache;
    cache.BuildFrom(plan);

    const auto& routes = cache.GetRoutes("A", "B");
    ASSERT_EQ(routes.size(), 2u);

    // Find the route for out1->in1
    // out1 slot index should match in A's output slots
    const auto& a_outputs = cache.GetOutputSlots("A");
    const auto& b_inputs = cache.GetInputSlots("B");

    // Verify slots are correct
    uint32_t out1_slot = 0, in1_slot = 0;
    for (const auto& s : a_outputs)
    {
        if (s.port_id == "out1")
            out1_slot = s.slot_index;
    }
    for (const auto& s : b_inputs)
    {
        if (s.port_id == "in1")
            in1_slot = s.slot_index;
    }

    bool found_route = false;
    for (const auto& r : routes)
    {
        if (r.source_slot == out1_slot && r.target_slot == in1_slot)
        {
            found_route = true;
            break;
        }
    }
    EXPECT_TRUE(found_route);
}
