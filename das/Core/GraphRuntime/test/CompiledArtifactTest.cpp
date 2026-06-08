#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>
#include <string_view>

namespace
{
    using namespace Das::Core::GraphRuntime::Dto;

    yyjson::value MakePayloadObject()
    {
        auto payload = Das::Utils::MakeYyjsonObject();
        auto obj = *payload.as_object();
        obj[std::string_view("type")] =
            std::make_pair(std::string_view("image"), yyjson::copy_string);
        return payload;
    }

    yyjson::value MakeSettingsObject()
    {
        auto payload = Das::Utils::MakeYyjsonObject();
        auto obj = *payload.as_object();
        obj[std::string_view("threshold")] = 0.5;
        obj[std::string_view("enabled")] = true;
        return payload;
    }

    yyjson::value MakeDiagnosticObject(
        const std::string& severity,
        const std::string& message)
    {
        auto payload = Das::Utils::MakeYyjsonObject();
        auto obj = *payload.as_object();
        obj[std::string_view("severity")] =
            std::make_pair(std::string_view(severity), yyjson::copy_string);
        obj[std::string_view("message")] =
            std::make_pair(std::string_view(message), yyjson::copy_string);
        return payload;
    }
} // namespace

// ---------------------------------------------------------------------------
// PortBindingDto
// ---------------------------------------------------------------------------
TEST(CompiledArtifactTest, PortBindingDtoRoundTrip)
{
    PortBindingDto binding;
    binding.source_node_id = "node_1";
    binding.source_port_id = "output_result";
    binding.target_node_id = "node_2";
    binding.target_port_id = "input_data";
    binding.expected_type = "image";

    auto serialized = yyjson::object(binding);

    EXPECT_TRUE(serialized.contains(std::string_view("sourceNodeId")));
    EXPECT_TRUE(serialized.contains(std::string_view("sourcePortId")));
    EXPECT_TRUE(serialized.contains(std::string_view("targetNodeId")));
    EXPECT_TRUE(serialized.contains(std::string_view("targetPortId")));
    EXPECT_TRUE(serialized.contains(std::string_view("expectedType")));
    EXPECT_FALSE(serialized.contains(std::string_view("source_node_id")));

    PortBindingDto round_tripped;
    SCOPED_TRACE(std::string(serialized.write()));
    ASSERT_NO_THROW(round_tripped = yyjson::cast<PortBindingDto>(serialized));
    EXPECT_EQ(round_tripped.source_node_id, "node_1");
    EXPECT_EQ(round_tripped.source_port_id, "output_result");
    EXPECT_EQ(round_tripped.target_node_id, "node_2");
    EXPECT_EQ(round_tripped.target_port_id, "input_data");
    EXPECT_EQ(round_tripped.expected_type, "image");
}

// ---------------------------------------------------------------------------
// PortBindingDto — various expected_type values
// ---------------------------------------------------------------------------
TEST(CompiledArtifactTest, PortBindingDtoVariousTypes)
{
    const char* types[] =
        {"int", "float", "string", "bool", "image", "base", "component"};

    for (const char* t : types)
    {
        PortBindingDto binding;
        binding.source_node_id = "src";
        binding.source_port_id = "out";
        binding.target_node_id = "dst";
        binding.target_port_id = "in";
        binding.expected_type = t;

        auto           serialized = yyjson::object(binding);
        PortBindingDto round_tripped;
        SCOPED_TRACE(std::string(serialized.write()));
        ASSERT_NO_THROW(
            round_tripped = yyjson::cast<PortBindingDto>(serialized));
        EXPECT_EQ(round_tripped.expected_type, t);
    }
}

// ---------------------------------------------------------------------------
// PortBindingPlanDto — empty
// ---------------------------------------------------------------------------
TEST(CompiledArtifactTest, PortBindingPlanDtoEmpty)
{
    PortBindingPlanDto plan;

    auto serialized = yyjson::object(plan);

    EXPECT_TRUE(serialized.contains(std::string_view("bindings")));

    PortBindingPlanDto round_tripped;
    SCOPED_TRACE(std::string(serialized.write()));
    ASSERT_NO_THROW(
        round_tripped = yyjson::cast<PortBindingPlanDto>(serialized));
    EXPECT_EQ(round_tripped.bindings.size(), 0u);
}

// ---------------------------------------------------------------------------
// PortBindingPlanDto — multiple bindings
// ---------------------------------------------------------------------------
TEST(CompiledArtifactTest, PortBindingPlanDtoMultipleBindings)
{
    PortBindingPlanDto plan;
    plan.bindings.push_back(
        {"node_1", "output_a", "node_2", "input_x", "string"});
    plan.bindings.push_back(
        {"node_1", "output_b", "node_3", "input_y", "image"});
    plan.bindings.push_back(
        {"node_3", "output_c", "node_4", "input_z", "float"});

    auto serialized = yyjson::object(plan);

    PortBindingPlanDto round_tripped;
    SCOPED_TRACE(std::string(serialized.write()));
    ASSERT_NO_THROW(
        round_tripped = yyjson::cast<PortBindingPlanDto>(serialized));
    ASSERT_EQ(round_tripped.bindings.size(), 3u);
    EXPECT_EQ(round_tripped.bindings[0].source_port_id, "output_a");
    EXPECT_EQ(round_tripped.bindings[0].target_port_id, "input_x");
    EXPECT_EQ(round_tripped.bindings[1].source_port_id, "output_b");
    EXPECT_EQ(round_tripped.bindings[2].expected_type, "float");
}

// ---------------------------------------------------------------------------
// CompiledNodeSnapshotDto
// ---------------------------------------------------------------------------
TEST(CompiledArtifactTest, CompiledNodeSnapshotDtoRoundTrip)
{
    CompiledNodeSnapshotDto snapshot;
    snapshot.node_id = "node_1";
    snapshot.component_guid = "{12345678-1234-1234-1234-123456789abc}";
    snapshot.compiled_payload_json = MakePayloadObject();
    snapshot.compiled_settings = MakeSettingsObject();
    snapshot.resolved_ports.push_back(
        GraphPortDefinitionDto{
            "output_result",
            "Result",
            "image",
            true,
            yyjson::value(),
            {"output", "result"}});
    snapshot.resolved_ports.push_back(
        GraphPortDefinitionDto{
            "input_data",
            "Data",
            "string",
            false,
            yyjson::value(),
            {}});

    auto serialized = yyjson::object(snapshot);

    EXPECT_TRUE(serialized.contains(std::string_view("nodeId")));
    EXPECT_TRUE(serialized.contains(std::string_view("componentGuid")));
    EXPECT_TRUE(serialized.contains(std::string_view("compiledPayloadJson")));
    EXPECT_TRUE(serialized.contains(std::string_view("compiledSettings")));
    EXPECT_TRUE(serialized.contains(std::string_view("resolvedPorts")));
    EXPECT_FALSE(serialized.contains(std::string_view("node_id")));

    CompiledNodeSnapshotDto round_tripped;
    SCOPED_TRACE(std::string(serialized.write()));
    ASSERT_NO_THROW(
        round_tripped = yyjson::cast<CompiledNodeSnapshotDto>(serialized));
    EXPECT_EQ(round_tripped.node_id, "node_1");
    EXPECT_EQ(
        round_tripped.component_guid,
        "{12345678-1234-1234-1234-123456789abc}");
    ASSERT_EQ(round_tripped.resolved_ports.size(), 2u);
    EXPECT_EQ(round_tripped.resolved_ports[0].port_id, "output_result");
    EXPECT_EQ(round_tripped.resolved_ports[1].port_id, "input_data");
}

// ---------------------------------------------------------------------------
// CompiledGraphPlanDto
// ---------------------------------------------------------------------------
TEST(CompiledArtifactTest, CompiledGraphPlanDtoRoundTrip)
{
    CompiledGraphPlanDto plan;
    plan.document_id = "doc_001";
    plan.source_revision = 3;
    plan.source_fingerprint = "sha256:aaa111";
    plan.compiled_fingerprint = "sha256:bbb222";
    plan.node_snapshots.push_back(
        CompiledNodeSnapshotDto{
            "node_1",
            "{guid-1}",
            MakePayloadObject(),
            MakeSettingsObject(),
            {GraphPortDefinitionDto{
                "port_1",
                "P1",
                "image",
                true,
                yyjson::value(),
                {}}}});
    plan.binding_plan.bindings.push_back(
        {"node_1", "out_1", "node_2", "in_1", "image"});
    plan.execution_order = {"node_1", "node_2", "node_3"};
    plan.diagnostics.push_back(
        MakeDiagnosticObject("warning", "Node node_3 has no connections"));

    auto serialized = yyjson::object(plan);

    EXPECT_TRUE(serialized.contains(std::string_view("documentId")));
    EXPECT_TRUE(serialized.contains(std::string_view("sourceRevision")));
    EXPECT_TRUE(serialized.contains(std::string_view("sourceFingerprint")));
    EXPECT_TRUE(serialized.contains(std::string_view("compiledFingerprint")));
    EXPECT_TRUE(serialized.contains(std::string_view("nodeSnapshots")));
    EXPECT_TRUE(serialized.contains(std::string_view("bindingPlan")));
    EXPECT_TRUE(serialized.contains(std::string_view("executionOrder")));
    EXPECT_TRUE(serialized.contains(std::string_view("diagnostics")));
    EXPECT_FALSE(serialized.contains(std::string_view("document_id")));

    CompiledGraphPlanDto round_tripped;
    SCOPED_TRACE(std::string(serialized.write()));
    ASSERT_NO_THROW(
        round_tripped = yyjson::cast<CompiledGraphPlanDto>(serialized));
    EXPECT_EQ(round_tripped.document_id, "doc_001");
    EXPECT_EQ(round_tripped.source_revision, 3);
    EXPECT_EQ(round_tripped.source_fingerprint, "sha256:aaa111");
    EXPECT_EQ(round_tripped.compiled_fingerprint, "sha256:bbb222");
    ASSERT_EQ(round_tripped.node_snapshots.size(), 1u);
    EXPECT_EQ(round_tripped.node_snapshots[0].node_id, "node_1");
    ASSERT_EQ(round_tripped.binding_plan.bindings.size(), 1u);
    EXPECT_EQ(round_tripped.binding_plan.bindings[0].source_port_id, "out_1");
    ASSERT_EQ(round_tripped.execution_order.size(), 3u);
    EXPECT_EQ(round_tripped.execution_order[0], "node_1");
    EXPECT_EQ(round_tripped.execution_order[2], "node_3");
    ASSERT_EQ(round_tripped.diagnostics.size(), 1u);
}

// ---------------------------------------------------------------------------
// CompiledSubgraphSnapshotDto
// ---------------------------------------------------------------------------
TEST(CompiledArtifactTest, CompiledSubgraphSnapshotDtoRoundTrip)
{
    CompiledSubgraphSnapshotDto subgraph;
    subgraph.graph_ref_id = "subgraph_1";
    subgraph.graph_ref_revision = 2;
    subgraph.graph_ref_fingerprint = "sha256:sub_fp";
    subgraph.input_mapping = {{"outer_in", "inner_in"}, {"data_in", "port_a"}};
    subgraph.output_mapping = {{"inner_out", "outer_out"}};
    subgraph.inner_snapshot = CompiledGraphPlanDto{
        "inner_doc",
        1,
        "sha256:inner_src",
        "sha256:inner_comp",
        {},
        {},
        {"inner_node_1"},
        {}};

    auto serialized = yyjson::object(subgraph);

    EXPECT_TRUE(serialized.contains(std::string_view("graphRefId")));
    EXPECT_TRUE(serialized.contains(std::string_view("graphRefRevision")));
    EXPECT_TRUE(serialized.contains(std::string_view("graphRefFingerprint")));
    EXPECT_TRUE(serialized.contains(std::string_view("inputMapping")));
    EXPECT_TRUE(serialized.contains(std::string_view("outputMapping")));
    EXPECT_TRUE(serialized.contains(std::string_view("innerSnapshot")));
    EXPECT_FALSE(serialized.contains(std::string_view("graph_ref_id")));

    CompiledSubgraphSnapshotDto round_tripped;
    SCOPED_TRACE(std::string(serialized.write()));
    ASSERT_NO_THROW(
        round_tripped = yyjson::cast<CompiledSubgraphSnapshotDto>(serialized));
    EXPECT_EQ(round_tripped.graph_ref_id, "subgraph_1");
    EXPECT_EQ(round_tripped.graph_ref_revision, 2);
    EXPECT_EQ(round_tripped.graph_ref_fingerprint, "sha256:sub_fp");
    ASSERT_EQ(round_tripped.input_mapping.size(), 2u);
    EXPECT_EQ(round_tripped.input_mapping.at("outer_in"), "inner_in");
    EXPECT_EQ(round_tripped.input_mapping.at("data_in"), "port_a");
    ASSERT_EQ(round_tripped.output_mapping.size(), 1u);
    EXPECT_EQ(round_tripped.output_mapping.at("inner_out"), "outer_out");
    EXPECT_EQ(round_tripped.inner_snapshot.document_id, "inner_doc");
    ASSERT_EQ(round_tripped.inner_snapshot.execution_order.size(), 1u);
    EXPECT_EQ(round_tripped.inner_snapshot.execution_order[0], "inner_node_1");
}
