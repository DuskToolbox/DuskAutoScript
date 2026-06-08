#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/GraphRuntime/GraphEntryId.h>
#include <das/Core/TaskScheduler/TaskRepositoryDtos.h>
#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>
#include <string_view>

static_assert(
    std::is_same_v<Das::Core::GraphRuntime::GraphEntryId, int64_t>,
    "GraphEntryId must be int64_t");

namespace
{
    using namespace Das::Core::GraphRuntime::Dto;

    yyjson::value MakeDefaultPayload()
    {
        auto payload = Das::Utils::MakeYyjsonObject();
        auto obj = *payload.as_object();
        obj[std::string_view("type")] =
            std::make_pair(std::string_view("image"), yyjson::copy_string);
        return payload;
    }

    yyjson::value MakeSettingsPayload()
    {
        auto payload = Das::Utils::MakeYyjsonObject();
        auto obj = *payload.as_object();
        obj[std::string_view("threshold")] = 0.5;
        obj[std::string_view("enabled")] = true;
        return payload;
    }
} // namespace

// ---------------------------------------------------------------------------
// GraphPortDefinitionDto
// ---------------------------------------------------------------------------
TEST(GraphDocumentDtoTest, GraphPortDefinitionDtoRoundTrip)
{
    GraphPortDefinitionDto port;
    port.port_id = "input_image";
    port.display_label = "Input Image";
    port.port_type = "image";
    port.is_required = true;
    port.default_value = MakeDefaultPayload();
    port.tags = {"input", "image"};

    auto serialized = yyjson::object(port);

    // Verify camelCase field names
    EXPECT_TRUE(serialized.contains(std::string_view("portId")));
    EXPECT_TRUE(serialized.contains(std::string_view("displayLabel")));
    EXPECT_TRUE(serialized.contains(std::string_view("portType")));
    EXPECT_TRUE(serialized.contains(std::string_view("isRequired")));
    EXPECT_TRUE(serialized.contains(std::string_view("defaultValue")));
    EXPECT_TRUE(serialized.contains(std::string_view("tags")));
    EXPECT_FALSE(serialized.contains(std::string_view("port_id")));

    GraphPortDefinitionDto round_tripped;
    SCOPED_TRACE(std::string(serialized.write()));
    ASSERT_NO_THROW(
        round_tripped = yyjson::cast<GraphPortDefinitionDto>(serialized));
    EXPECT_EQ(round_tripped.port_id, "input_image");
    EXPECT_EQ(round_tripped.display_label, "Input Image");
    EXPECT_EQ(round_tripped.port_type, "image");
    EXPECT_TRUE(round_tripped.is_required);
    EXPECT_EQ(round_tripped.tags.size(), 2u);
    EXPECT_EQ(round_tripped.tags[0], "input");
    EXPECT_EQ(round_tripped.tags[1], "image");
}

// ---------------------------------------------------------------------------
// ComponentRefDto
// ---------------------------------------------------------------------------
TEST(GraphDocumentDtoTest, ComponentRefDtoRoundTrip)
{
    ComponentRefDto ref;
    ref.kind = "componentRef";
    ref.component_guid = "{12345678-1234-1234-1234-123456789abc}";
    ref.plugin_guid = "{abcdef01-2345-6789-abcd-ef0123456789}";

    auto serialized = yyjson::object(ref);

    EXPECT_TRUE(serialized.contains(std::string_view("kind")));
    EXPECT_TRUE(serialized.contains(std::string_view("componentGuid")));
    EXPECT_TRUE(serialized.contains(std::string_view("pluginGuid")));
    EXPECT_FALSE(serialized.contains(std::string_view("component_guid")));

    ComponentRefDto round_tripped;
    SCOPED_TRACE(std::string(serialized.write()));
    ASSERT_NO_THROW(round_tripped = yyjson::cast<ComponentRefDto>(serialized));
    EXPECT_EQ(round_tripped.kind, "componentRef");
    EXPECT_EQ(
        round_tripped.component_guid,
        "{12345678-1234-1234-1234-123456789abc}");
    EXPECT_EQ(
        round_tripped.plugin_guid,
        "{abcdef01-2345-6789-abcd-ef0123456789}");
}

// ---------------------------------------------------------------------------
// EntryRefDto
// ---------------------------------------------------------------------------
TEST(GraphDocumentDtoTest, EntryRefDtoRoundTrip)
{
    EntryRefDto ref;
    ref.kind = "entryRef";
    ref.entry_id = 42;
    ref.expected_revision = 7;
    ref.source_fingerprint = "abc123hash";

    auto serialized = yyjson::object(ref);

    EXPECT_TRUE(serialized.contains(std::string_view("kind")));
    EXPECT_TRUE(serialized.contains(std::string_view("entryId")));
    EXPECT_TRUE(serialized.contains(std::string_view("expectedRevision")));
    EXPECT_TRUE(serialized.contains(std::string_view("sourceFingerprint")));

    EntryRefDto round_tripped;
    SCOPED_TRACE(std::string(serialized.write()));
    ASSERT_NO_THROW(round_tripped = yyjson::cast<EntryRefDto>(serialized));
    EXPECT_EQ(round_tripped.kind, "entryRef");
    EXPECT_EQ(round_tripped.entry_id, 42);
    ASSERT_TRUE(round_tripped.expected_revision.has_value());
    EXPECT_EQ(round_tripped.expected_revision.value(), 7);
    ASSERT_TRUE(round_tripped.source_fingerprint.has_value());
    EXPECT_EQ(round_tripped.source_fingerprint.value(), "abc123hash");
}

// ---------------------------------------------------------------------------
// GraphNodeTargetDto — componentRef variant
// ---------------------------------------------------------------------------
TEST(GraphDocumentDtoTest, GraphNodeTargetDtoComponentRefRoundTrip)
{
    GraphNodeTargetDto target;
    target.target_kind = "componentRef";
    target.component_ref =
        ComponentRefDto{"componentRef", "{guid-component}", "{guid-plugin}"};
    target.entry_ref = std::nullopt;

    auto serialized = yyjson::object(target);

    EXPECT_TRUE(serialized.contains(std::string_view("targetKind")));
    EXPECT_TRUE(serialized.contains(std::string_view("componentRef")));
    EXPECT_TRUE(serialized.contains(std::string_view("entryRef")));

    GraphNodeTargetDto round_tripped;
    SCOPED_TRACE(std::string(serialized.write()));
    ASSERT_NO_THROW(
        round_tripped = yyjson::cast<GraphNodeTargetDto>(serialized));
    EXPECT_EQ(round_tripped.target_kind, "componentRef");
    ASSERT_TRUE(round_tripped.component_ref.has_value());
    EXPECT_EQ(round_tripped.component_ref->component_guid, "{guid-component}");
    EXPECT_EQ(round_tripped.component_ref->plugin_guid, "{guid-plugin}");
    EXPECT_FALSE(round_tripped.entry_ref.has_value());
}

// ---------------------------------------------------------------------------
// GraphNodeTargetDto — entryRef variant
// ---------------------------------------------------------------------------
TEST(GraphDocumentDtoTest, GraphNodeTargetDtoEntryRefRoundTrip)
{
    GraphNodeTargetDto target;
    target.target_kind = "entryRef";
    target.component_ref = std::nullopt;
    target.entry_ref = EntryRefDto{"entryRef", 99, 3, "fp-hash"};

    auto serialized = yyjson::object(target);

    GraphNodeTargetDto round_tripped;
    SCOPED_TRACE(std::string(serialized.write()));
    ASSERT_NO_THROW(
        round_tripped = yyjson::cast<GraphNodeTargetDto>(serialized));
    EXPECT_EQ(round_tripped.target_kind, "entryRef");
    EXPECT_FALSE(round_tripped.component_ref.has_value());
    ASSERT_TRUE(round_tripped.entry_ref.has_value());
    EXPECT_EQ(round_tripped.entry_ref->entry_id, 99);
    ASSERT_TRUE(round_tripped.entry_ref->expected_revision.has_value());
    EXPECT_EQ(round_tripped.entry_ref->expected_revision.value(), 3);
    ASSERT_TRUE(round_tripped.entry_ref->source_fingerprint.has_value());
    EXPECT_EQ(round_tripped.entry_ref->source_fingerprint.value(), "fp-hash");
}

// ---------------------------------------------------------------------------
// GraphNodeDto
// ---------------------------------------------------------------------------
TEST(GraphDocumentDtoTest, GraphNodeDtoRoundTrip)
{
    GraphNodeDto node;
    node.node_id = "node_1";
    node.target = GraphNodeTargetDto{
        "componentRef",
        ComponentRefDto{"componentRef", "{comp-guid}", "{plug-guid}"},
        std::nullopt};
    node.settings = MakeSettingsPayload();
    node.dynamic_ports.push_back(
        GraphPortDefinitionDto{
            "output_result",
            "Result",
            "string",
            true,
            yyjson::value(),
            {}});

    auto serialized = yyjson::object(node);

    EXPECT_TRUE(serialized.contains(std::string_view("nodeId")));
    EXPECT_TRUE(serialized.contains(std::string_view("target")));
    EXPECT_TRUE(serialized.contains(std::string_view("settings")));
    EXPECT_TRUE(serialized.contains(std::string_view("dynamicPorts")));
    EXPECT_FALSE(serialized.contains(std::string_view("dynamic_ports")));

    GraphNodeDto round_tripped;
    SCOPED_TRACE(std::string(serialized.write()));
    ASSERT_NO_THROW(round_tripped = yyjson::cast<GraphNodeDto>(serialized));
    EXPECT_EQ(round_tripped.node_id, "node_1");
    EXPECT_EQ(round_tripped.target.target_kind, "componentRef");
    ASSERT_EQ(round_tripped.dynamic_ports.size(), 1u);
    EXPECT_EQ(round_tripped.dynamic_ports[0].port_id, "output_result");
}

// ---------------------------------------------------------------------------
// GraphEdgeDto
// ---------------------------------------------------------------------------
TEST(GraphDocumentDtoTest, GraphEdgeDtoRoundTrip)
{
    GraphEdgeDto edge;
    edge.edge_id = "edge_1";
    edge.source_node_id = "node_1";
    edge.source_port_id = "output_result";
    edge.target_node_id = "node_2";
    edge.target_port_id = "input_data";

    auto serialized = yyjson::object(edge);

    EXPECT_TRUE(serialized.contains(std::string_view("edgeId")));
    EXPECT_TRUE(serialized.contains(std::string_view("sourceNodeId")));
    EXPECT_TRUE(serialized.contains(std::string_view("sourcePortId")));
    EXPECT_TRUE(serialized.contains(std::string_view("targetNodeId")));
    EXPECT_TRUE(serialized.contains(std::string_view("targetPortId")));
    EXPECT_FALSE(serialized.contains(std::string_view("source_node_id")));

    GraphEdgeDto round_tripped;
    SCOPED_TRACE(std::string(serialized.write()));
    ASSERT_NO_THROW(round_tripped = yyjson::cast<GraphEdgeDto>(serialized));
    EXPECT_EQ(round_tripped.edge_id, "edge_1");
    EXPECT_EQ(round_tripped.source_node_id, "node_1");
    EXPECT_EQ(round_tripped.source_port_id, "output_result");
    EXPECT_EQ(round_tripped.target_node_id, "node_2");
    EXPECT_EQ(round_tripped.target_port_id, "input_data");
}

// ---------------------------------------------------------------------------
// GraphDocumentDto (full document round-trip)
// ---------------------------------------------------------------------------
TEST(GraphDocumentDtoTest, GraphDocumentDtoRoundTrip)
{
    GraphDocumentDto doc;
    doc.document_id = "graph_001";
    doc.version = 1;
    doc.fingerprint = "sha256:abcdef123456";
    doc.nodes.push_back(
        GraphNodeDto{
            "node_1",
            GraphNodeTargetDto{
                "componentRef",
                ComponentRefDto{"componentRef", "{comp-guid}", "{plug-guid}"},
                std::nullopt},
            MakeSettingsPayload(),
            {}});
    doc.edges.push_back(
        GraphEdgeDto{
            "edge_1",
            "node_1",
            "output_result",
            "node_2",
            "input_data"});
    doc.graph_inputs.push_back(
        GraphPortDefinitionDto{
            "graph_input",
            "Graph Input",
            "image",
            true,
            yyjson::value(),
            {}});
    doc.graph_outputs.push_back(
        GraphPortDefinitionDto{
            "graph_output",
            "Graph Output",
            "string",
            false,
            yyjson::value(),
            {}});

    auto serialized = yyjson::object(doc);

    EXPECT_TRUE(serialized.contains(std::string_view("documentId")));
    EXPECT_TRUE(serialized.contains(std::string_view("version")));
    EXPECT_TRUE(serialized.contains(std::string_view("fingerprint")));
    EXPECT_TRUE(serialized.contains(std::string_view("nodes")));
    EXPECT_TRUE(serialized.contains(std::string_view("edges")));
    EXPECT_TRUE(serialized.contains(std::string_view("graphInputs")));
    EXPECT_TRUE(serialized.contains(std::string_view("graphOutputs")));
    EXPECT_FALSE(serialized.contains(std::string_view("document_id")));
    EXPECT_FALSE(serialized.contains(std::string_view("graph_inputs")));

    GraphDocumentDto round_tripped;
    SCOPED_TRACE(std::string(serialized.write()));
    ASSERT_NO_THROW(round_tripped = yyjson::cast<GraphDocumentDto>(serialized));
    EXPECT_EQ(round_tripped.document_id, "graph_001");
    EXPECT_EQ(round_tripped.version, 1);
    EXPECT_EQ(round_tripped.fingerprint, "sha256:abcdef123456");
    ASSERT_EQ(round_tripped.nodes.size(), 1u);
    EXPECT_EQ(round_tripped.nodes[0].node_id, "node_1");
    ASSERT_EQ(round_tripped.edges.size(), 1u);
    EXPECT_EQ(round_tripped.edges[0].edge_id, "edge_1");
    ASSERT_EQ(round_tripped.graph_inputs.size(), 1u);
    EXPECT_EQ(round_tripped.graph_inputs[0].port_id, "graph_input");
    ASSERT_EQ(round_tripped.graph_outputs.size(), 1u);
    EXPECT_EQ(round_tripped.graph_outputs[0].port_id, "graph_output");
}

// ===========================================================================
// GraphTaskEntryStorageTest — entry storage round-trip (v31)
// ===========================================================================

namespace
{
    using TaskDto =
        Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto;

    /// Build a small but complete GraphDocumentDto for testing.
    GraphDocumentDto MakeTestGraphDocument()
    {
        GraphDocumentDto doc;
        doc.document_id = "test_graph_001";
        doc.version = 1;
        doc.fingerprint = "sha256:abc123";

        // Node 1 — componentRef
        GraphNodeDto node1;
        node1.node_id = "node_comp";
        node1.target = GraphNodeTargetDto{
            "componentRef",
            ComponentRefDto{
                "componentRef",
                "{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}",
                "{11111111-2222-3333-4444-555555555555}"},
            std::nullopt};
        node1.settings = MakeSettingsPayload();

        // Node 2 — entryRef
        GraphNodeDto node2;
        node2.node_id = "node_entry";
        node2.target = GraphNodeTargetDto{
            "entryRef",
            std::nullopt,
            EntryRefDto{"entryRef", 42, 7, "fp_abc123"}};
        node2.settings = MakeDefaultPayload();

        doc.nodes.push_back(std::move(node1));
        doc.nodes.push_back(std::move(node2));

        // Edge
        doc.edges.push_back(
            GraphEdgeDto{
                "edge_1",
                "node_comp",
                "output_result",
                "node_entry",
                "input_data"});

        // Graph input port
        doc.graph_inputs.push_back(
            GraphPortDefinitionDto{
                "graph_in",
                "Graph Input",
                "image",
                true,
                yyjson::value(),
                {}});
        return doc;
    }

    /// Round-trip a GraphDocumentDto through
    /// RepositoryEntryDto::graph_document. Serialises the full entry to JSON,
    /// deserialises, and extracts the graph.
    GraphDocumentDto RoundTripGraphDocument(const GraphDocumentDto& original)
    {
        // 1. Build a RepositoryEntryDto with the graph stored in graph_document
        TaskDto entry;
        entry.entry_id = 1;
        entry.display_name = "test_entry";
        entry.plugin_guid = "{plugin-guid}";
        entry.task_type_guid = "{task-type-guid}";
        entry.graph_document = yyjson::object(original);

        // 2. Serialise full entry to JSON string
        auto entry_json = yyjson::object(entry);
        auto json_str = std::string(entry_json.write());

        // 3. Deserialise back
        auto    parsed = yyjson::read(json_str);
        TaskDto restored = yyjson::cast<TaskDto>(parsed);

        // 4. Extract and deserialise the graph_document
        auto graph_obj = restored.graph_document;
        return yyjson::cast<GraphDocumentDto>(graph_obj);
    }

    /// Round-trip via accepted_properties.graphDocument (legacy path).
    GraphDocumentDto RoundTripThroughAcceptedProperties(
        const GraphDocumentDto& original)
    {
        TaskDto entry;
        entry.entry_id = 1;
        entry.display_name = "legacy_entry";

        auto props = Das::Utils::MakeYyjsonObject();
        auto obj = *props.as_object();
        obj[std::string_view("graphDocument")] = yyjson::object(original);
        entry.accepted_properties = std::move(props);

        auto entry_json = yyjson::object(entry);
        auto json_str = std::string(entry_json.write());

        auto    parsed = yyjson::read(json_str);
        TaskDto restored = yyjson::cast<TaskDto>(parsed);

        auto ap = restored.accepted_properties;
        auto ap_obj = ap.as_object();
        EXPECT_TRUE(ap_obj.has_value());
        auto graph_val = (*ap_obj)[std::string_view("graphDocument")];
        return yyjson::cast<GraphDocumentDto>(graph_val);
    }
} // namespace

// ---------------------------------------------------------------------------
// GraphEntryId compile-time check
// ---------------------------------------------------------------------------
TEST(GraphTaskEntryStorageTest, GraphEntryIdIsInt64)
{
    // Also verified by static_assert at top of file.
    Das::Core::GraphRuntime::GraphEntryId id = 42;
    EXPECT_EQ(id, 42);
    EXPECT_TRUE((std::is_same_v<decltype(id), int64_t>));
}

// ---------------------------------------------------------------------------
// RepositoryEntryDto has graph_document field (compile-time)
// ---------------------------------------------------------------------------
TEST(GraphTaskEntryStorageTest, EntryDtoHasGraphDocumentField)
{
    TaskDto entry;
    // The field must exist and be of type yyjson::value.
    auto& field = entry.graph_document;
    (void)field;
    EXPECT_TRUE(entry.graph_document.is_null());
}

// ---------------------------------------------------------------------------
// graph_document serialises as camelCase "graphDocument"
// ---------------------------------------------------------------------------
TEST(GraphTaskEntryStorageTest, GraphDocumentFieldSerializesAsCamelCase)
{
    TaskDto entry;
    entry.entry_id = 99;
    entry.graph_document = yyjson::object(MakeTestGraphDocument());

    auto serialized = yyjson::object(entry);
    auto json_str = std::string(serialized.write());

    EXPECT_TRUE(json_str.find("\"graphDocument\"") != std::string::npos)
        << "JSON must use camelCase key 'graphDocument', got: " << json_str;
    EXPECT_FALSE(json_str.find("\"graph_document\"") != std::string::npos)
        << "JSON must NOT use snake_case key 'graph_document'";
}

// ---------------------------------------------------------------------------
// GraphDocument round-trip through graph_document field
// ---------------------------------------------------------------------------
TEST(GraphTaskEntryStorageTest, GraphDocumentRoundTripThroughGraphDocument)
{
    auto original = MakeTestGraphDocument();
    auto restored = RoundTripGraphDocument(original);

    EXPECT_EQ(restored.document_id, "test_graph_001");
    EXPECT_EQ(restored.version, 1);
    EXPECT_EQ(restored.fingerprint, "sha256:abc123");
    ASSERT_EQ(restored.nodes.size(), 2u);
    EXPECT_EQ(restored.nodes[0].node_id, "node_comp");
    EXPECT_EQ(restored.nodes[0].target.target_kind, "componentRef");
    ASSERT_TRUE(restored.nodes[0].target.component_ref.has_value());
    EXPECT_EQ(
        restored.nodes[0].target.component_ref->component_guid,
        "{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}");
    ASSERT_EQ(restored.edges.size(), 1u);
    EXPECT_EQ(restored.edges[0].edge_id, "edge_1");
    EXPECT_EQ(restored.edges[0].source_port_id, "output_result");
    ASSERT_EQ(restored.graph_inputs.size(), 1u);
    EXPECT_EQ(restored.graph_inputs[0].port_id, "graph_in");
}

// ---------------------------------------------------------------------------
// Round-trip through accepted_properties.graphDocument (legacy path)
// ---------------------------------------------------------------------------
TEST(GraphTaskEntryStorageTest, GraphDocumentRoundTripThroughAcceptedProperties)
{
    auto original = MakeTestGraphDocument();
    auto restored = RoundTripThroughAcceptedProperties(original);

    EXPECT_EQ(restored.document_id, "test_graph_001");
    ASSERT_EQ(restored.nodes.size(), 2u);
    EXPECT_EQ(restored.nodes[0].target.target_kind, "componentRef");
    ASSERT_EQ(restored.edges.size(), 1u);
    EXPECT_EQ(restored.edges[0].source_port_id, "output_result");
    ASSERT_EQ(restored.graph_inputs.size(), 1u);
    EXPECT_EQ(restored.graph_inputs[0].port_id, "graph_in");
}

// ---------------------------------------------------------------------------
// EntryRef target survives round-trip
// ---------------------------------------------------------------------------
TEST(GraphTaskEntryStorageTest, GraphDocumentRoundTripPreservesEntryRefTarget)
{
    auto original = MakeTestGraphDocument();
    auto restored = RoundTripGraphDocument(original);

    ASSERT_EQ(restored.nodes.size(), 2u);
    auto& node2 = restored.nodes[1];
    EXPECT_EQ(node2.node_id, "node_entry");
    EXPECT_EQ(node2.target.target_kind, "entryRef");
    ASSERT_TRUE(node2.target.entry_ref.has_value());
    EXPECT_EQ(node2.target.entry_ref->entry_id, 42);
    ASSERT_TRUE(node2.target.entry_ref->expected_revision.has_value());
    EXPECT_EQ(node2.target.entry_ref->expected_revision.value(), 7);
    ASSERT_TRUE(node2.target.entry_ref->source_fingerprint.has_value());
    EXPECT_EQ(node2.target.entry_ref->source_fingerprint.value(), "fp_abc123");
}

// ---------------------------------------------------------------------------
// Edge port_ids are strings (v45 contract) and survive round-trip
// ---------------------------------------------------------------------------
TEST(GraphTaskEntryStorageTest, GraphDocumentRoundTripPreservesPortIdSemantics)
{
    auto original = MakeTestGraphDocument();
    auto restored = RoundTripGraphDocument(original);

    ASSERT_EQ(restored.edges.size(), 1u);
    EXPECT_EQ(restored.edges[0].source_port_id, "output_result");
    EXPECT_EQ(restored.edges[0].target_port_id, "input_data");
    // port_ids must be strings (not numeric indices)
    EXPECT_FALSE(restored.edges[0].source_port_id.empty());
    EXPECT_FALSE(restored.edges[0].target_port_id.empty());
}

// ---------------------------------------------------------------------------
// Empty graph round-trips correctly
// ---------------------------------------------------------------------------
TEST(GraphTaskEntryStorageTest, GraphDocumentRoundTripEmptyGraph)
{
    GraphDocumentDto empty;
    empty.document_id = "empty_graph";
    empty.version = 1;
    empty.fingerprint = "sha256:empty";

    auto restored = RoundTripGraphDocument(empty);

    EXPECT_EQ(restored.document_id, "empty_graph");
    EXPECT_EQ(restored.version, 1);
    EXPECT_EQ(restored.fingerprint, "sha256:empty");
    EXPECT_TRUE(restored.nodes.empty());
    EXPECT_TRUE(restored.edges.empty());
    EXPECT_TRUE(restored.graph_inputs.empty());
    EXPECT_TRUE(restored.graph_outputs.empty());
}
