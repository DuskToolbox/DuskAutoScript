#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/GraphRuntime/GraphEntryId.h>
#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/Core/TaskScheduler/TaskRepositoryDtos.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
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
// GraphNodeTargetDto — componentRef variant
// ---------------------------------------------------------------------------
TEST(GraphDocumentDtoTest, GraphNodeTargetDtoComponentRefRoundTrip)
{
    GraphNodeTargetDto target;
    target.target_kind = "componentRef";
    target.component_ref =
        ComponentRefDto{"componentRef", "{guid-component}", "{guid-plugin}"};

    auto serialized = yyjson::object(target);

    EXPECT_TRUE(serialized.contains(std::string_view("targetKind")));
    EXPECT_TRUE(serialized.contains(std::string_view("componentRef")));
    EXPECT_FALSE(serialized.contains(std::string_view("entryRef")));

    GraphNodeTargetDto round_tripped;
    SCOPED_TRACE(std::string(serialized.write()));
    ASSERT_NO_THROW(
        round_tripped = yyjson::cast<GraphNodeTargetDto>(serialized));
    EXPECT_EQ(round_tripped.target_kind, "componentRef");
    ASSERT_TRUE(round_tripped.component_ref.has_value());
    EXPECT_EQ(round_tripped.component_ref->component_guid, "{guid-component}");
    EXPECT_EQ(round_tripped.component_ref->plugin_guid, "{guid-plugin}");
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
        ComponentRefDto{"componentRef", "{comp-guid}", "{plug-guid}"}};
    node.settings = MakeSettingsPayload();

    auto serialized = yyjson::object(node);

    EXPECT_TRUE(serialized.contains(std::string_view("nodeId")));
    EXPECT_TRUE(serialized.contains(std::string_view("target")));
    EXPECT_TRUE(serialized.contains(std::string_view("settings")));
    // 节点不再携带端口声明——端口面归声明面（manifest/catalog）与推导。
    EXPECT_FALSE(serialized.contains(std::string_view("dynamicPorts")));

    GraphNodeDto round_tripped;
    SCOPED_TRACE(std::string(serialized.write()));
    ASSERT_NO_THROW(round_tripped = yyjson::cast<GraphNodeDto>(serialized));
    EXPECT_EQ(round_tripped.node_id, "node_1");
    EXPECT_EQ(round_tripped.target.target_kind, "componentRef");
}

// The store display label survives the aggregate serialization round-trip,
// and a node persisted before the label field existed (key absent)
// deserializes with the label disengaged instead of throwing — the caster
// guards every field read with contains().
TEST(GraphDocumentDtoTest, GraphNodeDtoLabelRoundTripAndMissingKeySafe)
{
    GraphNodeDto node;
    node.node_id = "node_1";
    node.label = "我的延迟节点";

    auto serialized = yyjson::object(node);
    ASSERT_TRUE(serialized.contains(std::string_view("label")));

    GraphNodeDto round_tripped;
    SCOPED_TRACE(std::string(serialized.write()));
    ASSERT_NO_THROW(round_tripped = yyjson::cast<GraphNodeDto>(serialized));
    ASSERT_TRUE(round_tripped.label.has_value());
    EXPECT_EQ(*round_tripped.label, "我的延迟节点");

    // A pre-label document carries no "label" key at all — the caster must
    // keep the field disengaged rather than fail the whole node.
    const auto legacy = Das::Utils::ParseYyjsonFromString(
        std::string_view(
            R"({"nodeId":"legacy_node","target":{"targetKind":"componentRef"},)"
            R"("settings":{}})"));
    ASSERT_TRUE(legacy.has_value());
    GraphNodeDto legacy_node;
    SCOPED_TRACE(
        Das::Utils::SerializeYyjsonValue(*legacy).value_or(std::string{}));
    ASSERT_NO_THROW(legacy_node = yyjson::cast<GraphNodeDto>(*legacy));
    EXPECT_EQ(legacy_node.node_id, "legacy_node");
    EXPECT_FALSE(legacy_node.label.has_value());
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
                ComponentRefDto{"componentRef", "{comp-guid}", "{plug-guid}"}},
            MakeSettingsPayload()});
    doc.edges.push_back(
        GraphEdgeDto{
            "edge_1",
            "node_1",
            "output_result",
            "node_2",
            "input_data"});
    // Graph-level markers (e.g. "fromsequence") — the authoring-doc contract
    // the frontend depends on (DAS-75).
    doc.tags = {"fromsequence"};

    auto serialized = yyjson::object(doc);

    EXPECT_TRUE(serialized.contains(std::string_view("documentId")));
    EXPECT_TRUE(serialized.contains(std::string_view("version")));
    EXPECT_TRUE(serialized.contains(std::string_view("fingerprint")));
    EXPECT_TRUE(serialized.contains(std::string_view("nodes")));
    EXPECT_TRUE(serialized.contains(std::string_view("edges")));
    EXPECT_TRUE(serialized.contains(std::string_view("tags")));
    EXPECT_FALSE(serialized.contains(std::string_view("document_id")));
    // 文档不再携带图级端口声明——边界口是推导物（声明面+边角色）。
    EXPECT_FALSE(serialized.contains(std::string_view("graphInputs")));
    EXPECT_FALSE(serialized.contains(std::string_view("graphOutputs")));

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
    ASSERT_EQ(round_tripped.tags.size(), 1u);
    EXPECT_EQ(round_tripped.tags[0], "fromsequence");
}

// ---------------------------------------------------------------------------
// viewData partition — legacy-doc safety and coordinate round-trip
// ---------------------------------------------------------------------------

/// Read viewData.positions[node_id].axis from a document blob; nullopt when
/// the partition, the node entry, or the axis is absent.
std::optional<double> ViewDataPositionOf(
    const GraphDocumentDto& doc,
    std::string_view        node_id,
    std::string_view        axis)
{
    auto view = doc.view_data.as_object();
    if (!view || !view->contains(std::string_view("positions")))
    {
        return std::nullopt;
    }
    auto positions = (*view)[std::string_view("positions")].as_object();
    if (!positions || !positions->contains(node_id))
    {
        return std::nullopt;
    }
    auto position = (*positions)[node_id].as_object();
    if (!position || !position->contains(axis))
    {
        return std::nullopt;
    }
    return (*position)[axis].as_num();
}

/// Materialize a JSON literal into a writer-side value, mirroring how
/// production consumes persisted blobs (RepositoryEntryDto::graph_document
/// is a writer value by the time GraphDocumentDto is cast from it — a
/// missing key keeps the field default instead of tripping the reader-side
/// strict operator[]).
yyjson::value ToWriterValue(std::string_view json)
{
    auto parsed = yyjson::read(std::string(json));
    const yyjson::reader::const_value_ref parsed_ref = parsed;
    return Das::Utils::CloneYyjsonValue(parsed_ref);
}

// A pre-viewData document carries every old DTO key and no viewData entry.
constexpr std::string_view OLD_DOC_WITHOUT_VIEW_DATA =
    R"({"documentId":"d1","version":1,"fingerprint":"fp","nodes":[],)"
    R"("edges":[],"tags":[]})";

TEST(GraphDocumentDtoTest, ViewDataMissingKeyDeserializesToNull)
{
    GraphDocumentDto doc;
    SCOPED_TRACE(std::string(OLD_DOC_WITHOUT_VIEW_DATA));
    ASSERT_NO_THROW(
        doc = yyjson::cast<GraphDocumentDto>(
            ToWriterValue(OLD_DOC_WITHOUT_VIEW_DATA)));
    EXPECT_TRUE(doc.view_data.is_null());
}

TEST(GraphDocumentDtoTest, ViewDataExplicitNullDeserializesToNull)
{
    const std::string json = std::string(OLD_DOC_WITHOUT_VIEW_DATA.substr(
                                 0,
                                 OLD_DOC_WITHOUT_VIEW_DATA.size() - 1))
                             + R"(,"viewData":null})";
    GraphDocumentDto  doc;
    SCOPED_TRACE(json);
    ASSERT_NO_THROW(doc = yyjson::cast<GraphDocumentDto>(ToWriterValue(json)));
    EXPECT_TRUE(doc.view_data.is_null());
}

TEST(GraphDocumentDtoTest, ViewDataCoordinatesRoundTrip)
{
    const std::string json =
        std::string(OLD_DOC_WITHOUT_VIEW_DATA.substr(
            0,
            OLD_DOC_WITHOUT_VIEW_DATA.size() - 1))
        + R"(,"viewData":{"positions":{"n1":{"x":1.5,"y":2.5},)"
          R"("n2":{"x":30,"y":40}}}})";
    GraphDocumentDto doc;
    ASSERT_NO_THROW(doc = yyjson::cast<GraphDocumentDto>(ToWriterValue(json)));
    ASSERT_TRUE(doc.view_data.as_object().has_value());

    auto serialized = yyjson::object(doc);
    EXPECT_TRUE(serialized.contains(std::string_view("viewData")));
    GraphDocumentDto round_tripped;
    SCOPED_TRACE(std::string(serialized.write()));
    ASSERT_NO_THROW(round_tripped = yyjson::cast<GraphDocumentDto>(serialized));

    ASSERT_TRUE(ViewDataPositionOf(round_tripped, "n1", "x").has_value());
    EXPECT_DOUBLE_EQ(*ViewDataPositionOf(round_tripped, "n1", "x"), 1.5);
    ASSERT_TRUE(ViewDataPositionOf(round_tripped, "n1", "y").has_value());
    EXPECT_DOUBLE_EQ(*ViewDataPositionOf(round_tripped, "n1", "y"), 2.5);
    // Integer-formatted coordinates round-trip numerically equal.
    ASSERT_TRUE(ViewDataPositionOf(round_tripped, "n2", "x").has_value());
    EXPECT_DOUBLE_EQ(*ViewDataPositionOf(round_tripped, "n2", "x"), 30.0);
    ASSERT_TRUE(ViewDataPositionOf(round_tripped, "n2", "y").has_value());
    EXPECT_DOUBLE_EQ(*ViewDataPositionOf(round_tripped, "n2", "y"), 40.0);
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
                "{11111111-2222-3333-4444-555555555555}"}};
        node1.settings = MakeSettingsPayload();

        // Node 2 — second componentRef（边与往返断言的双节点形态）
        GraphNodeDto node2;
        node2.node_id = "node_entry";
        node2.target = GraphNodeTargetDto{
            "componentRef",
            ComponentRefDto{
                "componentRef",
                "{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}",
                "{11111111-2222-3333-4444-555555555555}"}};
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
        auto          parsed = yyjson::read(json_str);
        const TaskDto restored = yyjson::cast<TaskDto>(parsed);

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

        auto          parsed = yyjson::read(json_str);
        const TaskDto restored = yyjson::cast<TaskDto>(parsed);

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
    const TaskDto entry;
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
}

// ---------------------------------------------------------------------------
// 旧节点 ports 分区的会话创建兼容迁移（分区退役的在途数据保全）：
// begin/end 节点旧分区等价迁入 settings.boundaryPorts；调用节点物化口/
// 普通节点残留分区丢弃（口面经 entryFaces 现算/文档口面推导）；新形状
// （无分区）空转。观测通道=会话创建后一次拒绝变更的 acceptedProperties
// （=SerializeSource，存储真源）。
// ---------------------------------------------------------------------------
namespace
{
    constexpr std::string_view kLegacyBeginGuid =
        "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E";
    constexpr std::string_view kLegacyEndGuid =
        "E1B7C9D3-5F4A-4E2B-8D6C-7A9E1F3B5C2D";

    struct LegacySessionHandle
    {
        GraphAuthoringSessionState* state = nullptr;

        explicit LegacySessionHandle(const std::string& context)
        {
            Das::DasPtr<IDasReadOnlyString> context_string;
            CreateIDasReadOnlyStringFromUtf8(
                context.c_str(),
                context_string.Put());
            CreateGraphAuthoringSession(nullptr, context_string.Get(), &state);
        }

        ~LegacySessionHandle() { DestroyGraphAuthoringSession(state); }

        LegacySessionHandle(const LegacySessionHandle&) = delete;
        LegacySessionHandle& operator=(const LegacySessionHandle&) = delete;
    };

    std::string LegacyNodeText(
        std::string_view node_id,
        std::string_view guid,
        std::string_view settings_extra,
        std::string_view legacy_ports)
    {
        std::string node =
            std::string(R"({"nodeId":")") + std::string(node_id)
            + R"(","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":")"
            + std::string(guid) + R"(","pluginGuid":"{p}"}},"settings":{)"
            + std::string(settings_extra) + "}";
        if (!legacy_ports.empty())
        {
            node += ",\"ports\":" + std::string(legacy_ports);
        }
        node += "}";
        return node;
    }

    // 一次拒绝变更（空 change）的结果 acceptedProperties——存储真源的
    // 观测通道。
    yyjson::value AcceptedPropertiesOf(GraphAuthoringSessionState* state)
    {
        Das::DasPtr<IDasReadOnlyString> empty_change;
        CreateIDasReadOnlyStringFromUtf8("", empty_change.Put());
        Das::DasPtr<Das::ExportInterface::IDasJson> result;
        GraphAuthoringSessionApplyChange(
            state,
            empty_change.Get(),
            result.Put());
        if (result.Get() == nullptr)
        {
            return Das::Utils::MakeYyjsonObject();
        }
        Das::DasPtr<IDasReadOnlyString> text;
        if (DAS::IsFailed(result->ToString(0, text.Put())) || !text.Get())
        {
            return Das::Utils::MakeYyjsonObject();
        }
        const char* utf8 = nullptr;
        if (DAS::IsFailed(text->GetUtf8(&utf8)) || utf8 == nullptr)
        {
            return Das::Utils::MakeYyjsonObject();
        }
        auto root = yyjson::read(std::string(utf8));
        auto obj = root.as_object();
        if (!obj || !obj->contains(std::string_view("acceptedProperties")))
        {
            return Das::Utils::MakeYyjsonObject();
        }
        return Das::Utils::CloneYyjsonValue(
            (*obj)[std::string_view("acceptedProperties")]);
    }

    // acceptedProperties.nodes[node_id] 的 settings.boundaryPorts 条目表
    // （{id,type} 对）。
    std::vector<std::pair<std::string, std::string>> MigratedBoundaryEntriesOf(
        const yyjson::value& source,
        std::string_view     node_id)
    {
        std::vector<std::pair<std::string, std::string>> entries;
        auto source_obj = source.as_object();
        if (!source_obj || !source_obj->contains(std::string_view("nodes")))
        {
            return entries;
        }
        auto nodes = (*source_obj)[std::string_view("nodes")].as_array();
        if (!nodes)
        {
            return entries;
        }
        for (const auto& node_value : *nodes)
        {
            auto node_obj = node_value.as_object();
            if (!node_obj
                || (*node_obj)[std::string_view("nodeId")].as_string().value_or(
                       "")
                       != node_id)
            {
                continue;
            }
            EXPECT_FALSE(node_obj->contains(std::string_view("ports")))
                << "the partition key must not survive serialization";
            auto settings =
                (*node_obj)[std::string_view("settings")].as_object();
            if (!settings
                || !settings->contains(std::string_view("boundaryPorts")))
            {
                return entries;
            }
            auto arr =
                (*settings)[std::string_view("boundaryPorts")].as_array();
            if (!arr)
            {
                return entries;
            }
            for (const auto& entry_value : *arr)
            {
                auto entry_obj = entry_value.as_object();
                if (!entry_obj)
                {
                    continue;
                }
                entries.emplace_back(
                    (*entry_obj)[std::string_view("id")].as_string().value_or(
                        ""),
                    (*entry_obj)[std::string_view("type")].as_string().value_or(
                        ""));
            }
            return entries;
        }
        return entries;
    }
} // namespace

TEST(GraphDocumentDtoTest, LegacyBoundaryPartitionMigratesToSettings)
{
    const LegacySessionHandle session{
        std::string(R"({"nodes":[)")
        + LegacyNodeText(
            "n_0",
            kLegacyBeginGuid,
            "",
            R"([{"id":"p_1","name":"左值","direction":"input","valueType":"int"},{"id":"p_2","name":"右值","direction":"input","valueType":"string"}])")
        + ","
        + LegacyNodeText(
            "n_1",
            kLegacyEndGuid,
            "",
            R"([{"id":"q_1","name":"结果","direction":"output","valueType":"image"}])")
        + "," + LegacyNodeText("n_work", "{g1}", "", "") + R"(],"edges":[]})"};

    const auto source = AcceptedPropertiesOf(session.state);

    // begin 旧分区逐条等价迁移（口 id/类型对应；name/direction 不迁——
    // 边界条目单 id 形态+角色固化）。
    const auto begin_entries = MigratedBoundaryEntriesOf(source, "n_0");
    ASSERT_EQ(begin_entries.size(), 2u);
    EXPECT_EQ(begin_entries[0].first, "p_1");
    EXPECT_EQ(begin_entries[0].second, "int");
    EXPECT_EQ(begin_entries[1].first, "p_2");
    EXPECT_EQ(begin_entries[1].second, "string");

    // end 同款。
    const auto end_entries = MigratedBoundaryEntriesOf(source, "n_1");
    ASSERT_EQ(end_entries.size(), 1u);
    EXPECT_EQ(end_entries[0].first, "q_1");
    EXPECT_EQ(end_entries[0].second, "image");

    // 普通节点零口数据（不迁不长键）。
    EXPECT_TRUE(MigratedBoundaryEntriesOf(source, "n_work").empty());
}

TEST(GraphDocumentDtoTest, LegacyMaterializedPartitionDroppedWithWarn)
{
    const LegacySessionHandle session{
        std::string(R"({"nodes":[)")
        + LegacyNodeText(
            "n_call",
            "{ginvoke}",
            R"("repositoryRef":42)",
            R"([{"id":"p_1","name":"payload","direction":"input","valueType":"int"},{"id":"p_1","name":"score","direction":"output","valueType":"int"}])")
        + R"(],"edges":[]})"};

    // 物化口数据丢弃（口面经 entryFaces 现算）——存储零口数据；丢弃经
    // WARN 可观测（日志面，本用例钉存储面）。
    const auto source = AcceptedPropertiesOf(session.state);
    EXPECT_TRUE(MigratedBoundaryEntriesOf(source, "n_call").empty());
}

TEST(GraphDocumentDtoTest, ExistingBoundaryEntriesMergeWithLegacyPartition)
{
    // 既有 settings.boundaryPorts 与旧分区共存（迁移追加，不清既有）。
    const LegacySessionHandle session{
        std::string(R"({"nodes":[)")
        + LegacyNodeText(
            "n_0",
            kLegacyBeginGuid,
            R"("boundaryPorts":[{"id":"keep","type":"bool"}])",
            R"([{"id":"p_1","name":"左值","direction":"input","valueType":"int"}])")
        + R"(],"edges":[]})"};

    const auto source = AcceptedPropertiesOf(session.state);
    const auto entries = MigratedBoundaryEntriesOf(source, "n_0");
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].first, "keep");
    EXPECT_EQ(entries[0].second, "bool");
    EXPECT_EQ(entries[1].first, "p_1");
    EXPECT_EQ(entries[1].second, "int");
}

TEST(GraphDocumentDtoTest, NewShapeDocumentWithoutPartitionNoOps)
{
    // 新形状（无分区）空转：不长键、不丢数据（A7 保守设计——数据不存在
    // 即空转）。
    const LegacySessionHandle session{
        std::string(R"({"documentId":"doc","version":4,"nodes":[)")
        + LegacyNodeText("n_0", kLegacyBeginGuid, "", "") + R"(],"edges":[]})"};

    const auto source = AcceptedPropertiesOf(session.state);
    const auto entries = MigratedBoundaryEntriesOf(source, "n_0");
    EXPECT_TRUE(entries.empty());

    auto source_obj = source.as_object();
    ASSERT_TRUE(source_obj.has_value());
    EXPECT_EQ(
        (*source_obj)[std::string_view("documentId")].as_string().value_or(""),
        "doc");
    EXPECT_EQ(
        (*source_obj)[std::string_view("version")].as_int().value_or(-1),
        4);
}

TEST(GraphDocumentDtoTest, LegacyPartitionKeyToleratedByCaster)
{
    // DTO caster 对未知 "ports" 键静默忽略（field-picking 形态）——旧形状
    // JSON 直接 cast 不炸（迁移读取段之外的直接 cast 路径安全）。
    const auto legacy = Das::Utils::ParseYyjsonFromString(
        std::string_view(
            R"({"nodeId":"legacy_node","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":"{g1}","pluginGuid":"{p}"}},"settings":{},"ports":[{"id":"p_1","name":"x","direction":"input","valueType":"int"}]})"));
    ASSERT_TRUE(legacy.has_value());
    GraphNodeDto node;
    SCOPED_TRACE(
        Das::Utils::SerializeYyjsonValue(*legacy).value_or(std::string{}));
    ASSERT_NO_THROW(node = yyjson::cast<GraphNodeDto>(*legacy));
    EXPECT_EQ(node.node_id, "legacy_node");
}
