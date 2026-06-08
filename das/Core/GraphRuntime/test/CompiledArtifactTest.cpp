#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/TaskScheduler/TaskRepositoryDtos.h>
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

// ===========================================================================
// CompiledArtifactEntryStorageTest — compiled_artifact slot on
// RepositoryEntryDto (02-16: v31/v19 compile-ahead storage)
// ===========================================================================
namespace
{
    using EntryDto = Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto;

    CompiledGraphPlanDto MakeTestCompiledGraphPlan()
    {
        CompiledGraphPlanDto plan;
        plan.document_id = "doc_test_001";
        plan.source_revision = 3;
        plan.source_fingerprint = "sha256:source_abc";
        plan.compiled_fingerprint = "abc123";

        plan.node_snapshots.push_back(CompiledNodeSnapshotDto{
            "nodeA",
            "{guid-node-a}",
            MakePayloadObject(),
            MakeSettingsObject(),
            {GraphPortDefinitionDto{
                "output1",
                "Output1",
                "image",
                true,
                yyjson::value(),
                {}}}});

        plan.node_snapshots.push_back(CompiledNodeSnapshotDto{
            "nodeB",
            "{guid-node-b}",
            MakePayloadObject(),
            MakeSettingsObject(),
            {GraphPortDefinitionDto{
                "input1",
                "Input1",
                "string",
                false,
                yyjson::value(),
                {}}}});

        plan.binding_plan.bindings.push_back(
            {"nodeA", "output1", "nodeB", "input1", "image"});

        plan.execution_order = {"nodeA", "nodeB"};

        plan.diagnostics.push_back(
            MakeDiagnosticObject("info", "Test diagnostic message"));

        return plan;
    }

    EntryDto MakeTestEntryWithCompiledArtifact()
    {
        auto plan = MakeTestCompiledGraphPlan();
        auto serialized_plan = yyjson::object(plan);

        EntryDto entry;
        entry.entry_id = 42;
        entry.display_name = "TestEntry";
        entry.compiled_artifact = std::move(serialized_plan);
        return entry;
    }
} // namespace

// ---------------------------------------------------------------------------
// 1. Compile-time check: compiled_artifact field exists
// ---------------------------------------------------------------------------
TEST(CompiledArtifactEntryStorageTest, EntryDtoHasCompiledArtifactField)
{
    EntryDto entry;
    yyjson::value test = entry.compiled_artifact;
    (void)test;

    auto serialized = yyjson::object(entry);
    EXPECT_TRUE(serialized.contains(std::string_view("compiledArtifact")));
}

// ---------------------------------------------------------------------------
// 2. Full round-trip through RepositoryEntryDto.compiled_artifact
// ---------------------------------------------------------------------------
TEST(CompiledArtifactEntryStorageTest, CompiledArtifactRoundTripThroughEntry)
{
    auto original_plan = MakeTestCompiledGraphPlan();
    auto serialized_plan = yyjson::object(original_plan);

    EntryDto entry;
    entry.entry_id = 42;
    entry.compiled_artifact = std::move(serialized_plan);

    auto serialized_entry = yyjson::object(entry);

    EntryDto roundtripped_entry;
    SCOPED_TRACE(std::string(serialized_entry.write()));
    ASSERT_NO_THROW(
        roundtripped_entry =
            yyjson::cast<EntryDto>(serialized_entry));

    EXPECT_EQ(roundtripped_entry.entry_id, 42);

    CompiledGraphPlanDto roundtripped_plan;
    ASSERT_NO_THROW(
        roundtripped_plan =
            yyjson::cast<CompiledGraphPlanDto>(
                roundtripped_entry.compiled_artifact));

    EXPECT_EQ(roundtripped_plan.document_id, "doc_test_001");
    ASSERT_EQ(roundtripped_plan.node_snapshots.size(), 2u);
    EXPECT_EQ(roundtripped_plan.node_snapshots[0].node_id, "nodeA");
    EXPECT_EQ(roundtripped_plan.node_snapshots[1].node_id, "nodeB");
    ASSERT_EQ(roundtripped_plan.binding_plan.bindings.size(), 1u);
    ASSERT_EQ(roundtripped_plan.execution_order.size(), 2u);
    EXPECT_EQ(roundtripped_plan.execution_order[0], "nodeA");
    EXPECT_EQ(roundtripped_plan.execution_order[1], "nodeB");
    EXPECT_EQ(roundtripped_plan.compiled_fingerprint, "abc123");
    ASSERT_GE(roundtripped_plan.diagnostics.size(), 1u);
}

// ---------------------------------------------------------------------------
// 3. PortBindingDto fields survive round-trip
// ---------------------------------------------------------------------------
TEST(CompiledArtifactEntryStorageTest, CompiledArtifactPreservesPortBindingPlan)
{
    auto original_plan = MakeTestCompiledGraphPlan();
    auto entry = MakeTestEntryWithCompiledArtifact();

    auto serialized_entry = yyjson::object(entry);
    auto roundtripped_entry =
        yyjson::cast<EntryDto>(serialized_entry);
    auto roundtripped_plan =
        yyjson::cast<CompiledGraphPlanDto>(
            roundtripped_entry.compiled_artifact);

    ASSERT_EQ(roundtripped_plan.binding_plan.bindings.size(), 1u);

    const auto& binding = roundtripped_plan.binding_plan.bindings[0];
    SCOPED_TRACE(std::string(serialized_entry.write()));
    EXPECT_EQ(binding.source_node_id, "nodeA");
    EXPECT_EQ(binding.source_port_id, "output1");
    EXPECT_EQ(binding.target_node_id, "nodeB");
    EXPECT_EQ(binding.target_port_id, "input1");
    EXPECT_EQ(binding.expected_type, "image");
}

// ---------------------------------------------------------------------------
// 4. execution_order preserves exact sequence
// ---------------------------------------------------------------------------
TEST(CompiledArtifactEntryStorageTest, CompiledArtifactPreservesExecutionOrder)
{
    auto entry = MakeTestEntryWithCompiledArtifact();

    auto serialized_entry = yyjson::object(entry);
    auto roundtripped_entry =
        yyjson::cast<EntryDto>(serialized_entry);
    auto roundtripped_plan =
        yyjson::cast<CompiledGraphPlanDto>(
            roundtripped_entry.compiled_artifact);

    ASSERT_EQ(roundtripped_plan.execution_order.size(), 2u);
    EXPECT_EQ(roundtripped_plan.execution_order[0], "nodeA");
    EXPECT_EQ(roundtripped_plan.execution_order[1], "nodeB");
}

// ---------------------------------------------------------------------------
// 5. compiled_fingerprint survives round-trip (v32 fingerprint validation)
// ---------------------------------------------------------------------------
TEST(CompiledArtifactEntryStorageTest, CompiledArtifactPreservesCompiledFingerprint)
{
    auto entry = MakeTestEntryWithCompiledArtifact();

    auto serialized_entry = yyjson::object(entry);
    auto roundtripped_entry =
        yyjson::cast<EntryDto>(serialized_entry);
    auto roundtripped_plan =
        yyjson::cast<CompiledGraphPlanDto>(
            roundtripped_entry.compiled_artifact);

    EXPECT_EQ(roundtripped_plan.compiled_fingerprint, "abc123");
    EXPECT_EQ(roundtripped_plan.source_fingerprint, "sha256:source_abc");
}

// ---------------------------------------------------------------------------
// 6. compiled_artifact and graph_document coexist independently
// ---------------------------------------------------------------------------
TEST(CompiledArtifactEntryStorageTest, CompiledArtifactIndependentFromGraphDocument)
{
    auto plan = MakeTestCompiledGraphPlan();
    auto serialized_plan = yyjson::object(plan);

    auto graph_doc = Das::Utils::MakeYyjsonObject();
    auto graph_obj = *graph_doc.as_object();
    graph_obj[std::string_view("nodes")] = yyjson::array();
    graph_obj[std::string_view("edges")] = yyjson::array();

    EntryDto entry;
    entry.entry_id = 99;
    entry.graph_document = std::move(graph_doc);
    entry.compiled_artifact = std::move(serialized_plan);

    auto serialized_entry = yyjson::object(entry);

    EntryDto roundtripped;
    SCOPED_TRACE(std::string(serialized_entry.write()));
    ASSERT_NO_THROW(roundtripped = yyjson::cast<EntryDto>(serialized_entry));

    EXPECT_TRUE(roundtripped.graph_document.as_object().has_value());
    EXPECT_TRUE(
        roundtripped.graph_document.as_object()->contains(
            std::string_view("nodes")));
    EXPECT_TRUE(
        roundtripped.graph_document.as_object()->contains(
            std::string_view("edges")));

    auto roundtripped_plan =
        yyjson::cast<CompiledGraphPlanDto>(roundtripped.compiled_artifact);
    EXPECT_EQ(roundtripped_plan.document_id, "doc_test_001");
    ASSERT_EQ(roundtripped_plan.node_snapshots.size(), 2u);
}

// ---------------------------------------------------------------------------
// 7. compiled_artifact optional / null by default
// ---------------------------------------------------------------------------
TEST(CompiledArtifactEntryStorageTest, CompiledArtifactOptionalDefault)
{
    EntryDto entry;
    entry.entry_id = 7;
    // compiled_artifact left as default (null)

    auto serialized = yyjson::object(entry);

    EntryDto roundtripped;
    SCOPED_TRACE(std::string(serialized.write()));
    ASSERT_NO_THROW(roundtripped = yyjson::cast<EntryDto>(serialized));

    auto obj = serialized.as_object();
    if (obj.has_value() && obj->contains(std::string_view("compiledArtifact")))
    {
        auto val = (*obj)[std::string_view("compiledArtifact")];
        EXPECT_TRUE(val.is_null());
    }

    auto plan_opt = roundtripped.compiled_artifact.as_object();
    EXPECT_FALSE(plan_opt.has_value());
}

// ---------------------------------------------------------------------------
// 8. JSON key is camelCase "compiledArtifact"
// ---------------------------------------------------------------------------
TEST(CompiledArtifactEntryStorageTest, CompiledArtifactSerializesAsCamelCase)
{
    auto entry = MakeTestEntryWithCompiledArtifact();

    auto serialized = yyjson::object(entry);

    EXPECT_TRUE(serialized.contains(std::string_view("compiledArtifact")));
    EXPECT_FALSE(serialized.contains(std::string_view("compiled_artifact")));
}
