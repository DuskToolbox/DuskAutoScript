#include <das/Core/GraphRuntime/AuthoringDocContract.h>
#include <das/Core/GraphRuntime/FormSequenceProjector.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace
{
    using namespace Das::Core::GraphRuntime;
    using namespace Das::Core::GraphRuntime::Contract;
    using namespace Das::Core::GraphRuntime::Dto;

    // -----------------------------------------------------------------------
    // Builders
    // -----------------------------------------------------------------------

    yyjson::value MakeSettings(double v)
    {
        auto payload = Das::Utils::MakeYyjsonObject();
        auto obj = *payload.as_object();
        obj[std::string_view("threshold")] = v;
        return payload;
    }

    ComponentRefDto MakeCompRef(std::string_view guid)
    {
        return ComponentRefDto{
            std::string("componentRef"),
            std::string(guid),
            std::string("{plug-guid}")};
    }

    GraphNodeDto MakeNode(
        std::string_view id,
        std::string_view guid,
        double           settings_val)
    {
        GraphNodeDto node;
        node.node_id = std::string(id);
        node.target.target_kind = "componentRef";
        node.target.component_ref = MakeCompRef(guid);
        node.settings = MakeSettings(settings_val);
        return node;
    }

    GraphEdgeDto MakeSignalEdge(
        std::string_view id,
        std::string_view from,
        std::string_view to)
    {
        GraphEdgeDto edge;
        edge.edge_id = std::string(id);
        edge.source_node_id = std::string(from);
        edge.source_port_id = std::string(FormSequenceProjector::SignalOutPort);
        edge.target_node_id = std::string(to);
        edge.target_port_id = std::string(FormSequenceProjector::SignalInPort);
        edge.edge_type = "signal";
        return edge;
    }

    /// Serialize the authoritative store directly into contract JSON.
    std::string ToJson(const Dto::GraphDocumentDto& doc)
    {
        auto v = Contract::SerializeDocument(doc);
        auto str = Das::Utils::SerializeYyjsonValue(v);
        return str.value_or(std::string{});
    }

    /// Catalog-aware variant of ToJson for the merge projections.
    std::string ToJsonWithCatalog(
        const Dto::GraphDocumentDto&        doc,
        const std::optional<ActionCatalog>& catalog)
    {
        auto v = Contract::SerializeDocument(doc, catalog);
        auto str = Das::Utils::SerializeYyjsonValue(v);
        return str.value_or(std::string{});
    }

    GraphPortDefinitionDto MakePortDef(
        std::string_view id,
        std::string_view type,
        std::string_view label = {})
    {
        GraphPortDefinitionDto port;
        port.port_id = std::string(id);
        port.port_type = std::string(type);
        port.display_label = std::string(label);
        return port;
    }

    GraphEdgeDto MakeDataEdge(
        std::string_view id,
        std::string_view from_node,
        std::string_view from_port,
        std::string_view to_node,
        std::string_view to_port)
    {
        GraphEdgeDto edge;
        edge.edge_id = std::string(id);
        edge.source_node_id = std::string(from_node);
        edge.source_port_id = std::string(from_port);
        edge.target_node_id = std::string(to_node);
        edge.target_port_id = std::string(to_port);
        edge.edge_type = "data";
        return edge;
    }

    GraphNodeDto MakeEntryRefNode(
        std::string_view id,
        int64_t          entry_id,
        int64_t          expected_rev,
        std::string_view fingerprint)
    {
        GraphNodeDto node;
        node.node_id = std::string(id);
        node.target.target_kind = "entryRef";
        node.target.entry_ref = EntryRefDto{
            "entryRef",
            entry_id,
            expected_rev,
            std::string(fingerprint)};
        return node;
    }

    ConfigFieldDescriptor MakeConfigField(
        std::string_view                name,
        std::string_view                type,
        const yyjson::value             default_value = yyjson::value{},
        std::optional<std::string_view> task_guid = std::nullopt)
    {
        ConfigFieldDescriptor field;
        field.name = std::string(name);
        field.type = std::string(type);
        field.default_value = Das::Utils::CloneYyjsonValue(default_value);
        if (task_guid.has_value())
        {
            field.task_guid = std::string(*task_guid);
        }
        return field;
    }

    ActionCatalog MakeDelayCatalog()
    {
        ActionCatalog      catalog;
        ActionCatalogEntry entry;
        entry.action_id = "{g1}";
        entry.label = "das.flow.delay";
        entry.config_fields.push_back(
            MakeConfigField("delay_ms", "int64", yyjson::value(1000)));
        catalog.entries.push_back(entry);
        return catalog;
    }

    // -----------------------------------------------------------------------
    // ToAuthoringDocument — graph mode (no linear tag)
    // -----------------------------------------------------------------------

    TEST(AuthoringDocContractTest, GraphMode_ThinMapsNodesAndDropsPrivateFields)
    {
        GraphDocumentDto doc;
        doc.version = 7;
        doc.fingerprint = "fp";
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.1));
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.2));
        doc.edges.push_back(MakeSignalEdge("e1", "n1", "n2"));

        auto out = ToAuthoringDocument(doc);
        ASSERT_EQ(out.view_kind, ViewKind::Graph);
        ASSERT_TRUE(out.graph.has_value());
        ASSERT_FALSE(out.form_sequence.has_value());

        const auto& view = *out.graph;
        ASSERT_EQ(view.nodes.size(), 2u);
        EXPECT_EQ(view.nodes[0].id, "n1");
        EXPECT_EQ(view.nodes[0].component_guid, "{g1}");
        ASSERT_EQ(view.connections.size(), 1u);
        EXPECT_EQ(view.connections[0].from_node_id, "n1");
        EXPECT_EQ(view.connections[0].to_node_id, "n2");
    }

    TEST(AuthoringDocContractTest, GraphMode_SerializeHasNoEdgeTypeOrEdgeId)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        // A non-signal (data) edge to prove edge_type is stripped regardless.
        GraphEdgeDto edge = MakeSignalEdge("private-edge", "n1", "n2");
        edge.edge_type = "data";
        doc.edges.push_back(edge);

        auto json = ToJson(doc);
        EXPECT_EQ(json.find("edgeType"), std::string::npos);
        EXPECT_EQ(json.find("edgeId"), std::string::npos);
        EXPECT_EQ(json.find("dynamicPorts"), std::string::npos);
        EXPECT_NE(json.find("\"graph\""), std::string::npos);
        EXPECT_NE(json.find("\"connections\""), std::string::npos);
    }

    // copy_string must deep-copy strings nested INSIDE yyjson::value settings
    // subtrees too, not just top-level scalar members — otherwise the returned
    // value dangles once the store is gone (das-yyjson-string-safety skill).
    TEST(
        AuthoringDocContractTest,
        GraphMode_CopyStringDeepCopiesNestedStringSettings)
    {
        GraphDocumentDto doc;
        // settings carries a string payload that would dangle if not copied.
        auto payload = Das::Utils::MakeYyjsonObject();
        auto sobj = *payload.as_object();
        sobj[std::string_view("name")] = std::string("deep-string-payload");
        sobj[std::string_view("threshold")] = 0.5;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes.back().settings = payload;

        // Declared config fields (graph view): the settings overrides ride
        // the declared-field deep copy — undeclared live keys are port
        // value-key channel data and stay off the fields wire.
        ActionCatalog      catalog;
        ActionCatalogEntry entry;
        entry.action_id = "{g1}";
        entry.label = "deep";
        entry.config_fields = {
            MakeConfigField("name", "string"),
            MakeConfigField("threshold", "float")};
        catalog.entries.push_back(entry);

        auto json = ToJsonWithCatalog(doc, catalog);
        // The projected fields[] preserve nested string payloads even though
        // the public wire drops the raw `settings` blob.
        EXPECT_NE(json.find("deep-string-payload"), std::string::npos);
        EXPECT_NE(json.find("\"value\":0.5"), std::string::npos);
    }

    // -----------------------------------------------------------------------
    // Node port projection — drag-link precheck data source (D-09)
    // -----------------------------------------------------------------------

    // Static registry ports (carried on the actionCatalog channel, direction
    // by definition.inputs/outputs list membership) merge onto nodes by
    // componentGuid == actionId; dynamic ports derive direction from their
    // edge role in the document (target side = input, source side = output;
    // no directional evidence → once per direction so the soft precheck
    // never over-restricts).
    TEST(AuthoringDocContractTest, PortProjection_MergesStaticAndDynamic)
    {
        GraphDocumentDto doc;
        auto             n1 = MakeNode("n1", "{g1}", 0.0);
        n1.dynamic_ports.push_back(MakePortDef("dyn_out", "image"));
        doc.nodes.push_back(n1);
        auto n2 = MakeNode("n2", "{g2}", 0.0);
        n2.dynamic_ports.push_back(MakePortDef("dyn_in", "image", "Dyn In"));
        doc.nodes.push_back(n2);
        auto n3 = MakeNode("n3", "{g3}", 0.0);
        n3.dynamic_ports.push_back(MakePortDef("dyn_free", "json"));
        doc.nodes.push_back(n3);
        doc.edges.push_back(
            MakeDataEdge("e1", "n1", "dyn_out", "n2", "dyn_in"));

        ActionCatalog      catalog;
        ActionCatalogEntry entry;
        entry.action_id = "{g1}";
        entry.label = "{g1}";
        entry.ports = {
            GraphNodePort{"cond", "", "bool", "input"},
            GraphNodePort{"next", "", "signal", "output"},
        };
        catalog.entries.push_back(entry);

        const auto  out = ToAuthoringDocument(doc, catalog);
        const auto& nodes = out.graph->nodes;

        // n1: static registry ports first (catalog order), then the dynamic
        // port with the edge-role derived direction ("output": source only).
        ASSERT_EQ(nodes[0].ports.size(), 3u);
        EXPECT_EQ(nodes[0].ports[0].port_id, "cond");
        EXPECT_EQ(nodes[0].ports[0].direction, "input");
        EXPECT_EQ(nodes[0].ports[0].port_type, "bool");
        EXPECT_EQ(nodes[0].ports[0].display_label, "");
        EXPECT_EQ(nodes[0].ports[0].category, "data");
        EXPECT_EQ(nodes[0].ports[1].port_id, "next");
        EXPECT_EQ(nodes[0].ports[1].direction, "output");
        EXPECT_EQ(nodes[0].ports[1].category, "execution");
        EXPECT_EQ(nodes[0].ports[2].port_id, "dyn_out");
        EXPECT_EQ(nodes[0].ports[2].direction, "output");
        EXPECT_EQ(nodes[0].ports[2].port_type, "image");
        EXPECT_EQ(nodes[0].ports[2].category, "data");

        // n2: no catalog entry for {g2}; dynamic port used as edge target
        // only → "input"; display_label thin-maps through.
        ASSERT_EQ(nodes[1].ports.size(), 1u);
        EXPECT_EQ(nodes[1].ports[0].port_id, "dyn_in");
        EXPECT_EQ(nodes[1].ports[0].display_label, "Dyn In");
        EXPECT_EQ(nodes[1].ports[0].direction, "input");
        EXPECT_EQ(nodes[1].ports[0].category, "data");

        // n3: unreferenced dynamic port carries no directional evidence →
        // projected once per direction.
        ASSERT_EQ(nodes[2].ports.size(), 2u);
        EXPECT_EQ(nodes[2].ports[0].port_id, "dyn_free");
        EXPECT_EQ(nodes[2].ports[0].direction, "input");
        EXPECT_EQ(nodes[2].ports[1].port_id, "dyn_free");
        EXPECT_EQ(nodes[2].ports[1].direction, "output");
    }

    // Category derivation is single-point in the Core projection: every port
    // (registry-declared static and document dynamic alike) derives
    // category == "execution" iff its port_type is the "signal" protocol
    // literal, "data" otherwise. The DasFlowControl branch shape (bool
    // condition in, signal true/false out) and the DasMaaPi run shape (object
    // in, signal-comparable out) both anchor the rule on real manifest
    // vocabulary.
    TEST(AuthoringDocContractTest, PortProjection_SignalVersusDataCategory)
    {
        GraphDocumentDto doc;
        // n1: edge-referenced dynamic signal port (target side → input) and
        // an unreferenced dynamic data port (projected both directions).
        auto n1 = MakeNode("n1", "{g1}", 0.0);
        n1.dynamic_ports.push_back(MakePortDef("dyn_signal", "signal"));
        doc.nodes.push_back(n1);
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        doc.edges.push_back(
            MakeDataEdge("e1", "n2", "src", "n1", "dyn_signal"));

        // Static catalog surface mirroring das.flow.branch: bool condition
        // input + signal true/false outputs, plus an object-typed input.
        ActionCatalog      catalog;
        ActionCatalogEntry entry;
        entry.action_id = "{g2}";
        entry.label = "{g2}";
        entry.ports = {
            GraphNodePort{"condition", "", "bool", "input"},
            GraphNodePort{"executionInput", "", "object", "input"},
            GraphNodePort{"true", "", "signal", "output"},
            GraphNodePort{"false", "", "signal", "output"},
        };
        catalog.entries.push_back(entry);

        const auto  out = ToAuthoringDocument(doc, catalog);
        const auto& nodes = out.graph->nodes;

        // Static: signal ports are execution, bool/object ports are data.
        ASSERT_EQ(nodes[1].ports.size(), 4u);
        EXPECT_EQ(nodes[1].ports[0].port_type, "bool");
        EXPECT_EQ(nodes[1].ports[0].category, "data");
        EXPECT_EQ(nodes[1].ports[1].port_type, "object");
        EXPECT_EQ(nodes[1].ports[1].category, "data");
        EXPECT_EQ(nodes[1].ports[2].port_type, "signal");
        EXPECT_EQ(nodes[1].ports[2].category, "execution");
        EXPECT_EQ(nodes[1].ports[3].port_type, "signal");
        EXPECT_EQ(nodes[1].ports[3].category, "execution");

        // Dynamic: signal port derives execution regardless of the
        // edge-derived direction.
        ASSERT_EQ(nodes[0].ports.size(), 1u);
        EXPECT_EQ(nodes[0].ports[0].port_type, "signal");
        EXPECT_EQ(nodes[0].ports[0].direction, "input");
        EXPECT_EQ(nodes[0].ports[0].category, "execution");

        // Dynamic non-signal port without directional evidence projects once
        // per direction, both carrying the data category.
        auto n3 = MakeNode("n3", "{g3}", 0.0);
        n3.dynamic_ports.push_back(MakePortDef("dyn_data", "image"));
        doc.nodes.push_back(n3);
        const auto  out2 = ToAuthoringDocument(doc, catalog);
        const auto& ports3 = out2.graph->nodes[2].ports;
        ASSERT_EQ(ports3.size(), 2u);
        EXPECT_EQ(ports3[0].direction, "input");
        EXPECT_EQ(ports3[0].category, "data");
        EXPECT_EQ(ports3[1].direction, "output");
        EXPECT_EQ(ports3[1].category, "data");
    }

    // Document-level graph_inputs/graph_outputs are graph boundary data
    // without an owning node; node-to-boundary authoring does not exist
    // (connectGraphPorts is strictly node-to-node), so they never surface
    // as node ports.
    TEST(AuthoringDocContractTest, PortProjection_ExcludesDocumentLevelPorts)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.graph_inputs.push_back(MakePortDef("in_param", "image"));
        doc.graph_outputs.push_back(MakePortDef("out_result", "image"));

        const auto out = ToAuthoringDocument(doc);
        EXPECT_TRUE(out.graph->nodes[0].ports.empty());
    }

    // Wire element key set is exactly the five locked keys: the four
    // projection keys plus the derived category (T-109-08 shape + category).
    TEST(AuthoringDocContractTest, PortProjection_WireEmitsExactlyFiveKeys)
    {
        GraphDocumentDto doc;
        auto             n1 = MakeNode("n1", "{g1}", 0.0);
        n1.dynamic_ports.push_back(MakePortDef("dyn_out", "image", "Out"));
        doc.nodes.push_back(n1);
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        doc.edges.push_back(MakeDataEdge("e1", "n1", "dyn_out", "n2", "sink"));

        ActionCatalog      catalog;
        ActionCatalogEntry entry;
        entry.action_id = "{g2}";
        entry.label = "{g2}";
        entry.ports = {GraphNodePort{"sink", "", "bool", "input"}};
        catalog.entries.push_back(entry);

        const auto value = SerializeDocument(doc, catalog);
        const auto root = value.as_object();
        ASSERT_TRUE(root.has_value());
        const auto graph = (*root)[std::string_view("graph")].as_object();
        ASSERT_TRUE(graph.has_value());
        const auto nodes = (*graph)[std::string_view("nodes")].as_array();
        ASSERT_TRUE(nodes.has_value());
        ASSERT_EQ(nodes->size(), 2u);

        const auto ports_of = [&](std::size_t node_index)
        {
            const auto node_obj = (*nodes)[node_index].as_object();
            if (!node_obj || !node_obj->contains(std::string_view("ports")))
            {
                return yyjson::value{};
            }
            return Das::Utils::CloneYyjsonValue(
                (*node_obj)[std::string_view("ports")]);
        };

        // Dynamic port with an edge role: single entry, exact five keys with
        // the derived category value ("image" is not the signal literal).
        const auto dyn_ports = ports_of(0);
        ASSERT_TRUE(dyn_ports.is_array());
        ASSERT_EQ(dyn_ports.as_array()->size(), 1u);
        const auto dyn_port_obj = (*dyn_ports.as_array())[0].as_object();
        ASSERT_TRUE(dyn_port_obj.has_value());
        EXPECT_EQ(dyn_port_obj->size(), 5u);
        EXPECT_TRUE(dyn_port_obj->contains(std::string_view("portId")));
        EXPECT_TRUE(dyn_port_obj->contains(std::string_view("displayLabel")));
        EXPECT_TRUE(dyn_port_obj->contains(std::string_view("portType")));
        EXPECT_TRUE(dyn_port_obj->contains(std::string_view("direction")));
        EXPECT_TRUE(dyn_port_obj->contains(std::string_view("category")));
        auto dyn_label =
            (*dyn_port_obj)[std::string_view("displayLabel")].as_string();
        ASSERT_TRUE(dyn_label.has_value());
        EXPECT_EQ(*dyn_label, "Out");
        auto dyn_category =
            (*dyn_port_obj)[std::string_view("category")].as_string();
        ASSERT_TRUE(dyn_category.has_value());
        EXPECT_EQ(*dyn_category, "data");

        // Static registry port lands on the wire node with its declared
        // direction and the derived category.
        const auto static_ports = ports_of(1);
        ASSERT_TRUE(static_ports.is_array());
        ASSERT_EQ(static_ports.as_array()->size(), 1u);
        const auto static_port_obj = (*static_ports.as_array())[0].as_object();
        ASSERT_TRUE(static_port_obj.has_value());
        EXPECT_EQ(static_port_obj->size(), 5u);
        auto static_dir =
            (*static_port_obj)[std::string_view("direction")].as_string();
        ASSERT_TRUE(static_dir.has_value());
        EXPECT_EQ(*static_dir, "input");
        auto static_category =
            (*static_port_obj)[std::string_view("category")].as_string();
        ASSERT_TRUE(static_category.has_value());
        EXPECT_EQ(*static_category, "data");
    }

    // A node without any port still emits the "ports" key as an empty array
    // (key presence is stable wire; clients keep their missing-key
    // tolerance).
    TEST(AuthoringDocContractTest, PortProjection_EmptyNodeEmitsEmptyPortsKey)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));

        const auto value = SerializeDocument(doc);
        const auto root = value.as_object();
        ASSERT_TRUE(root.has_value());
        const auto graph = (*root)[std::string_view("graph")].as_object();
        ASSERT_TRUE(graph.has_value());
        const auto nodes = (*graph)[std::string_view("nodes")].as_array();
        ASSERT_TRUE(nodes.has_value());
        ASSERT_EQ(nodes->size(), 1u);
        const auto node_obj = (*nodes)[0].as_object();
        ASSERT_TRUE(node_obj.has_value());
        ASSERT_TRUE(node_obj->contains(std::string_view("ports")));
        const auto ports = (*node_obj)[std::string_view("ports")].as_array();
        ASSERT_TRUE(ports.has_value());
        EXPECT_EQ(ports->size(), 0u);
    }

    // -----------------------------------------------------------------------
    // Node display-name projection — priority chain: store user label >
    // catalog entry merge (componentGuid == actionId, entry label carries
    // the manifest kind default name) > (client-side) node id. description/
    // icon merge from the matching catalog entry only.
    // -----------------------------------------------------------------------

    TEST(AuthoringDocContractTest, LabelProjection_CatalogDefaultNameFallback)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0)); // store label unset

        ActionCatalog      catalog;
        ActionCatalogEntry entry;
        entry.action_id = "{g1}";
        entry.label = "das.flow.delay";
        catalog.entries.push_back(entry);

        const auto out = ToAuthoringDocument(doc, catalog);
        ASSERT_EQ(out.graph->nodes.size(), 1u);
        // The unnamed node follows the catalog default name (the manifest
        // kind) so catalog renames propagate to never-renamed nodes.
        EXPECT_EQ(out.graph->nodes[0].label, "das.flow.delay");

        const auto json = ToJsonWithCatalog(doc, catalog);
        EXPECT_NE(json.find("\"label\":\"das.flow.delay\""), std::string::npos);
    }

    TEST(AuthoringDocContractTest, LabelProjection_StoreLabelOverridesCatalog)
    {
        GraphDocumentDto doc;
        auto             n1 = MakeNode("n1", "{g1}", 0.0);
        n1.label = "我的延迟";
        doc.nodes.push_back(n1);

        ActionCatalog      catalog;
        ActionCatalogEntry entry;
        entry.action_id = "{g1}";
        entry.label = "das.flow.delay";
        catalog.entries.push_back(entry);

        const auto out = ToAuthoringDocument(doc, catalog);
        ASSERT_EQ(out.graph->nodes.size(), 1u);
        // The store user label wins over the catalog default name.
        EXPECT_EQ(out.graph->nodes[0].label, "我的延迟");

        // Wire-level: the projected node carries the user label (the
        // catalog section keeps its own entry label — inspect the node
        // object, not the whole document text).
        const auto value = SerializeDocument(doc, catalog);
        const auto root = value.as_object();
        ASSERT_TRUE(root.has_value());
        const auto graph = (*root)[std::string_view("graph")].as_object();
        ASSERT_TRUE(graph.has_value());
        const auto nodes = (*graph)[std::string_view("nodes")].as_array();
        ASSERT_TRUE(nodes.has_value());
        ASSERT_EQ(nodes->size(), 1u);
        const auto node_obj = (*nodes)[0].as_object();
        ASSERT_TRUE(node_obj.has_value());
        ASSERT_TRUE(node_obj->contains(std::string_view("label")));
        auto wire_label = (*node_obj)[std::string_view("label")].as_string();
        ASSERT_TRUE(wire_label.has_value());
        EXPECT_EQ(*wire_label, "我的延迟");
    }

    TEST(AuthoringDocContractTest, LabelProjection_NoSourceOmitsKey)
    {
        GraphDocumentDto doc;
        // componentRef node with no catalog match + no store label...
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        // ...and an entryRef node (no catalog match by construction).
        doc.nodes.push_back(MakeEntryRefNode("sub", 42, 3, "fp-sub"));

        ActionCatalog      catalog;
        ActionCatalogEntry entry;
        entry.action_id = "{other}";
        entry.label = "unrelated";
        catalog.entries.push_back(entry);

        const auto value = SerializeDocument(doc, catalog);
        const auto root = value.as_object();
        ASSERT_TRUE(root.has_value());
        const auto graph = (*root)[std::string_view("graph")].as_object();
        ASSERT_TRUE(graph.has_value());
        const auto nodes = (*graph)[std::string_view("nodes")].as_array();
        ASSERT_TRUE(nodes.has_value());
        ASSERT_EQ(nodes->size(), 2u);
        for (const auto& node : *nodes)
        {
            auto node_obj = node.as_object();
            ASSERT_TRUE(node_obj.has_value());
            // No empty-string label on the wire — the client falls back to
            // the node id itself.
            EXPECT_FALSE(node_obj->contains(std::string_view("label")));
        }

        const auto out = ToAuthoringDocument(doc, catalog);
        EXPECT_TRUE(out.graph->nodes[0].label.empty());
        EXPECT_TRUE(out.graph->nodes[1].label.empty());
    }

    TEST(AuthoringDocContractTest, LabelProjection_DescriptionIconFromCatalog)
    {
        GraphDocumentDto doc;
        auto             n1 = MakeNode("n1", "{g1}", 0.0);
        n1.label = "用户名"; // store label engaged — metadata still merges
        doc.nodes.push_back(n1);
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0)); // no catalog match

        ActionCatalog      catalog;
        ActionCatalogEntry entry;
        entry.action_id = "{g1}";
        entry.label = "das.flow.delay";
        entry.description = "Delays the signal chain";
        entry.icon = "https://example.com/delay.png";
        catalog.entries.push_back(entry);

        const auto out = ToAuthoringDocument(doc, catalog);
        ASSERT_EQ(out.graph->nodes.size(), 2u);
        ASSERT_TRUE(out.graph->nodes[0].description.has_value());
        EXPECT_EQ(*out.graph->nodes[0].description, "Delays the signal chain");
        ASSERT_TRUE(out.graph->nodes[0].icon.has_value());
        EXPECT_EQ(*out.graph->nodes[0].icon, "https://example.com/delay.png");
        EXPECT_FALSE(out.graph->nodes[1].description.has_value());
        EXPECT_FALSE(out.graph->nodes[1].icon.has_value());

        const auto json = ToJsonWithCatalog(doc, catalog);
        EXPECT_NE(
            json.find("\"description\":\"Delays the signal chain\""),
            std::string::npos);
        EXPECT_NE(
            json.find("\"icon\":\"https://example.com/delay.png\""),
            std::string::npos);
    }

    // -----------------------------------------------------------------------
    // Config-declared field projection: manifest config entries merge into
    // node fields[] so a freshly added node is immediately editable. Value
    // priority: settings override > declared default > null; undeclared
    // settings keys keep the existing live-value projection, appended after
    // the declared fields.
    // -----------------------------------------------------------------------

    TEST(
        AuthoringDocContractTest,
        ConfigProjection_DeclaredDefaultsVisibleWithoutSettings)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0)); // threshold only

        const auto catalog = MakeDelayCatalog();
        const auto out = ToAuthoringDocument(doc, catalog);
        ASSERT_EQ(out.graph->nodes.size(), 1u);
        const auto& fields = out.graph->nodes[0].fields;
        // Declared delay_ms only — undeclared live settings keys (port
        // binding defaults like threshold) ride the port "value" key channel
        // instead of fields (one data source, one editing control).
        ASSERT_EQ(fields.size(), 1u);
        EXPECT_EQ(fields[0].name, "delay_ms");

        const auto& delay_field = fields[0];
        EXPECT_EQ(delay_field.name, "delay_ms");
        EXPECT_EQ(delay_field.label, "delay_ms");
        EXPECT_EQ(delay_field.value_type, 0);
        EXPECT_EQ(delay_field.input_type, 0);
        ASSERT_TRUE(delay_field.value.as_int().has_value());
        EXPECT_EQ(*delay_field.value.as_int(), 1000);

        // The wire carries the declared default so the detail panel can
        // render and edit it with zero setValue round-trips.
        const auto json = ToJsonWithCatalog(doc, catalog);
        EXPECT_NE(json.find("\"name\":\"delay_ms\""), std::string::npos);
        EXPECT_NE(json.find("\"value\":1000"), std::string::npos);
    }

    TEST(
        AuthoringDocContractTest,
        ConfigProjection_SettingsValueOverridesDeclaredDefault)
    {
        GraphDocumentDto doc;
        auto             n1 = MakeNode("n1", "{g1}", 0.0);
        auto             payload = Das::Utils::MakeYyjsonObject();
        (*payload.as_object())[std::string_view("delay_ms")] = 500;
        n1.settings = payload;
        doc.nodes.push_back(n1);

        const auto out = ToAuthoringDocument(doc, MakeDelayCatalog());
        ASSERT_EQ(out.graph->nodes.size(), 1u);
        const auto& fields = out.graph->nodes[0].fields;
        // delay_ms only — the payload replaced the MakeNode threshold key.
        ASSERT_EQ(fields.size(), 1u);
        EXPECT_EQ(fields[0].name, "delay_ms");
        ASSERT_TRUE(fields[0].value.as_int().has_value());
        EXPECT_EQ(*fields[0].value.as_int(), 500);

        // A settings null clears to the declared default (null reads as
        // "no override", matching the live-value projection semantics).
        auto null_payload = Das::Utils::MakeYyjsonObject();
        (*null_payload.as_object())[std::string_view("delay_ms")] = nullptr;
        doc.nodes[0].settings = null_payload;
        const auto  out2 = ToAuthoringDocument(doc, MakeDelayCatalog());
        const auto& fields2 = out2.graph->nodes[0].fields;
        ASSERT_EQ(fields2.size(), 1u);
        EXPECT_EQ(fields2[0].name, "delay_ms");
        ASSERT_TRUE(fields2[0].value.as_int().has_value());
        EXPECT_EQ(*fields2[0].value.as_int(), 1000);
    }

    TEST(
        AuthoringDocContractTest,
        ConfigProjection_UndeclaredSettingsKeysAppendAfterDeclared)
    {
        GraphDocumentDto doc;
        auto             n1 = MakeNode("n1", "{g1}", 0.0);
        auto             payload = Das::Utils::MakeYyjsonObject();
        (*payload.as_object())[std::string_view("delay_ms")] = 250;
        (*payload.as_object())[std::string_view("custom_note")] =
            std::string("keep me");
        n1.settings = payload;
        doc.nodes.push_back(n1);

        const auto  out = ToAuthoringDocument(doc, MakeDelayCatalog());
        const auto& fields = out.graph->nodes[0].fields;
        // Graph view carries declared config fields only — undeclared live
        // settings keys ride the port "value" key channel, never fields
        // (one data source, one editing control).
        ASSERT_EQ(fields.size(), 1u);
        EXPECT_EQ(fields[0].name, "delay_ms");
        ASSERT_TRUE(fields[0].value.as_int().has_value());
        EXPECT_EQ(*fields[0].value.as_int(), 250);
        bool custom_note_found = false;
        for (const auto& field : fields)
        {
            if (field.name == "custom_note")
            {
                custom_note_found = true;
            }
        }
        EXPECT_FALSE(custom_note_found);
    }

    // Manifest config type → FieldDescriptor typing pair + wire type word:
    // inputType stays within the official 0-8 vocabulary — taskRepositoryRef
    // maps conservatively like unknown types and its typed dispatch rides on
    // the carried config type word instead.
    TEST(AuthoringDocContractTest, ConfigProjection_TypeMappingMatrix)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));

        ActionCatalog      catalog;
        ActionCatalogEntry entry;
        entry.action_id = "{g1}";
        entry.label = "matrix";
        entry.config_fields.push_back(MakeConfigField("f_int", "int"));
        entry.config_fields.push_back(MakeConfigField("f_int64", "int64"));
        entry.config_fields.push_back(MakeConfigField("f_uint", "uint"));
        entry.config_fields.push_back(MakeConfigField("f_float", "float"));
        entry.config_fields.push_back(MakeConfigField("f_double", "double"));
        entry.config_fields.push_back(MakeConfigField("f_string", "string"));
        entry.config_fields.push_back(MakeConfigField("f_bool", "bool"));
        entry.config_fields.push_back(MakeConfigField("f_object", "object"));
        entry.config_fields.push_back(
            MakeConfigField("f_array", "array<string>"));
        entry.config_fields.push_back(
            MakeConfigField("f_ref", "taskRepositoryRef"));
        entry.config_fields.push_back(MakeConfigField("f_weird", "quux"));
        catalog.entries.push_back(entry);

        const auto out = ToAuthoringDocument(doc, catalog);
        const std::vector<std::pair<int32_t, int32_t>> expected{
            {0, 0},   // int
            {0, 0},   // int64
            {0, 0},   // uint
            {2, 0},   // float
            {2, 0},   // double
            {4, 0},   // string
            {8, 0},   // bool
            {16, 0},  // object
            {32, 0},  // array<string>
            {16, 0},  // taskRepositoryRef → conservative object (typed
                      // dispatch rides on the config type word, NOT inputType)
            {16, 0}}; // unknown → conservative object
        const auto& fields = out.graph->nodes[0].fields;
        // One declared field per matrix row; the live "threshold" rides the
        // port value-key channel instead of fields (graph view).
        ASSERT_EQ(fields.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index)
        {
            EXPECT_EQ(fields[index].value_type, expected[index].first)
                << fields[index].name;
            EXPECT_EQ(fields[index].input_type, expected[index].second)
                << fields[index].name;
        }
        // Fields without a default and without a settings value project a
        // null value (JsonValue allows null — the picker starts empty).
        EXPECT_TRUE(fields[9].value.is_null());

        // Declared fields carry their manifest config type word.
        ASSERT_TRUE(fields[0].type_word.has_value());
        EXPECT_EQ(*fields[0].type_word, "int");
        ASSERT_TRUE(fields[9].type_word.has_value());
        EXPECT_EQ(*fields[9].type_word, "taskRepositoryRef");
        ASSERT_TRUE(fields[10].type_word.has_value());
        EXPECT_EQ(*fields[10].type_word, "quux");
    }

    // The wire field descriptor emits the optional "type" key only for
    // config-declared fields (engaged-only emission, copy_string).
    TEST(AuthoringDocContractTest, ConfigProjection_WireEmitsTypeWord)
    {
        GraphDocumentDto doc;
        auto             payload = Das::Utils::MakeYyjsonObject();
        (*payload.as_object())[std::string_view("f_ref")] = 7;
        (*payload.as_object())[std::string_view("custom_note")] =
            std::string("keep me");
        auto node = MakeNode("n1", "{g1}", 0.0);
        node.settings = payload;
        doc.nodes.push_back(node);

        ActionCatalog      catalog;
        ActionCatalogEntry entry;
        entry.action_id = "{g1}";
        entry.label = "typed";
        entry.config_fields.push_back(
            MakeConfigField("f_ref", "taskRepositoryRef"));
        catalog.entries.push_back(entry);

        const auto wire = SerializeDocument(doc, catalog);
        auto       wire_obj = wire.as_object();
        ASSERT_TRUE(wire_obj.has_value());
        auto graph = (*wire_obj)[std::string_view("graph")].as_object();
        ASSERT_TRUE(graph.has_value());
        auto nodes = (*graph)[std::string_view("nodes")].as_array();
        ASSERT_TRUE(nodes.has_value());
        ASSERT_EQ(nodes->size(), 1u);
        auto node_obj = (*nodes)[0].as_object();
        ASSERT_TRUE(node_obj.has_value());
        auto fields = (*node_obj)[std::string_view("fields")].as_array();
        ASSERT_TRUE(fields.has_value());
        // Declared f_ref only — the undeclared live "custom_note" rides the
        // port value-key channel instead of fields (graph view).
        ASSERT_EQ(fields->size(), 1u);

        auto declared = (*fields)[0].as_object();
        ASSERT_TRUE(declared.has_value());
        ASSERT_TRUE(declared->contains(std::string_view("type")));
        EXPECT_EQ(
            (*declared)[std::string_view("type")].as_string().value_or(""),
            "taskRepositoryRef");
        // Plain entryId integer value rides the wire as-is.
        EXPECT_EQ(
            (*declared)[std::string_view("value")].as_int().value_or(-1),
            7);
    }

    // The declared target task type (taskGuid) rides the declaration→
    // projection→wire channel engaged-only: a config declaration naming a
    // task type surfaces it on the projected field and the wire descriptor;
    // declarations without one carry no key (never an explicit null).
    TEST(AuthoringDocContractTest, ConfigProjection_TaskGuidChannel)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));

        ActionCatalog      catalog;
        ActionCatalogEntry entry;
        entry.action_id = "{g1}";
        entry.label = "typed";
        entry.config_fields.push_back(MakeConfigField(
            "repositoryRef",
            "taskRepositoryRef",
            yyjson::value{},
            "69F20001-0000-4000-8000-000000000001"));
        entry.config_fields.push_back(
            MakeConfigField("runtimeInputs", "object"));
        catalog.entries.push_back(entry);

        const auto  out = ToAuthoringDocument(doc, catalog);
        const auto& fields = out.graph->nodes[0].fields;
        ASSERT_EQ(fields.size(), 2u);

        ASSERT_TRUE(fields[0].task_guid.has_value());
        EXPECT_EQ(*fields[0].task_guid, "69F20001-0000-4000-8000-000000000001");
        EXPECT_FALSE(fields[1].task_guid.has_value());

        const auto wire = SerializeDocument(doc, catalog);
        auto       wire_obj = wire.as_object();
        ASSERT_TRUE(wire_obj.has_value());
        auto graph = (*wire_obj)[std::string_view("graph")].as_object();
        ASSERT_TRUE(graph.has_value());
        auto nodes = (*graph)[std::string_view("nodes")].as_array();
        ASSERT_TRUE(nodes.has_value());
        auto node_obj = (*nodes)[0].as_object();
        ASSERT_TRUE(node_obj.has_value());
        auto wire_fields = (*node_obj)[std::string_view("fields")].as_array();
        ASSERT_TRUE(wire_fields.has_value());
        ASSERT_EQ(wire_fields->size(), 2u);

        auto ref_field = (*wire_fields)[0].as_object();
        ASSERT_TRUE(ref_field.has_value());
        ASSERT_TRUE(ref_field->contains(std::string_view("taskGuid")));
        EXPECT_EQ(
            (*ref_field)[std::string_view("taskGuid")].as_string().value_or(""),
            "69F20001-0000-4000-8000-000000000001");
        auto inputs_field = (*wire_fields)[1].as_object();
        ASSERT_TRUE(inputs_field.has_value());
        EXPECT_FALSE(inputs_field->contains(std::string_view("taskGuid")));
    }

    // The formSequence projection merges config declarations the same way
    // (items of a linear document reference components too).
    TEST(AuthoringDocContractTest, ConfigProjection_FormSequenceItemsMerge)
    {
        FormSequenceDto seq;
        seq.items.push_back(
            {.item_id = "a",
             .target =
                 {.target_kind = "componentRef",
                  .component_ref = MakeCompRef("{g1}")},
             .settings = Das::Utils::MakeYyjsonObject()});
        auto doc = FormSequenceProjector::Project(seq);
        doc.tags.emplace_back(LINEAR_TAG);

        const auto out = ToAuthoringDocument(doc, MakeDelayCatalog());
        ASSERT_TRUE(out.form_sequence.has_value());
        ASSERT_EQ(out.form_sequence->items.size(), 1u);
        const auto& fields = out.form_sequence->items[0].fields;
        ASSERT_EQ(fields.size(), 1u);
        EXPECT_EQ(fields[0].name, "delay_ms");
        ASSERT_TRUE(fields[0].value.as_int().has_value());
        EXPECT_EQ(*fields[0].value.as_int(), 1000);
    }

    // -----------------------------------------------------------------------
    // ToAuthoringDocument — formSequence mode (linear tag)
    // -----------------------------------------------------------------------

    TEST(AuthoringDocContractTest, FormSequenceMode_ReverseProjectsLinearChain)
    {
        // Build via the projector so the structure matches production.
        FormSequenceDto seq;
        seq.version = 3;
        seq.fingerprint = "seq-fp";
        seq.items.push_back(
            {.item_id = "a",
             .target =
                 {.target_kind = "componentRef",
                  .component_ref = MakeCompRef("{ga}")},
             .settings = MakeSettings(1.0)});
        seq.items.push_back(
            {.item_id = "b",
             .target =
                 {.target_kind = "componentRef",
                  .component_ref = MakeCompRef("{gb}")},
             .settings = MakeSettings(2.0)});
        seq.items.push_back(
            {.item_id = "c",
             .target =
                 {.target_kind = "componentRef",
                  .component_ref = MakeCompRef("{gc}")},
             .settings = MakeSettings(3.0)});

        auto doc = FormSequenceProjector::Project(seq);
        doc.tags.emplace_back(LINEAR_TAG); // mark linear state

        auto out = ToAuthoringDocument(doc);
        ASSERT_EQ(out.view_kind, ViewKind::FormSequence);
        ASSERT_TRUE(out.form_sequence.has_value());
        ASSERT_FALSE(out.graph.has_value());

        const auto& items = out.form_sequence->items;
        ASSERT_EQ(items.size(), 3u);
        EXPECT_EQ(items[0].id, "a");
        EXPECT_EQ(items[0].component_guid, "{ga}");
        EXPECT_EQ(items[1].id, "b");
        EXPECT_EQ(items[2].id, "c");
    }

    TEST(AuthoringDocContractTest, FormSequenceMode_SerializeShape)
    {
        GraphDocumentDto doc;
        doc.version = 1;
        doc.nodes.push_back(MakeNode("x", "{gx}", 9.0));
        doc.tags.emplace_back(LINEAR_TAG);

        auto json = ToJson(doc);
        EXPECT_NE(
            json.find("\"viewKind\":\"formSequence\""),
            std::string::npos);
        EXPECT_NE(json.find("\"formSequence\""), std::string::npos);
        EXPECT_NE(json.find("\"items\":["), std::string::npos);
        // Private fields must not leak even in formSequence mode.
        EXPECT_EQ(json.find("edgeType"), std::string::npos);
    }

    // Comprehensive zero-leak guard: the contract output must NEVER carry any
    // runtime-private field, regardless of mode or edge type. Covers edgeType,
    // edgeId and dynamicPorts keys, and pins the contract GraphConnection to
    // exactly its four endpoint keys — the signal/data edge-type tokens only
    // exist on the internal GraphEdgeDto and have no slot on the connection
    // shape. (The projected port category legitimately reuses the
    // "signal"/"data" vocabulary, so token-level substring checks are
    // structural here, not textual.)
    TEST(AuthoringDocContractTest, BothModes_NeverLeakRuntimePrivateFields)
    {
        // graph mode: a node with dynamic_ports + a data edge + a signal edge.
        // The port type deliberately avoids the "data"/"signal" tokens so the
        // assertions below isolate the connection shape from the port
        // projection shape.
        GraphDocumentDto g;
        auto&            n1 = g.nodes.emplace_back(MakeNode("n1", "{g1}", 0.0));
        n1.dynamic_ports.push_back({.port_id = "dyn", .port_type = "image"});
        g.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        GraphEdgeDto data_edge = MakeSignalEdge("e-data", "n1", "n2");
        data_edge.edge_type = "data";
        g.edges.push_back(data_edge);
        const GraphEdgeDto sig = MakeSignalEdge("e-sig", "n1", "n2");
        g.edges.push_back(sig);

        const auto graph_json = ToJson(g);
        EXPECT_EQ(graph_json.find("edgeType"), std::string::npos);
        EXPECT_EQ(graph_json.find("edgeId"), std::string::npos);
        EXPECT_EQ(graph_json.find("dynamicPorts"), std::string::npos);

        // Structural edge-type leak guard: every projected connection carries
        // exactly the four endpoint keys — no edgeType/edgeId slot exists.
        const auto parsed = yyjson::read(graph_json);
        auto       root = parsed.as_object();
        ASSERT_TRUE(root.has_value());
        ASSERT_TRUE(root->contains(std::string_view("graph")));
        auto graph = (*root)[std::string_view("graph")].as_object();
        ASSERT_TRUE(graph.has_value());
        auto connections = (*graph)[std::string_view("connections")].as_array();
        ASSERT_TRUE(connections.has_value());
        ASSERT_EQ(connections->size(), 2u);
        for (const auto& connection : *connections)
        {
            auto connection_obj = connection.as_object();
            ASSERT_TRUE(connection_obj.has_value());
            EXPECT_EQ(connection_obj->size(), 4u);
            EXPECT_TRUE(
                connection_obj->contains(std::string_view("fromNodeId")));
            EXPECT_TRUE(
                connection_obj->contains(std::string_view("fromPortId")));
            EXPECT_TRUE(connection_obj->contains(std::string_view("toNodeId")));
            EXPECT_TRUE(connection_obj->contains(std::string_view("toPortId")));
        }

        // formSequence mode: project a sequence (signal edges auto-generated) +
        // ensure the same fields are absent in the projected view.
        FormSequenceDto seq;
        seq.items.push_back(
            {.item_id = "a",
             .target =
                 {.target_kind = "componentRef",
                  .component_ref = MakeCompRef("{ga}")},
             .settings = MakeSettings(1.0)});
        seq.items.push_back(
            {.item_id = "b",
             .target =
                 {.target_kind = "componentRef",
                  .component_ref = MakeCompRef("{gb}")},
             .settings = MakeSettings(2.0)});
        auto fdoc = FormSequenceProjector::Project(seq);
        fdoc.tags.emplace_back(LINEAR_TAG);
        const auto fs_json = ToJson(fdoc);
        EXPECT_EQ(fs_json.find("edgeType"), std::string::npos);
        EXPECT_EQ(fs_json.find("edgeId"), std::string::npos);
        EXPECT_EQ(fs_json.find("dynamicPorts"), std::string::npos);
        EXPECT_EQ(fs_json.find("\"signal\""), std::string::npos);
    }

    // Catalog groups' actionIds are built element by element through the
    // (string, copy_string) primitive path: an action id carrying quotes,
    // backslashes or control characters must survive serialization as
    // properly escaped JSON instead of corrupting the document (the old
    // hand-built JSON literal path threw read_error and took the whole
    // Get/Apply pipeline down with it).
    TEST(AuthoringDocContractTest, ActionCatalogGroupsWithHostileIdsSerialize)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));

        ActionCatalog      catalog;
        ActionCatalogEntry entry;
        entry.action_id = "{g1}";
        entry.label = "das.flow.delay";
        catalog.entries.push_back(entry);
        ActionCatalogGroup group;
        group.id = "g1";
        group.label = "Flow";
        group.action_ids = {
            "{g1}",
            "hostile\"quote\\and\\slash",
            "with\nnewline",
        };
        catalog.groups.push_back(group);

        // Must not throw; the escaped id round-trips through a re-parse.
        const auto json = ToJsonWithCatalog(doc, catalog);
        EXPECT_NE(json.find("\"actionIds\""), std::string::npos);
        const auto parsed = yyjson::read(json);
        auto       root = parsed.as_object();
        ASSERT_TRUE(root.has_value());
        ASSERT_TRUE(root->contains(std::string_view("actionCatalog")));
        auto action_catalog =
            (*root)[std::string_view("actionCatalog")].as_object();
        ASSERT_TRUE(action_catalog.has_value());
        ASSERT_TRUE(action_catalog->contains(std::string_view("groups")));
        auto groups = (*action_catalog)[std::string_view("groups")].as_array();
        ASSERT_TRUE(groups.has_value());
        ASSERT_EQ(groups->size(), 1u);
        auto group_obj = (*groups)[0].as_object();
        ASSERT_TRUE(group_obj.has_value());
        auto ids = (*group_obj)[std::string_view("actionIds")].as_array();
        ASSERT_TRUE(ids.has_value());
        ASSERT_EQ(ids->size(), 3u);
        auto hostile = (*ids)[1].as_string();
        ASSERT_TRUE(hostile.has_value());
        EXPECT_EQ(*hostile, std::string_view("hostile\"quote\\and\\slash"));
        auto newline_id = (*ids)[2].as_string();
        ASSERT_TRUE(newline_id.has_value());
        EXPECT_EQ(*newline_id, std::string_view("with\nnewline"));
    }

    // -----------------------------------------------------------------------
    // DAS-81 compliance: componentGuid (no `type`), entryRef emit, fields[]
    // -----------------------------------------------------------------------

    TEST(AuthoringDocContractTest, GraphMode_EmitsComponentGuidAndNotType)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        auto json = ToJson(doc);
        // DAS-81 rename: componentGuid present, legacy `type` absent.
        EXPECT_NE(json.find("\"componentGuid\":\"{g1}\""), std::string::npos);
        EXPECT_EQ(json.find("\"type\":\"{g1}\""), std::string::npos);
    }

    TEST(AuthoringDocContractTest, GraphMode_EntryRefCarriesOnlyEntryId)
    {
        // D-84-39: public entryRef is exactly {entryId}; expectedRevision and
        // sourceFingerprint remain internal to GraphDocumentDto.
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeEntryRefNode("sub", 42, 3, "fp-sub"));
        auto out = ToAuthoringDocument(doc);
        ASSERT_TRUE(out.graph.has_value());
        ASSERT_EQ(out.graph->nodes.size(), 1u);
        const auto& n = out.graph->nodes[0];
        EXPECT_EQ(n.target_kind, "entryRef");
        ASSERT_TRUE(n.entry_ref.has_value());
        EXPECT_EQ(n.entry_ref->entry_id, 42);

        auto json = ToJson(doc);
        EXPECT_NE(json.find("\"entryId\":42"), std::string::npos);
        EXPECT_EQ(json.find("\"expectedRevision\""), std::string::npos);
        EXPECT_EQ(json.find("\"sourceFingerprint\""), std::string::npos);
    }

    TEST(AuthoringDocContractTest, GraphMode_LiveSettingsKeysNotInFields)
    {
        GraphDocumentDto doc;
        auto& n = doc.nodes.emplace_back(MakeNode("n1", "{g1}", 0.0));
        auto  payload = Das::Utils::MakeYyjsonObject();
        auto  sobj = *payload.as_object();
        sobj[std::string_view("threshold")] = 0.5; // float -> valueType 2
        sobj[std::string_view("count")] = 3;       // int -> valueType 0
        sobj[std::string_view("name")] = std::string("x"); // string -> 4
        sobj[std::string_view("enabled")] = true;          // bool -> 8
        n.settings = payload;

        auto out = ToAuthoringDocument(doc);
        ASSERT_TRUE(out.graph.has_value());
        ASSERT_EQ(out.graph->nodes.size(), 1u);
        // Graph view synthesizes NO fields from undeclared live settings
        // keys — the values ride the port "value" key channel (keys with no
        // declared port surface are ghost keys and stay invisible; one data
        // source, one editing control).
        const auto& fields = out.graph->nodes[0].fields;
        ASSERT_EQ(fields.size(), 0u);

        auto json = ToJson(doc);
        EXPECT_EQ(json.find("\"threshold\""), std::string::npos);
        EXPECT_EQ(json.find("\"enabled\""), std::string::npos);
    }

    TEST(AuthoringDocContractTest, SerializeSource_PersistsFullGraphDocumentDto)
    {
        // D-41: acceptedProperties = source (full GraphDocumentDto). It MUST
        // carry the internal-only fields (edgeId/edgeType/dynamicPorts) that
        // the regenerated document view deliberately drops — proving source and
        // view are now distinct blobs.
        GraphDocumentDto doc;
        doc.version = 5;
        doc.fingerprint = "fp";
        auto& n1 = doc.nodes.emplace_back(MakeNode("n1", "{g1}", 0.0));
        n1.dynamic_ports.push_back({.port_id = "dyn", .port_type = "data"});
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        GraphEdgeDto e;
        e.edge_id = "e1";
        e.source_node_id = "n1";
        e.source_port_id = "o";
        e.target_node_id = "n2";
        e.target_port_id = "i";
        e.edge_type = "data";
        doc.edges.push_back(e);

        auto src = Contract::SerializeSource(doc);
        auto src_str =
            Das::Utils::SerializeYyjsonValue(src).value_or(std::string{});
        // Source keeps internal-only fields...
        EXPECT_NE(src_str.find("\"edgeId\""), std::string::npos);
        EXPECT_NE(src_str.find("\"edgeType\""), std::string::npos);
        EXPECT_NE(src_str.find("\"dynamicPorts\""), std::string::npos);
        EXPECT_NE(src_str.find("\"nodes\""), std::string::npos);

        // ...while the document view still strips them.
        auto view_str = ToJson(doc);
        EXPECT_EQ(view_str.find("edgeId"), std::string::npos);
        EXPECT_EQ(view_str.find("edgeType"), std::string::npos);
        EXPECT_EQ(view_str.find("dynamicPorts"), std::string::npos);
    }

    // -----------------------------------------------------------------------
    // Upgrade — one-way, lossless on the store
    // -----------------------------------------------------------------------

    TEST(AuthoringDocContractTest, UpgradeToGraph_RemovesTagAndIsLossless)
    {
        FormSequenceDto seq;
        seq.items.push_back(
            {.item_id = "a", .target = {.target_kind = "componentRef"}});
        seq.items.push_back(
            {.item_id = "b", .target = {.target_kind = "componentRef"}});
        auto doc = FormSequenceProjector::Project(seq);
        doc.tags.emplace_back(LINEAR_TAG);
        const auto nodes_before = doc.nodes.size();
        const auto edges_before = doc.edges.size();

        ASSERT_TRUE(UpgradeToGraph(doc));
        EXPECT_FALSE(IsLinear(doc));
        // Store untouched apart from the tag → lossless.
        EXPECT_EQ(doc.nodes.size(), nodes_before);
        EXPECT_EQ(doc.edges.size(), edges_before);

        // After upgrade, projection is graph mode.
        EXPECT_EQ(ToAuthoringDocument(doc).view_kind, ViewKind::Graph);

        // One-way: re-applying UpgradeToGraph on an already-graph doc is a
        // no-op (returns false), and there is intentionally no downgrade path.
        EXPECT_FALSE(UpgradeToGraph(doc));
        EXPECT_EQ(ToAuthoringDocument(doc).view_kind, ViewKind::Graph);
    }

    TEST(AuthoringDocContractTest, Upgrade_IsNoOpWhenNotLinear)
    {
        GraphDocumentDto doc;              // no tag → already graph mode
        EXPECT_FALSE(UpgradeToGraph(doc)); // nothing to remove
        EXPECT_FALSE(IsLinear(doc));
    }

    TEST(AuthoringDocContractTest, IsLinear_RespectsOnlyCanonicalTag)
    {
        GraphDocumentDto doc;
        EXPECT_FALSE(IsLinear(doc));
        doc.tags.emplace_back("fromsequence"); // legacy/other tag — not linear
        EXPECT_FALSE(IsLinear(doc));
        doc.tags.emplace_back(LINEAR_TAG);
        EXPECT_TRUE(IsLinear(doc));
    }

    // -----------------------------------------------------------------------
    // ApplyAuthoringChange — strict Phase 84 change kinds (graph mode)
    // -----------------------------------------------------------------------

    TEST(
        AuthoringDocContractTest,
        GraphChange_AddGraphNodeThenConnectGraphPorts)
    {
        GraphDocumentDto doc; // graph mode (no tag)
        doc.version = 0;

        // actionId must be registered (D-84-31/32/33). Here the test passes a
        // single registered id; production wires the registry snapshot.
        AuthoringChange add;
        add.kind = "addGraphNode";
        add.action_id = "{g1}";
        auto r1 = ApplyAuthoringChange(doc, add, {"{g1}"});
        ASSERT_TRUE(r1.Ok()) << r1.message;
        ASSERT_EQ(doc.nodes.size(), 1u);
        EXPECT_EQ(doc.version, 1);
        ASSERT_TRUE(r1.allocated_id.has_value());
        EXPECT_FALSE(r1.allocated_id->empty());
        EXPECT_EQ(doc.nodes[0].node_id, *r1.allocated_id);

        AuthoringChange add2;
        add2.kind = "addGraphNode";
        add2.action_id = "{g2}";
        auto r2 = ApplyAuthoringChange(doc, add2, {"{g2}"});
        ASSERT_TRUE(r2.Ok());

        AuthoringChange conn;
        conn.kind = "connectGraphPorts";
        conn.connection =
            GraphConnection{*r1.allocated_id, "out", *r2.allocated_id, "in"};
        ASSERT_TRUE(ApplyAuthoringChange(doc, conn).Ok());
        ASSERT_EQ(doc.edges.size(), 1u);
        EXPECT_EQ(doc.edges[0].source_node_id, *r1.allocated_id);
        EXPECT_EQ(doc.edges[0].edge_type, "data"); // graph-mode edges are data

        // The public projection still hides edge_type / edge_id.
        auto json = ToJson(doc);
        EXPECT_EQ(json.find("edgeType"), std::string::npos);
        EXPECT_EQ(json.find("edgeId"), std::string::npos);
    }

    TEST(AuthoringDocContractTest, GraphChange_RemoveGraphNodeCascadesEdges)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        GraphEdgeDto e;
        e.edge_id = "e1";
        e.source_node_id = "n1";
        e.source_port_id = "o";
        e.target_node_id = "n2";
        e.target_port_id = "i";
        e.edge_type = "data";
        doc.edges.push_back(e);

        AuthoringChange rm;
        rm.kind = "removeGraphNode";
        rm.item_or_node_id = "n1";
        ASSERT_TRUE(ApplyAuthoringChange(doc, rm).Ok());
        ASSERT_EQ(doc.nodes.size(), 1u);
        EXPECT_EQ(doc.nodes[0].node_id, "n2");
        EXPECT_TRUE(doc.edges.empty()); // cascade
    }

    TEST(AuthoringDocContractTest, GraphChange_DisconnectGraphPortsByEndpoints)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        GraphEdgeDto e;
        e.edge_id = "private";
        e.source_node_id = "n1";
        e.source_port_id = "o";
        e.target_node_id = "n2";
        e.target_port_id = "i";
        e.edge_type = "data";
        doc.edges.push_back(e);

        AuthoringChange disc;
        disc.kind = "disconnectGraphPorts";
        disc.connection = GraphConnection{"n1", "o", "n2", "i"};
        ASSERT_TRUE(ApplyAuthoringChange(doc, disc).Ok());
        EXPECT_TRUE(doc.edges.empty());
    }

    TEST(
        AuthoringDocContractTest,
        GraphChange_AddGraphNodeRejectsUnknownActionId)
    {
        GraphDocumentDto doc;
        const auto       before = doc.nodes.size();

        AuthoringChange add;
        add.kind = "addGraphNode";
        add.action_id = "{unregistered}";
        auto r = ApplyAuthoringChange(doc, add, {"{g1}"});
        ASSERT_FALSE(r.Ok());
        EXPECT_EQ(r.error_kind, ChangeErrorKind::UnknownActionId);
        EXPECT_EQ(doc.nodes.size(), before); // unchanged
        EXPECT_EQ(doc.version, 0);           // no revision bump
    }

    // -----------------------------------------------------------------------
    // ApplyAuthoringChange — linear mode (reuses FormSequenceProjector)
    // -----------------------------------------------------------------------

    TEST(
        AuthoringDocContractTest,
        LinearChange_AddFormSequenceItemAppendsToChain)
    {
        // Seed a 2-item linear store via the projector + tag.
        FormSequenceDto seq;
        seq.items.push_back(
            {.item_id = "a",
             .target =
                 {.target_kind = "componentRef",
                  .component_ref = MakeCompRef("{ga}")},
             .settings = MakeSettings(1.0)});
        seq.items.push_back(
            {.item_id = "b",
             .target =
                 {.target_kind = "componentRef",
                  .component_ref = MakeCompRef("{gb}")},
             .settings = MakeSettings(2.0)});
        auto doc = FormSequenceProjector::Project(seq);
        doc.tags.emplace_back(LINEAR_TAG);
        ASSERT_EQ(doc.edges.size(), 1u); // a->b signal

        AuthoringChange add;
        add.kind = "addFormSequenceItem";
        add.action_id = "{gc}";
        auto r = ApplyAuthoringChange(doc, add, {"{gc}"});
        ASSERT_TRUE(r.Ok()) << r.message;
        ASSERT_TRUE(r.allocated_id.has_value());

        // Chain extended: a->b->new (two signal edges), still linear.
        ASSERT_EQ(doc.nodes.size(), 3u);
        ASSERT_EQ(doc.edges.size(), 2u);
        EXPECT_TRUE(IsLinear(doc));
        EXPECT_EQ(doc.edges[0].edge_type, "signal");
    }

    TEST(
        AuthoringDocContractTest,
        LinearChange_RemoveFormSequenceItemReconnectsChain)
    {
        FormSequenceDto seq;
        seq.items.push_back(
            {.item_id = "a",
             .target = {
                 .target_kind = "componentRef",
                 .component_ref = MakeCompRef("{ga}")}});
        seq.items.push_back(
            {.item_id = "b",
             .target = {
                 .target_kind = "componentRef",
                 .component_ref = MakeCompRef("{gb}")}});
        seq.items.push_back(
            {.item_id = "c",
             .target = {
                 .target_kind = "componentRef",
                 .component_ref = MakeCompRef("{gc}")}});
        auto doc = FormSequenceProjector::Project(seq);
        doc.tags.emplace_back(LINEAR_TAG);

        AuthoringChange rm;
        rm.kind = "removeFormSequenceItem";
        rm.item_or_node_id = "b";
        ASSERT_TRUE(ApplyAuthoringChange(doc, rm).Ok());

        // b gone; chain a->c reconnected (1 signal edge).
        ASSERT_EQ(doc.nodes.size(), 2u);
        ASSERT_EQ(doc.edges.size(), 1u);
        EXPECT_EQ(doc.edges[0].source_node_id, "a");
        EXPECT_EQ(doc.edges[0].target_node_id, "c");
    }

    TEST(
        AuthoringDocContractTest,
        LinearChange_MoveFormSequenceItemReordersChain)
    {
        FormSequenceDto seq;
        for (const char c : {'a', 'b', 'c'})
        {
            seq.items.push_back(
                {.item_id = std::string(1, c),
                 .target = {
                     .target_kind = "componentRef",
                     .component_ref = MakeCompRef("{g}")}});
        }
        auto doc = FormSequenceProjector::Project(seq);
        doc.tags.emplace_back(LINEAR_TAG);

        AuthoringChange mv;
        mv.kind = "moveFormSequenceItem";
        mv.form_sequence_item_id = "a";
        mv.destination_index = 2; // a moves to the end → b->c->a
        ASSERT_TRUE(ApplyAuthoringChange(doc, mv).Ok());

        // New head is b.
        auto items = ToAuthoringDocument(doc).form_sequence->items;
        ASSERT_EQ(items.size(), 3u);
        EXPECT_EQ(items[0].id, "b");
        EXPECT_EQ(items[1].id, "c");
        EXPECT_EQ(items[2].id, "a");
    }

    TEST(AuthoringDocContractTest, LinearChange_RejectsGraphOnlyKind)
    {
        GraphDocumentDto doc;
        doc.tags.emplace_back(LINEAR_TAG);

        AuthoringChange bad;
        bad.kind = "addGraphNode";
        bad.action_id = "{g}";
        auto r = ApplyAuthoringChange(doc, bad, {"{g}"});
        EXPECT_FALSE(r.Ok());
        EXPECT_EQ(r.error_kind, ChangeErrorKind::InvalidOp);
    }

    TEST(
        AuthoringDocContractTest,
        LinearChange_AddFormSequenceItemRejectsUnknownActionId)
    {
        FormSequenceDto seq;
        seq.items.push_back(
            {.item_id = "a", .target = {.target_kind = "componentRef"}});
        auto doc = FormSequenceProjector::Project(seq);
        doc.tags.emplace_back(LINEAR_TAG);

        AuthoringChange add;
        add.kind = "addFormSequenceItem";
        add.action_id = "{unregistered}";
        auto r = ApplyAuthoringChange(doc, add, {"{ga}"});
        ASSERT_FALSE(r.Ok());
        EXPECT_EQ(r.error_kind, ChangeErrorKind::UnknownActionId);
        EXPECT_EQ(doc.version, 0);
        EXPECT_FALSE(r.allocated_id.has_value());
    }

    // -----------------------------------------------------------------------
    // convertView — D-84-29/38: stable IDs across formSequence <-> graph.
    // -----------------------------------------------------------------------

    TEST(AuthoringDocContractTest, ConvertView_LinearToGraphPreservesNodeIds)
    {
        FormSequenceDto seq;
        seq.items.push_back(
            {.item_id = "a",
             .target = {
                 .target_kind = "componentRef",
                 .component_ref = MakeCompRef("{ga}")}});
        seq.items.push_back(
            {.item_id = "b",
             .target = {
                 .target_kind = "componentRef",
                 .component_ref = MakeCompRef("{gb}")}});
        auto doc = FormSequenceProjector::Project(seq);
        doc.tags.emplace_back(LINEAR_TAG);

        // Snapshot the ids before conversion; the linear projection currently
        // shows them as FormSequenceItem.id.
        const auto before = ToAuthoringDocument(doc);
        ASSERT_EQ(before.view_kind, ViewKind::FormSequence);
        ASSERT_EQ(before.form_sequence->items.size(), 2u);
        const std::string id0 = before.form_sequence->items[0].id;
        const std::string id1 = before.form_sequence->items[1].id;

        AuthoringChange cv;
        cv.kind = "convertView";
        cv.target_view_kind = "graph";
        ASSERT_TRUE(ApplyAuthoringChange(doc, cv).Ok());

        const auto after = ToAuthoringDocument(doc);
        ASSERT_EQ(after.view_kind, ViewKind::Graph);
        ASSERT_EQ(after.graph->nodes.size(), 2u);
        // D-84-38: same node ids survive conversion.
        EXPECT_EQ(after.graph->nodes[0].id, id0);
        EXPECT_EQ(after.graph->nodes[1].id, id1);
    }

    TEST(AuthoringDocContractTest, ConvertView_RejectsUnsupportedTarget)
    {
        GraphDocumentDto doc;
        doc.tags.emplace_back(LINEAR_TAG);

        AuthoringChange cv;
        cv.kind = "convertView";
        cv.target_view_kind = "formSequence"; // unsupported from linear mode
        auto r = ApplyAuthoringChange(doc, cv);
        EXPECT_FALSE(r.Ok());
        EXPECT_EQ(r.error_kind, ChangeErrorKind::UnsupportedView);
    }

    // -----------------------------------------------------------------------
    // setValue — D-84-24: strict {target, fieldName, value}.
    // -----------------------------------------------------------------------

    TEST(AuthoringDocContractTest, SetValue_GraphNodeFieldUpdatesSettings)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));

        AuthoringChange set;
        set.kind = "setValue";
        set.target = SetValueTarget{"graphNode", "n1"};
        set.field_name = "threshold";
        // The setValue payload is the raw value (yyjson::value::operator=
        // accepts the literal directly).
        set.value = 0.9;
        ASSERT_TRUE(ApplyAuthoringChange(doc, set).Ok());

        // The internal store now carries the new value under node.settings.
        ASSERT_FALSE(doc.nodes.empty());
        auto settings_obj = doc.nodes[0].settings.as_object();
        ASSERT_TRUE(settings_obj.has_value());
        ASSERT_TRUE(settings_obj->contains(std::string_view("threshold")));
        auto threshold =
            (*settings_obj)[std::string_view("threshold")].as_real();
        ASSERT_TRUE(threshold.has_value());
        EXPECT_DOUBLE_EQ(*threshold, 0.9);
    }

    // -----------------------------------------------------------------------
    // moveGraphNode — document-level viewData partition (graph mode)
    // -----------------------------------------------------------------------

    /// Read viewData.positions[node_id].axis from the store; nullopt when the
    /// partition, the node entry, or the axis is absent.
    std::optional<double> ViewPositionOf(
        const Dto::GraphDocumentDto& doc,
        std::string_view             node_id,
        std::string_view             axis)
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

    AuthoringChange MakeMoveGraphNode(
        std::string_view node_id,
        double           x,
        double           y)
    {
        AuthoringChange move;
        move.kind = "moveGraphNode";
        if (!node_id.empty())
        {
            move.item_or_node_id = std::string(node_id);
        }
        move.position_x = x;
        move.position_y = y;
        return move;
    }

    TEST(
        AuthoringDocContractTest,
        GraphChange_MoveGraphNodeWritesViewDataPositions)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));

        auto r =
            ApplyAuthoringChange(doc, MakeMoveGraphNode("n1", 120.5, 88.0));
        ASSERT_TRUE(r.Ok()) << r.message;

        // The coordinates live in the document-level viewData partition.
        ASSERT_TRUE(ViewPositionOf(doc, "n1", "x").has_value());
        EXPECT_DOUBLE_EQ(*ViewPositionOf(doc, "n1", "x"), 120.5);
        ASSERT_TRUE(ViewPositionOf(doc, "n1", "y").has_value());
        EXPECT_DOUBLE_EQ(*ViewPositionOf(doc, "n1", "y"), 88.0);
        // node.settings stays untouched (viewData is not node data).
        auto settings_obj = doc.nodes[0].settings.as_object();
        ASSERT_TRUE(settings_obj.has_value());
        EXPECT_FALSE(settings_obj->contains(std::string_view("x")));
        EXPECT_FALSE(settings_obj->contains(std::string_view("y")));

        // The public document wire carries a root-level viewData key.
        auto json = ToJson(doc);
        EXPECT_NE(json.find("\"viewData\""), std::string::npos);
        EXPECT_NE(json.find("\"positions\""), std::string::npos);
    }

    TEST(AuthoringDocContractTest, GraphChange_MoveGraphNodeRequiresNodeId)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));

        auto r = ApplyAuthoringChange(doc, MakeMoveGraphNode("", 1.0, 2.0));
        EXPECT_FALSE(r.Ok());
        EXPECT_EQ(r.error_kind, ChangeErrorKind::InvalidOp);
        EXPECT_NE(
            r.message.find("moveGraphNode.nodeId required"),
            std::string::npos);
        // The store is untouched on failure.
        EXPECT_TRUE(doc.view_data.is_null());
    }

    TEST(
        AuthoringDocContractTest,
        GraphChange_MoveGraphNodeCreatesPositionsOnBareViewData)
    {
        GraphDocumentDto doc; // fresh store: view_data is the null default
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        ASSERT_TRUE(doc.view_data.is_null());

        ASSERT_TRUE(
            ApplyAuthoringChange(doc, MakeMoveGraphNode("n1", 10.0, 20.0))
                .Ok());
        ASSERT_TRUE(doc.view_data.as_object().has_value());

        // A second node id lands in the same lazily-created positions map.
        ASSERT_TRUE(
            ApplyAuthoringChange(doc, MakeMoveGraphNode("n2", 30.0, 40.0))
                .Ok());
        ASSERT_TRUE(ViewPositionOf(doc, "n1", "x").has_value());
        EXPECT_DOUBLE_EQ(*ViewPositionOf(doc, "n1", "x"), 10.0);
        ASSERT_TRUE(ViewPositionOf(doc, "n2", "y").has_value());
        EXPECT_DOUBLE_EQ(*ViewPositionOf(doc, "n2", "y"), 40.0);
    }

    TEST(AuthoringDocContractTest, SerializeSource_EmitsViewDataKey)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));

        ASSERT_TRUE(
            ApplyAuthoringChange(doc, MakeMoveGraphNode("n1", 5.5, 6.5)).Ok());

        auto source = Contract::SerializeSource(doc);
        auto source_str =
            Das::Utils::SerializeYyjsonValue(source).value_or(std::string{});
        EXPECT_NE(source_str.find("\"viewData\""), std::string::npos);
        EXPECT_NE(source_str.find("\"positions\""), std::string::npos);
    }

    TEST(AuthoringDocContractTest, GraphChange_MoveGraphNodeDoesNotBumpRevision)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.version = 5;

        auto r = ApplyAuthoringChange(doc, MakeMoveGraphNode("n1", 1.0, 2.0));
        ASSERT_TRUE(r.Ok()) << r.message;
        // Coordinate changes ride the lightweight viewData channel: the
        // semantic revision must stay put.
        EXPECT_EQ(doc.version, 5);
    }

    TEST(
        AuthoringDocContractTest,
        GraphChange_ConnectGraphPortsStillBumpsRevision)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        doc.version = 5;

        AuthoringChange conn;
        conn.kind = "connectGraphPorts";
        conn.connection = GraphConnection{"n1", "o", "n2", "i"};
        auto r = ApplyAuthoringChange(doc, conn);
        ASSERT_TRUE(r.Ok()) << r.message;
        EXPECT_EQ(doc.version, 6);
    }

    TEST(AuthoringDocContractTest, GraphChange_SetValueStillBumpsRevision)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.version = 5;

        AuthoringChange set;
        set.kind = "setValue";
        set.target = SetValueTarget{"graphNode", "n1"};
        set.field_name = "threshold";
        set.value = 0.9;
        auto r = ApplyAuthoringChange(doc, set);
        ASSERT_TRUE(r.Ok()) << r.message;
        EXPECT_EQ(doc.version, 6);
    }

    // -----------------------------------------------------------------------
    // removeGraphNodes / moveGraphNodes — batch primitives. One gesture =
    // one message: validate-all-then-mutate atomicity (any reject leaves the
    // store byte-identical), removeGraphNodes bumps the semantic revision
    // exactly once, moveGraphNodes rides the viewData lightweight channel.
    // -----------------------------------------------------------------------

    AuthoringChange MakeRemoveGraphNodes(
        const std::vector<std::string>&     node_ids,
        const std::vector<GraphConnection>& connections = {})
    {
        AuthoringChange change;
        change.kind = "removeGraphNodes";
        change.removed_node_ids = node_ids;
        change.removed_connections = connections;
        return change;
    }

    AuthoringChange MakeMoveGraphNodes(
        const std::vector<std::tuple<std::string, double, double>>& moves)
    {
        AuthoringChange change;
        change.kind = "moveGraphNodes";
        std::vector<GraphNodeMove> node_moves;
        node_moves.reserve(moves.size());
        for (const auto& [node_id, x, y] : moves)
        {
            node_moves.push_back(GraphNodeMove{node_id, x, y});
        }
        change.node_moves = std::move(node_moves);
        return change;
    }

    TEST(
        AuthoringDocContractTest,
        GraphChange_RemoveGraphNodesDeletesBatchAtomically)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        doc.nodes.push_back(MakeNode("n3", "{g3}", 0.0));
        doc.edges.push_back(MakeDataEdge("e1", "n1", "o", "n2", "i"));
        doc.version = 5;
        // Seed coordinates for the doomed nodes plus a survivor (the
        // coordinate channel never bumps the revision).
        ASSERT_TRUE(
            ApplyAuthoringChange(doc, MakeMoveGraphNode("n1", 1.0, 2.0)).Ok());
        ASSERT_TRUE(
            ApplyAuthoringChange(doc, MakeMoveGraphNode("n2", 3.0, 4.0)).Ok());
        ASSERT_TRUE(
            ApplyAuthoringChange(doc, MakeMoveGraphNode("n3", 5.0, 6.0)).Ok());

        auto r = ApplyAuthoringChange(doc, MakeRemoveGraphNodes({"n1", "n2"}));
        ASSERT_TRUE(r.Ok()) << r.message;

        // Both nodes are gone and the cascaded edge went with them.
        ASSERT_EQ(doc.nodes.size(), 1u);
        EXPECT_EQ(doc.nodes[0].node_id, "n3");
        EXPECT_TRUE(doc.edges.empty());
        // Orphan coordinate entries are cleaned with their nodes.
        EXPECT_FALSE(ViewPositionOf(doc, "n1", "x").has_value());
        EXPECT_FALSE(ViewPositionOf(doc, "n2", "x").has_value());
        ASSERT_TRUE(ViewPositionOf(doc, "n3", "x").has_value());
        EXPECT_DOUBLE_EQ(*ViewPositionOf(doc, "n3", "x"), 5.0);
        // One message bumps the semantic revision exactly once (not once
        // per node).
        EXPECT_EQ(doc.version, 6);
    }

    TEST(
        AuthoringDocContractTest,
        GraphChange_RemoveGraphNodesRejectsUnknownIdAtomically)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        doc.edges.push_back(MakeDataEdge("e1", "n1", "o", "n2", "i"));
        doc.version = 5;
        ASSERT_TRUE(
            ApplyAuthoringChange(doc, MakeMoveGraphNode("n1", 1.0, 2.0)).Ok());

        auto r =
            ApplyAuthoringChange(doc, MakeRemoveGraphNodes({"n1", "ghost"}));
        EXPECT_FALSE(r.Ok());
        EXPECT_EQ(r.error_kind, ChangeErrorKind::NodeNotFound);

        // The whole batch is rejected — the store is byte-identical.
        EXPECT_EQ(doc.version, 5);
        ASSERT_EQ(doc.nodes.size(), 2u);
        ASSERT_EQ(doc.edges.size(), 1u);
        ASSERT_TRUE(ViewPositionOf(doc, "n1", "x").has_value());
    }

    TEST(
        AuthoringDocContractTest,
        GraphChange_RemoveGraphNodesDropsListedConnections)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        doc.nodes.push_back(MakeNode("n3", "{g3}", 0.0));
        doc.edges.push_back(MakeDataEdge("e1", "n1", "o", "n2", "i"));
        doc.edges.push_back(MakeDataEdge("e2", "n2", "o", "n3", "i"));
        doc.version = 3;

        auto r = ApplyAuthoringChange(
            doc,
            MakeRemoveGraphNodes({}, {GraphConnection{"n1", "o", "n2", "i"}}));
        ASSERT_TRUE(r.Ok()) << r.message;

        // Only the listed connection is dropped; the endpoint nodes survive.
        ASSERT_EQ(doc.nodes.size(), 3u);
        ASSERT_EQ(doc.edges.size(), 1u);
        EXPECT_EQ(doc.edges[0].edge_id, "e2");
        EXPECT_EQ(doc.version, 4);
    }

    TEST(
        AuthoringDocContractTest,
        GraphChange_RemoveGraphNodesRejectsUnknownConnectionAtomically)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        doc.edges.push_back(MakeDataEdge("e1", "n1", "o", "n2", "i"));
        doc.version = 3;

        auto r = ApplyAuthoringChange(
            doc,
            MakeRemoveGraphNodes(
                {"n1"},
                {GraphConnection{"n1", "o", "n2", "WRONG_PORT"}}));
        EXPECT_FALSE(r.Ok());
        EXPECT_EQ(r.error_kind, ChangeErrorKind::EdgeNotFound);

        // The whole batch is rejected — node deletion included.
        EXPECT_EQ(doc.version, 3);
        ASSERT_EQ(doc.nodes.size(), 2u);
        ASSERT_EQ(doc.edges.size(), 1u);
    }

    TEST(
        AuthoringDocContractTest,
        GraphChange_MoveGraphNodesWritesCoordinatesWithoutBump)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        doc.version = 9;

        auto r = ApplyAuthoringChange(
            doc,
            MakeMoveGraphNodes({{"n1", 11.5, 12.0}, {"n2", 21.0, 22.25}}));
        ASSERT_TRUE(r.Ok()) << r.message;

        ASSERT_TRUE(ViewPositionOf(doc, "n1", "x").has_value());
        EXPECT_DOUBLE_EQ(*ViewPositionOf(doc, "n1", "x"), 11.5);
        ASSERT_TRUE(ViewPositionOf(doc, "n1", "y").has_value());
        EXPECT_DOUBLE_EQ(*ViewPositionOf(doc, "n1", "y"), 12.0);
        ASSERT_TRUE(ViewPositionOf(doc, "n2", "x").has_value());
        EXPECT_DOUBLE_EQ(*ViewPositionOf(doc, "n2", "x"), 21.0);
        ASSERT_TRUE(ViewPositionOf(doc, "n2", "y").has_value());
        EXPECT_DOUBLE_EQ(*ViewPositionOf(doc, "n2", "y"), 22.25);
        // The lightweight coordinate channel leaves the revision untouched.
        EXPECT_EQ(doc.version, 9);
    }

    TEST(
        AuthoringDocContractTest,
        GraphChange_MoveGraphNodesRejectsUnknownIdAtomically)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        doc.version = 7;

        auto r = ApplyAuthoringChange(
            doc,
            MakeMoveGraphNodes({{"n1", 1.0, 2.0}, {"ghost", 3.0, 4.0}}));
        EXPECT_FALSE(r.Ok());
        EXPECT_EQ(r.error_kind, ChangeErrorKind::NodeNotFound);

        // Zero coordinate writes leaked through the rejected batch.
        EXPECT_TRUE(doc.view_data.is_null());
        EXPECT_FALSE(ViewPositionOf(doc, "n1", "x").has_value());
        EXPECT_EQ(doc.version, 7);
    }

    // The singular move must reject unknown nodes instead of writing
    // orphan coordinates into the persisted viewData partition.
    TEST(AuthoringDocContractTest, GraphChange_MoveGraphNodeRejectsUnknownNode)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.version = 5;

        auto r =
            ApplyAuthoringChange(doc, MakeMoveGraphNode("ghost", 1.0, 2.0));
        EXPECT_FALSE(r.Ok());
        EXPECT_EQ(r.error_kind, ChangeErrorKind::NodeNotFound);
        EXPECT_TRUE(doc.view_data.is_null());
        EXPECT_EQ(doc.version, 5);
    }

    // Node removal must clean the orphan coordinate entry so stale
    // positions never ride along into persistence.
    TEST(AuthoringDocContractTest, GraphChange_RemoveGraphNodeCleansPosition)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        ASSERT_TRUE(
            ApplyAuthoringChange(doc, MakeMoveGraphNode("n1", 1.0, 2.0)).Ok());
        ASSERT_TRUE(
            ApplyAuthoringChange(doc, MakeMoveGraphNode("n2", 3.0, 4.0)).Ok());

        AuthoringChange remove;
        remove.kind = "removeGraphNode";
        remove.item_or_node_id = "n1";
        auto r = ApplyAuthoringChange(doc, remove);
        ASSERT_TRUE(r.Ok()) << r.message;

        ASSERT_EQ(doc.nodes.size(), 1u);
        EXPECT_FALSE(ViewPositionOf(doc, "n1", "x").has_value());
        ASSERT_TRUE(ViewPositionOf(doc, "n2", "x").has_value());
        EXPECT_DOUBLE_EQ(*ViewPositionOf(doc, "n2", "x"), 3.0);
    }

    // -----------------------------------------------------------------------
    // renameGraphNode — the store display label. Semantic change (revision
    // exactly +1); the label is pure presentation data, so the entire
    // addressing surface (node id, viewData.positions keys, edge endpoints,
    // subsequent message addressing) must stay untouched.
    // -----------------------------------------------------------------------

    AuthoringChange MakeRenameGraphNode(
        std::string_view node_id,
        std::string_view label)
    {
        AuthoringChange change;
        change.kind = "renameGraphNode";
        if (!node_id.empty())
        {
            change.item_or_node_id = std::string(node_id);
        }
        if (!label.empty())
        {
            change.rename_label = std::string(label);
        }
        return change;
    }

    TEST(
        AuthoringDocContractTest,
        GraphChange_RenameGraphNodeWritesStoreLabelAndBumpsRevision)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.version = 5;

        auto r = ApplyAuthoringChange(
            doc,
            MakeRenameGraphNode("n1", "我的延迟节点"));
        ASSERT_TRUE(r.Ok()) << r.message;
        ASSERT_TRUE(doc.nodes[0].label.has_value());
        EXPECT_EQ(*doc.nodes[0].label, "我的延迟节点");
        // Semantic change: the revision moves exactly once per rename.
        EXPECT_EQ(doc.version, 6);

        // Renaming again overwrites the previous label and bumps once more.
        auto r2 =
            ApplyAuthoringChange(doc, MakeRenameGraphNode("n1", "第二个名字"));
        ASSERT_TRUE(r2.Ok()) << r2.message;
        ASSERT_TRUE(doc.nodes[0].label.has_value());
        EXPECT_EQ(*doc.nodes[0].label, "第二个名字");
        EXPECT_EQ(doc.version, 7);
    }

    TEST(AuthoringDocContractTest, GraphChange_RenameGraphNodeProjectsOntoWire)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));

        ActionCatalog      catalog;
        ActionCatalogEntry entry;
        entry.action_id = "{g1}";
        entry.label = "das.flow.delay";
        catalog.entries.push_back(entry);

        ASSERT_TRUE(
            ApplyAuthoringChange(doc, MakeRenameGraphNode("n1", "用户名"))
                .Ok());
        const auto out = ToAuthoringDocument(doc, catalog);
        ASSERT_EQ(out.graph->nodes.size(), 1u);
        // The store label wins over the catalog default name after rename.
        EXPECT_EQ(out.graph->nodes[0].label, "用户名");
    }

    TEST(
        AuthoringDocContractTest,
        GraphChange_RenameGraphNodeRejectsUnknownNodeAtomically)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes[0].label = "原名";
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        doc.version = 5;

        auto r =
            ApplyAuthoringChange(doc, MakeRenameGraphNode("ghost", "新名"));
        EXPECT_FALSE(r.Ok());
        EXPECT_EQ(r.error_kind, ChangeErrorKind::NodeNotFound);
        // The whole change is rejected — zero mutation.
        EXPECT_EQ(doc.version, 5);
        ASSERT_EQ(doc.nodes.size(), 2u);
        ASSERT_TRUE(doc.nodes[0].label.has_value());
        EXPECT_EQ(*doc.nodes[0].label, "原名");
    }

    TEST(
        AuthoringDocContractTest,
        GraphChange_RenameGraphNodeRequiresBothFields)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));

        auto r_no_id = ApplyAuthoringChange(doc, MakeRenameGraphNode("", "名"));
        EXPECT_FALSE(r_no_id.Ok());
        EXPECT_EQ(r_no_id.error_kind, ChangeErrorKind::InvalidOp);

        auto r_no_label =
            ApplyAuthoringChange(doc, MakeRenameGraphNode("n1", ""));
        EXPECT_FALSE(r_no_label.Ok());
        EXPECT_EQ(r_no_label.error_kind, ChangeErrorKind::InvalidOp);
        // Neither failure touched the store.
        EXPECT_FALSE(doc.nodes[0].label.has_value());
    }

    // The label is display-only: renaming must leave the node id, the
    // viewData.positions key set, the edge endpoints and subsequent message
    // addressing (setValueGraphNode on the same id) completely untouched.
    TEST(AuthoringDocContractTest, GraphChange_RenameGraphNodeKeepsAddressing)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.nodes.push_back(MakeNode("n2", "{g2}", 0.0));
        doc.edges.push_back(MakeDataEdge("e1", "n1", "out", "n2", "in"));
        ASSERT_TRUE(
            ApplyAuthoringChange(doc, MakeMoveGraphNode("n1", 11.0, 12.0))
                .Ok());
        ASSERT_TRUE(
            ApplyAuthoringChange(doc, MakeMoveGraphNode("n2", 21.0, 22.0))
                .Ok());

        auto r = ApplyAuthoringChange(doc, MakeRenameGraphNode("n1", "重命名"));
        ASSERT_TRUE(r.Ok()) << r.message;

        // Node id unchanged (label never becomes an address).
        ASSERT_EQ(doc.nodes.size(), 2u);
        EXPECT_EQ(doc.nodes[0].node_id, "n1");
        EXPECT_EQ(doc.nodes[1].node_id, "n2");
        // viewData.positions key set and values unchanged.
        ASSERT_TRUE(ViewPositionOf(doc, "n1", "x").has_value());
        EXPECT_DOUBLE_EQ(*ViewPositionOf(doc, "n1", "x"), 11.0);
        ASSERT_TRUE(ViewPositionOf(doc, "n2", "y").has_value());
        EXPECT_DOUBLE_EQ(*ViewPositionOf(doc, "n2", "y"), 22.0);
        // Edge endpoints unchanged.
        ASSERT_EQ(doc.edges.size(), 1u);
        EXPECT_EQ(doc.edges[0].source_node_id, "n1");
        EXPECT_EQ(doc.edges[0].source_port_id, "out");
        EXPECT_EQ(doc.edges[0].target_node_id, "n2");
        EXPECT_EQ(doc.edges[0].target_port_id, "in");

        // Subsequent message addressing on the same node id still works.
        AuthoringChange set;
        set.kind = "setValue";
        set.target = SetValueTarget{"graphNode", "n1"};
        set.field_name = "threshold";
        set.value = 0.75;
        auto set_result = ApplyAuthoringChange(doc, set);
        EXPECT_TRUE(set_result.Ok()) << set_result.message;
    }

    // -----------------------------------------------------------------------
    // Phase 84 strict wire (D-84-02/07/13/38/39) — viewKind, items[], no
    // public settings/sourceFingerprint, EntryRef={entryId} only.
    // -----------------------------------------------------------------------

    TEST(
        AuthoringDocContractTest,
        NewWire_EmitsViewKindAndItemsNotLegacyAliases)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.tags.emplace_back(LINEAR_TAG);

        const auto json = ToJson(doc);
        // New strict wire uses viewKind (not kind) and formSequence.items
        // (not formSequence.sequence).
        EXPECT_NE(
            json.find("\"viewKind\":\"formSequence\""),
            std::string::npos);
        EXPECT_EQ(json.find("\"kind\":\"formSequence\""), std::string::npos);
        EXPECT_NE(json.find("\"items\":["), std::string::npos);
        EXPECT_EQ(json.find("\"sequence\":["), std::string::npos);
    }

    TEST(AuthoringDocContractTest, NewWire_GraphModeAlsoUsesViewKind)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));

        const auto json = ToJson(doc);
        EXPECT_NE(json.find("\"viewKind\":\"graph\""), std::string::npos);
        EXPECT_EQ(json.find("\"kind\":\"graph\""), std::string::npos);
    }

    TEST(
        AuthoringDocContractTest,
        NewWire_DropsPublicSettingsAndSourceFingerprint)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeNode("n1", "{g1}", 0.0));
        doc.fingerprint = "should-not-leak";
        doc.tags.emplace_back(LINEAR_TAG);

        const auto json = ToJson(doc);
        // Public wire drops business settings (D-84-07) and the document-level
        // sourceFingerprint (D-84-04).
        EXPECT_EQ(json.find("\"settings\""), std::string::npos);
        EXPECT_EQ(json.find("\"sourceFingerprint\""), std::string::npos);
    }

    TEST(AuthoringDocContractTest, NewWire_EntryRefCarriesOnlyEntryId)
    {
        GraphDocumentDto doc;
        doc.nodes.push_back(MakeEntryRefNode("sub", 42, 3, "fp-sub"));

        const auto json = ToJson(doc);
        EXPECT_NE(json.find("\"entryId\":42"), std::string::npos);
        // D-84-39: expectedRevision and sourceFingerprint are removed from the
        // public entryRef. They remain internal to GraphDocumentDto.
        EXPECT_EQ(json.find("\"expectedRevision\""), std::string::npos);
        EXPECT_EQ(json.find("\"sourceFingerprint\""), std::string::npos);
    }
} // namespace
