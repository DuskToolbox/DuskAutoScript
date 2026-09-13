#include <das/Core/GraphRuntime/GraphCompiler.h>
#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/Core/TaskScheduler/TaskAuthoringContract.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace
{
    using DAS::DasPtr;

    DasPtr<IDasReadOnlyString> MakeStr(const std::string& s)
    {
        DasPtr<IDasReadOnlyString> ro;
        CreateIDasReadOnlyStringFromUtf8(s.c_str(), ro.Put());
        return ro;
    }

    std::string JsonToUtf8(Das::ExportInterface::IDasJson* json)
    {
        if (json == nullptr)
        {
            return {};
        }
        DasPtr<IDasReadOnlyString> text;
        if (DAS::IsFailed(json->ToString(0, text.Put()))
            || text.Get() == nullptr)
        {
            return {};
        }
        const char* utf8 = nullptr;
        if (DAS::IsFailed(text->GetUtf8(&utf8)) || utf8 == nullptr)
        {
            return {};
        }
        return std::string(utf8);
    }

    // Collapse all whitespace so assertions are spacing/format agnostic.
    std::string Compact(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        for (const char c : s)
        {
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            {
                out.push_back(c);
            }
        }
        return out;
    }

    // RAII handle to a Core authoring session.
    struct SessionHandle
    {
        GraphAuthoringSessionState* p = nullptr;
        explicit SessionHandle(const std::string& context)
        {
            auto s = MakeStr(context);
            CreateGraphAuthoringSession(s.Get(), &p);
        }
        ~SessionHandle() { DestroyGraphAuthoringSession(p); }
        SessionHandle(const SessionHandle&) = delete;
        SessionHandle& operator=(const SessionHandle&) = delete;
    };

    // GraphAuthoringSessionGetDocument returns the strict Phase 84 envelope
    // {document, diagnostics}. The full envelope is needed for validator
    // cross-checks; field-level assertions unwrap the document payload.
    DasPtr<Das::ExportInterface::IDasJson> GetDocument(
        GraphAuthoringSessionState* p)
    {
        DasPtr<Das::ExportInterface::IDasJson> out;
        GraphAuthoringSessionGetDocument(p, nullptr, out.Put());
        return out;
    }

    // Extract the nested `document` value from the Get envelope. Field-level
    // assertions read from here; validator-level assertions use the full
    // envelope returned by GetDocument.
    yyjson::value DocumentOf(GraphAuthoringSessionState* p)
    {
        auto json = JsonToUtf8(GetDocument(p).Get());
        auto root = yyjson::read(json);
        auto obj = root.as_object();
        if (!obj || !obj->contains(std::string_view("document")))
        {
            return Das::Utils::MakeYyjsonObject();
        }
        return Das::Utils::CloneYyjsonValue(
            (*obj)[std::string_view("document")]);
    }

    DasPtr<Das::ExportInterface::IDasJson> ApplyChange(
        GraphAuthoringSessionState* p,
        const std::string&          change)
    {
        DasPtr<Das::ExportInterface::IDasJson> out;
        auto                                   s = MakeStr(change);
        GraphAuthoringSessionApplyChange(p, s.Get(), out.Put());
        return out;
    }

    // Helper: populate the session's registry-backed actionCatalog so that
    // addGraphNode / addFormSequenceItem succeed (D-84-31/32/33). Tests inject
    // a literal {actionId,label} entry set rather than reaching into
    // PluginManager. Returns the Core result so shape-rejection cases can
    // assert on it.
    DasResult SetActionCatalog(
        GraphAuthoringSessionState* p,
        const std::string&          json_array)
    {
        auto s = MakeStr(json_array);
        return GraphAuthoringSessionSetActionCatalog(p, s.Get());
    }

    // The Phase 84 strict AuthoringChange parser rejects the legacy flat
    // {op, ...} shape; only the unified {baseRevision, kind, payload}
    // envelope is accepted. Production callers inject baseRevision at submit
    // time; tests use 0 (fresh session).
    std::string WrapEnvelope(std::string_view kind, std::string_view payload)
    {
        std::string out = R"({"baseRevision":0,"kind":")";
        out += kind;
        out += R"(","payload":)";
        out += payload;
        out += "}";
        return out;
    }

    TEST(GraphAuthoringSessionTest, EmptyStoreProjectsAsGraph)
    {
        const SessionHandle s{""};
        auto                json = Compact(
            Das::Utils::SerializeYyjsonValue(DocumentOf(s.p)).value_or(""));
        // Phase 84 strict wire uses viewKind (not kind).
        EXPECT_NE(json.find("\"viewKind\":\"graph\""), std::string::npos);
        EXPECT_EQ(json.find("\"kind\":\"graph\""), std::string::npos);
    }

    TEST(GraphAuthoringSessionTest, FormSequenceSeedProjectsAsFormSequence)
    {
        // A 2-item formSequence context (full target so the DTO caster accepts
        // it).
        constexpr std::string_view item_a =
            R"({"itemId":"a","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":"{ga}","pluginGuid":"{p}"}},"settings":{}})";
        constexpr std::string_view item_b =
            R"({"itemId":"b","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":"{gb}","pluginGuid":"{p}"}},"settings":{}})";
        const SessionHandle s{
            std::string(R"({"documentId":"d1","version":0,"items":[)")
            + std::string(item_a) + "," + std::string(item_b) + "]}"};
        auto json = Compact(
            Das::Utils::SerializeYyjsonValue(DocumentOf(s.p)).value_or(""));
        EXPECT_NE(
            json.find("\"viewKind\":\"formSequence\""),
            std::string::npos);
        EXPECT_EQ(json.find("\"kind\":\"formSequence\""), std::string::npos);
        // formSequence.items[] is the canonical list (not .sequence).
        EXPECT_NE(json.find("\"items\":["), std::string::npos);
        EXPECT_NE(json.find("\"id\":\"a\""), std::string::npos);
        EXPECT_NE(json.find("\"id\":\"b\""), std::string::npos);
        // Private runtime fields do not leak.
        EXPECT_EQ(json.find("edgeType"), std::string::npos);
        // Public wire dropped raw settings (D-84-07).
        EXPECT_EQ(json.find("\"settings\""), std::string::npos);
    }

    // Seeding accepts the current contract doc shape: formSequence.items[]
    // (what SerializeDocument emits). The legacy sequence[] alias stays
    // accepted as the fallback — neither must seed an empty document.
    TEST(GraphAuthoringSessionTest, FormSequenceSeedAcceptsContractItemsShape)
    {
        constexpr std::string_view contract_doc = R"({
            "contractVersion": 1,
            "viewKind": "formSequence",
            "revision": 2,
            "formSequence": {
                "items": [
                    {"id": "x", "componentGuid": "{gx}", "targetKind": "componentRef"},
                    {"id": "y", "componentGuid": "{gy}", "targetKind": "componentRef"}
                ]
            }
        })";
        const SessionHandle        items_seeded{
            std::string(R"({"properties":)") + std::string(contract_doc) + "}"};
        auto items_json = Compact(
            Das::Utils::SerializeYyjsonValue(DocumentOf(items_seeded.p))
                .value_or(""));
        EXPECT_NE(
            items_json.find("\"viewKind\":\"formSequence\""),
            std::string::npos);
        EXPECT_NE(items_json.find("\"id\":\"x\""), std::string::npos);
        EXPECT_NE(items_json.find("\"id\":\"y\""), std::string::npos);

        constexpr std::string_view legacy_doc = R"({
            "contractVersion": 1,
            "viewKind": "formSequence",
            "revision": 2,
            "formSequence": {
                "sequence": [
                    {"id": "x", "componentGuid": "{gx}"},
                    {"id": "y", "componentGuid": "{gy}"}
                ]
            }
        })";
        const SessionHandle        legacy_seeded{
            std::string(R"({"properties":)") + std::string(legacy_doc) + "}"};
        auto legacy_json = Compact(
            Das::Utils::SerializeYyjsonValue(DocumentOf(legacy_seeded.p))
                .value_or(""));
        EXPECT_NE(legacy_json.find("\"id\":\"x\""), std::string::npos);
        EXPECT_NE(legacy_json.find("\"id\":\"y\""), std::string::npos);
    }

    TEST(GraphAuthoringSessionTest, GraphModeApplyChangeAddGraphNode)
    {
        const SessionHandle s{""}; // graph mode
        SetActionCatalog(s.p, R"([{"actionId":"{g1}"}])");
        auto r = ApplyChange(
            s.p,
            WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"));
        const auto result = Compact(JsonToUtf8(r.Get()));
        // Strict Phase 84 Apply result envelope: {acceptedProperties, document,
        // diagnostics}. document.revision == baseRevision + 1 == 1.
        EXPECT_NE(result.find("\"acceptedProperties\""), std::string::npos);
        EXPECT_NE(result.find("\"diagnostics\":[]"), std::string::npos);
        EXPECT_NE(result.find("\"revision\":1"), std::string::npos);

        auto doc = Compact(
            Das::Utils::SerializeYyjsonValue(DocumentOf(s.p)).value_or(""));
        // The newly-allocated node id must be present; addGraphNode now mints
        // a stable id instead of using a caller-supplied id.
        EXPECT_NE(doc.find("\"componentGuid\":\"{g1}\""), std::string::npos);
    }

    TEST(GraphAuthoringSessionTest, LegacyFlatOpFormatIsRejected)
    {
        // Only the unified {baseRevision, kind, payload} envelope is accepted;
        // the legacy {op, flat} shape is rejected as malformed.
        const SessionHandle s{""};
        auto                r = ApplyChange(
            s.p,
            R"({"op":"addGraphNode","node":{"id":"x","componentGuid":"{g1}","settings":{}}})");
        const auto result = Compact(JsonToUtf8(r.Get()));
        // Failure envelope carries an error diagnostic.
        EXPECT_NE(result.find("\"severity\":\"error\""), std::string::npos);
        EXPECT_NE(result.find("malformed"), std::string::npos);
    }

    TEST(GraphAuthoringSessionTest, UpgradeFlipsViewKindFromFormSequenceToGraph)
    {
        constexpr std::string_view item_a =
            R"({"itemId":"a","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":"{ga}","pluginGuid":"{p}"}},"settings":{}})";
        constexpr std::string_view item_b =
            R"({"itemId":"b","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":"{gb}","pluginGuid":"{p}"}},"settings":{}})";
        const SessionHandle s{
            std::string(R"({"items":[)") + std::string(item_a) + ","
            + std::string(item_b) + "]}"};
        EXPECT_NE(
            Compact(
                Das::Utils::SerializeYyjsonValue(DocumentOf(s.p)).value_or(""))
                .find("\"viewKind\":\"formSequence\""),
            std::string::npos);

        EXPECT_EQ(GraphAuthoringSessionUpgradeToGraph(s.p), DAS_S_OK);
        auto after = Compact(
            Das::Utils::SerializeYyjsonValue(DocumentOf(s.p)).value_or(""));
        EXPECT_NE(after.find("\"viewKind\":\"graph\""), std::string::npos);
        EXPECT_EQ(
            after.find("\"viewKind\":\"formSequence\""),
            std::string::npos);
        // Lossless: nodes preserved after upgrade.
        EXPECT_NE(after.find("\"id\":\"a\""), std::string::npos);
    }

    TEST(
        GraphAuthoringSessionTest,
        LinearModeApplyChangeAppendsFormSequenceItem)
    {
        constexpr std::string_view item_a =
            R"({"itemId":"a","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":"{ga}","pluginGuid":"{p}"}},"settings":{}})";
        const SessionHandle s{
            std::string(R"({"items":[)") + std::string(item_a) + "]}"};
        SetActionCatalog(s.p, R"([{"actionId":"{gb}"}])");
        auto r = ApplyChange(
            s.p,
            WrapEnvelope("addFormSequenceItem", R"({"actionId":"{gb}"})"));
        const auto result = Compact(JsonToUtf8(r.Get()));
        EXPECT_NE(result.find("\"revision\":1"), std::string::npos);

        auto doc = Compact(
            Das::Utils::SerializeYyjsonValue(DocumentOf(s.p)).value_or(""));
        // The original item id survived; the new item is allocated by the
        // Provider (caller does not supply an id).
        EXPECT_NE(doc.find("\"id\":\"a\""), std::string::npos);
    }

    // -----------------------------------------------------------------------
    // actionCatalog object form: entries are {actionId,label} objects; the
    // label comes from the component manifest definition's kind (carried by
    // the Provider context) and falls back to the actionId when absent.
    // -----------------------------------------------------------------------

    // Label of a catalog entry looked up by actionId inside
    // document.actionCatalog.entries; nullopt when the entry is absent, empty
    // when the entry carries no label field.
    std::optional<std::string> CatalogLabelOf(
        const yyjson::value& document,
        std::string_view     action_id)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("actionCatalog")))
        {
            return std::nullopt;
        }
        auto catalog = (*obj)[std::string_view("actionCatalog")].as_object();
        if (!catalog || !catalog->contains(std::string_view("entries")))
        {
            return std::nullopt;
        }
        auto entries = (*catalog)[std::string_view("entries")].as_array();
        if (!entries)
        {
            return std::nullopt;
        }
        for (const auto& entry : *entries)
        {
            auto entry_obj = entry.as_object();
            if (!entry_obj
                || !entry_obj->contains(std::string_view("actionId")))
            {
                continue;
            }
            auto id = (*entry_obj)[std::string_view("actionId")].as_string();
            if (id && *id == action_id)
            {
                if (!entry_obj->contains(std::string_view("label")))
                {
                    return std::string{};
                }
                return std::string((*entry_obj)[std::string_view("label")]
                                       .as_string()
                                       .value_or(""));
            }
        }
        return std::nullopt;
    }

    TEST(
        GraphAuthoringSessionTest,
        ActionCatalogObjectEntriesProjectLabelAndFallback)
    {
        const SessionHandle s{""};
        ASSERT_EQ(
            SetActionCatalog(
                s.p,
                R"([{"actionId":"{g1}","label":"das.flow.launch"},{"actionId":"{g2}"}])"),
            DAS_S_OK);
        const auto document = DocumentOf(s.p);

        // A labeled entry keeps its label on the GetDocument projection.
        const auto labeled = CatalogLabelOf(document, "{g1}");
        ASSERT_TRUE(labeled.has_value());
        EXPECT_EQ(*labeled, "das.flow.launch");

        // An entry without a label falls back to its actionId so the UI
        // always has a renderable value.
        const auto unlabeled = CatalogLabelOf(document, "{g2}");
        ASSERT_TRUE(unlabeled.has_value());
        EXPECT_EQ(*unlabeled, "{g2}");

        // The projected document (catalog included) stays valid strict wire.
        auto result =
            Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
        EXPECT_TRUE(result.valid)
            << result.error_kind << ": " << result.message;
    }

    TEST(GraphAuthoringSessionTest, ActionCatalogRejectsMalformedEntries)
    {
        const SessionHandle s{""};
        // Entry missing actionId.
        EXPECT_EQ(
            SetActionCatalog(s.p, R"([{"label":"x"}])"),
            DAS_E_INVALID_ARGUMENT);
        // Non-object entry: the legacy flat guid-string array form is gone.
        EXPECT_EQ(SetActionCatalog(s.p, R"(["{g1}"])"), DAS_E_INVALID_ARGUMENT);
        // Payload is not an array at all.
        EXPECT_EQ(
            SetActionCatalog(s.p, R"({"actionId":"{g1}"})"),
            DAS_E_INVALID_ARGUMENT);

        // Rejections leave the session untouched: no catalog is projected.
        const auto document = DocumentOf(s.p);
        auto       obj = document.as_object();
        ASSERT_TRUE(obj.has_value());
        EXPECT_FALSE(obj->contains(std::string_view("actionCatalog")));
    }

    // -----------------------------------------------------------------------
    // Node port projection through the session C ABI: the registry-declared
    // static port surface rides the actionCatalog channel (context entries
    // carry a per-entry "ports" array; the public actionCatalog wire keeps
    // the strict {actionId,label} shape) and merges onto nodes by
    // componentGuid == actionId.
    // -----------------------------------------------------------------------

    /// Ports array of the graph node carrying the given componentGuid inside
    /// a Get document; nullopt when the node or its "ports" key is absent.
    /// Empty graph sessions preset begin/end boundary nodes, so tests must
    /// address their own added nodes by componentGuid, never by position.
    std::optional<yyjson::value> NodePortsOfComponent(
        const yyjson::value& document,
        std::string_view     component_guid)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("graph")))
        {
            return std::nullopt;
        }
        auto graph = (*obj)[std::string_view("graph")].as_object();
        if (!graph || !graph->contains(std::string_view("nodes")))
        {
            return std::nullopt;
        }
        auto nodes = (*graph)[std::string_view("nodes")].as_array();
        if (!nodes || nodes->empty())
        {
            return std::nullopt;
        }
        for (const auto& node : *nodes)
        {
            auto node_obj = node.as_object();
            if (!node_obj)
            {
                continue;
            }
            auto node_guid =
                (*node_obj)[std::string_view("componentGuid")].as_string();
            if (!node_guid || *node_guid != component_guid)
            {
                continue;
            }
            if (!node_obj->contains(std::string_view("ports")))
            {
                return std::nullopt;
            }
            return Das::Utils::CloneYyjsonValue(
                (*node_obj)[std::string_view("ports")]);
        }
        return std::nullopt;
    }

    TEST(GraphAuthoringSessionTest, NodePortsProjectThroughSessionCatalog)
    {
        const SessionHandle s{""}; // graph mode
        // The registry snapshot form the Scheduler injects into the context:
        // {actionId,label} entries plus the per-entry static port surface
        // read from the component manifest definition
        // (inputs → "input", outputs → "output").
        ASSERT_EQ(
            SetActionCatalog(
                s.p,
                R"json([{"actionId":"{g1}","label":"das.flow.branch","ports":[{"portId":"condition","portType":"bool","direction":"input"},{"portId":"true","portType":"signal","direction":"output"}]}])json"),
            DAS_S_OK);

        // The node is minted from the catalog entry, so its componentGuid is
        // the actionId the static ports merge onto.
        ApplyChange(
            s.p,
            WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"));

        const auto document = DocumentOf(s.p);

        // Static ports surface on the node in the declared order.
        const auto ports =
            NodePortsOfComponent(document, std::string_view("{g1}"));
        ASSERT_TRUE(ports.has_value());
        auto arr = ports->as_array();
        ASSERT_TRUE(arr.has_value());
        ASSERT_EQ(arr->size(), 2u);
        auto first = (*arr)[0].as_object();
        ASSERT_TRUE(first.has_value());
        EXPECT_TRUE(first->contains(std::string_view("portId")));
        EXPECT_TRUE(first->contains(std::string_view("displayLabel")));
        EXPECT_TRUE(first->contains(std::string_view("portType")));
        EXPECT_TRUE(first->contains(std::string_view("direction")));
        auto condition = (*first)[std::string_view("portId")].as_string();
        ASSERT_TRUE(condition.has_value());
        EXPECT_EQ(*condition, "condition");

        // The public actionCatalog wire keeps the strict {actionId,label}
        // shape — the port surface is node-level only, never catalog-level.
        const auto catalog_label = CatalogLabelOf(document, "{g1}");
        ASSERT_TRUE(catalog_label.has_value());
        auto doc_obj = document.as_object();
        ASSERT_TRUE(doc_obj.has_value());
        ASSERT_TRUE(doc_obj->contains(std::string_view("actionCatalog")));
        auto catalog_json = Das::Utils::SerializeYyjsonValue(
            (*doc_obj)[std::string_view("actionCatalog")]);
        ASSERT_TRUE(catalog_json.has_value());
        EXPECT_EQ(catalog_json->find("portId"), std::string::npos);

        // The ports-bearing document stays valid strict wire.
        auto result =
            Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
        EXPECT_TRUE(result.valid)
            << result.error_kind << ": " << result.message;
    }

    // A malformed static-port entry (bad direction / unknown key / non-object)
    // rejects the whole catalog — the same all-or-nothing contract as the
    // {actionId,label} shape. An empty portType rejects too: the consumer
    // stays the all-or-nothing backstop behind the producer's skip rule.
    TEST(GraphAuthoringSessionTest, ActionCatalogRejectsMalformedPortEntries)
    {
        const SessionHandle s{""};
        EXPECT_EQ(
            SetActionCatalog(
                s.p,
                R"json([{"actionId":"{g1}","ports":[{"portId":"x","portType":"bool","direction":"sideways"}]}])json"),
            DAS_E_INVALID_ARGUMENT);
        EXPECT_EQ(
            SetActionCatalog(
                s.p,
                R"json([{"actionId":"{g1}","ports":[{"portId":"x","portType":"","direction":"input"}]}])json"),
            DAS_E_INVALID_ARGUMENT);
        EXPECT_EQ(
            SetActionCatalog(
                s.p,
                R"json([{"actionId":"{g1}","ports":[{"portId":"x","portType":"bool","direction":"input","extra":1}]}])json"),
            DAS_E_INVALID_ARGUMENT);
        EXPECT_EQ(
            SetActionCatalog(
                s.p,
                R"json([{"actionId":"{g1}","ports":["x"]}])json"),
            DAS_E_INVALID_ARGUMENT);
        EXPECT_EQ(
            SetActionCatalog(
                s.p,
                R"json([{"actionId":"{g1}","ports":{"portId":"x"}}])json"),
            DAS_E_INVALID_ARGUMENT);
    }

    // The loopCarried declaration is the one extra key a port element may
    // carry: the scheduler emits it for loop-carried outputs (for.index) and
    // this entry-point whitelist must admit it or the whole catalog is
    // rejected — every Add-Node panel went empty when the validation side
    // drifted from the port-surface side. A boolean value passes (stored or
    // not is the wire's business — the surface channel re-reads the raw
    // text); a non-boolean value is a shape violation like any other.
    TEST(GraphAuthoringSessionTest, ActionCatalogAdmitsLoopCarriedPortKey)
    {
        const SessionHandle s{""};
        EXPECT_EQ(
            SetActionCatalog(
                s.p,
                R"json([{"actionId":"{g1}","ports":[{"portId":"index","portType":"int64","direction":"output","loopCarried":true}]}])json"),
            DAS_S_OK);
        EXPECT_EQ(
            SetActionCatalog(
                s.p,
                R"json([{"actionId":"{g1}","ports":[{"portId":"index","portType":"int64","direction":"output","loopCarried":"yes"}]}])json"),
            DAS_E_INVALID_ARGUMENT);
    }

    // Five-key port projection end to end through the C ABI session: the
    // registry snapshot carries the channel form (with displayLabel, as the
    // scheduler produces since the label backfill), and GetDocument projects
    // each node port with exactly the five locked keys — signal ports
    // category "execution" with a non-empty displayLabel, everything else
    // "data". This locks the wire shape the UI fixture consumes.
    TEST(GraphAuthoringSessionTest, NodePortsProjectFiveKeysEndToEnd)
    {
        const SessionHandle s{""}; // graph mode
        ASSERT_EQ(
            SetActionCatalog(
                s.p,
                R"json([{"actionId":"{g1}","label":"das.maapi.run","ports":[
                    {"portId":"executionInput","portType":"object","direction":"input","displayLabel":"Execution Input"},
                    {"portId":"completedTasks","portType":"array<string>","direction":"output","displayLabel":"Completed Tasks"},
                    {"portId":"succeeded","portType":"signal","direction":"output","displayLabel":"Succeeded"},
                    {"portId":"failed","portType":"signal","direction":"output","displayLabel":"Failed"}
                ]}])json"),
            DAS_S_OK);
        ApplyChange(
            s.p,
            WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"));

        const auto ports =
            NodePortsOfComponent(DocumentOf(s.p), std::string_view("{g1}"));
        ASSERT_TRUE(ports.has_value());
        auto arr = ports->as_array();
        ASSERT_TRUE(arr.has_value());
        ASSERT_EQ(arr->size(), 4u);

        const auto category_of = [](const yyjson::value& port) -> std::string
        {
            auto port_obj = port.as_object();
            if (!port_obj || !port_obj->contains(std::string_view("category")))
            {
                return {};
            }
            return std::string(
                (*port_obj)[std::string_view("category")].as_string().value_or(
                    ""));
        };
        const auto label_of = [](const yyjson::value& port) -> std::string
        {
            auto port_obj = port.as_object();
            if (!port_obj
                || !port_obj->contains(std::string_view("displayLabel")))
            {
                return {};
            }
            return std::string((*port_obj)[std::string_view("displayLabel")]
                                   .as_string()
                                   .value_or(""));
        };

        for (const auto& port : *arr)
        {
            auto port_obj = port.as_object();
            ASSERT_TRUE(port_obj.has_value());
            // Exactly the five locked keys, category included.
            EXPECT_EQ(port_obj->size(), 5u);
            EXPECT_TRUE(port_obj->contains(std::string_view("portId")));
            EXPECT_TRUE(port_obj->contains(std::string_view("displayLabel")));
            EXPECT_TRUE(port_obj->contains(std::string_view("portType")));
            EXPECT_TRUE(port_obj->contains(std::string_view("direction")));
            EXPECT_TRUE(port_obj->contains(std::string_view("category")));
            EXPECT_FALSE(label_of(port).empty());
        }

        // Data ports carry the object/array<string> types with category
        // "data"; signal ports project "execution".
        EXPECT_EQ(category_of((*arr)[0]), "data");
        EXPECT_EQ(category_of((*arr)[1]), "data");
        EXPECT_EQ(category_of((*arr)[2]), "execution");
        EXPECT_EQ(category_of((*arr)[3]), "execution");
        EXPECT_EQ(label_of((*arr)[2]), "Succeeded");

        // The five-key document stays valid strict wire.
        auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(
            DocumentOf(s.p));
        EXPECT_TRUE(result.valid)
            << result.error_kind << ": " << result.message;
    }

    // -----------------------------------------------------------------------
    // E2E: store round-trips through the scheduler contract
    // (context.properties <-> ApplyChange.acceptedProperties)
    // -----------------------------------------------------------------------

    /// Extract the acceptedProperties blob from an ApplyChange result.
    std::string ExtractAcceptedProperties(Das::ExportInterface::IDasJson* r)
    {
        auto full = JsonToUtf8(r);
        auto obj = yyjson::read(full).as_object();
        if (!obj || !obj->contains(std::string_view("acceptedProperties")))
        {
            return {};
        }
        auto v = Das::Utils::SerializeYyjsonValue(
            (*obj)[std::string_view("acceptedProperties")]);
        return v.value_or(std::string{});
    }

    TEST(GraphAuthoringSessionTest, E2E_StoreRoundTripsThroughProperties)
    {
        // Start empty (graph mode), add a node, take the acceptedProperties the
        // scheduler would persist, and re-seed a fresh session from it — the
        // node must survive the round-trip.
        const SessionHandle s1{""};
        SetActionCatalog(s1.p, R"([{"actionId":"{g1}"},{"actionId":"{g2}"}])");
        ASSERT_NE(
            Compact(
                JsonToUtf8(
                    ApplyChange(
                        s1.p,
                        WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"))
                        .Get()))
                .find("\"revision\":1"),
            std::string::npos);

        const auto persisted = ExtractAcceptedProperties(
            ApplyChange(
                s1.p,
                WrapEnvelope("addGraphNode", R"({"actionId":"{g2}"})"))
                .Get());
        ASSERT_FALSE(persisted.empty());

        // Re-seed from the persisted source blob (wrapped as scheduler
        // context).
        const std::string ctx =
            std::string(R"({"properties":)") + persisted + "}";
        const SessionHandle s2{ctx};
        const auto          doc = Compact(
            Das::Utils::SerializeYyjsonValue(DocumentOf(s2.p)).value_or(""));
        EXPECT_NE(doc.find("\"componentGuid\":\"{g1}\""), std::string::npos);
        EXPECT_NE(doc.find("\"componentGuid\":\"{g2}\""), std::string::npos);
        EXPECT_NE(doc.find("\"viewKind\":\"graph\""), std::string::npos);
    }

    TEST(GraphAuthoringSessionTest, E2E_CompileProducesExecutablePlan)
    {
        // Seed a graph store, Compile, and confirm the plan is well-formed
        // (object with the compiled-plan shape; execution itself is exercised
        // by DasGraphTaskTest / GraphRuntimeTest).
        const SessionHandle s{""};
        SetActionCatalog(s.p, R"([{"actionId":"{g1}"}])");
        ApplyChange(
            s.p,
            WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"));

        DasPtr<Das::ExportInterface::IDasJson> plan_json;
        ASSERT_EQ(
            GraphAuthoringSessionCompile(s.p, nullptr, plan_json.Put()),
            DAS_S_OK);
        ASSERT_NE(plan_json.Get(), nullptr);
        const auto plan = Compact(JsonToUtf8(plan_json.Get()));
        // CompiledGraphPlanDto serialises with these keys.
        EXPECT_NE(plan.find("executionOrder"), std::string::npos);
    }

    // 会话编译端口面注入：catalog 声明的静态端口面（无 factory manager 绑
    // 定的 authoring 编译路径）驱动边界端口推导——未连线的声明输入口生成
    // $graph_input broadcast 绑定、边界口清单进编译产物。
    TEST(GraphAuthoringSessionTest, CompileDerivesBoundaryFromSessionCatalog)
    {
        const SessionHandle s{""};
        ASSERT_EQ(
            SetActionCatalog(
                s.p,
                R"json([{"actionId":"{g1}","ports":[{"portId":"seed","portType":"int","direction":"input"},{"portId":"result","portType":"string","direction":"output"}]}])json"),
            DAS_S_OK);
        ASSERT_NE(
            Compact(
                JsonToUtf8(
                    ApplyChange(
                        s.p,
                        WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"))
                        .Get()))
                .find("\"revision\":1"),
            std::string::npos);

        DasPtr<Das::ExportInterface::IDasJson> plan_json;
        ASSERT_EQ(
            GraphAuthoringSessionCompile(s.p, nullptr, plan_json.Put()),
            DAS_S_OK);
        ASSERT_NE(plan_json.Get(), nullptr);

        const auto root = yyjson::read(JsonToUtf8(plan_json.Get())).as_object();
        ASSERT_TRUE(root.has_value());
        const auto compile_section =
            (*root)[std::string_view("compile")].as_object();
        ASSERT_TRUE(compile_section.has_value());
        const auto shell =
            (*compile_section)[std::string_view("executionInput")].as_object();
        ASSERT_TRUE(shell.has_value());
        const auto plan_obj = (*shell)[std::string_view("plan")].as_object();
        ASSERT_TRUE(plan_obj.has_value());

        // 未连线声明输入口（seed）→ 推导 $graph_input broadcast 绑定。
        const auto binding_plan =
            (*plan_obj)[std::string_view("bindingPlan")].as_object();
        ASSERT_TRUE(binding_plan.has_value());
        const auto bindings =
            (*binding_plan)[std::string_view("bindings")].as_array();
        ASSERT_TRUE(bindings.has_value());
        bool seed_broadcast_found = false;
        for (const auto& binding : *bindings)
        {
            auto binding_obj = binding.as_object();
            ASSERT_TRUE(binding_obj.has_value());
            if ((*binding_obj)[std::string_view("sourceNodeId")]
                        .as_string()
                        .value_or("")
                    == std::string_view("$graph_input")
                && (*binding_obj)[std::string_view("targetPortId")]
                           .as_string()
                           .value_or("")
                       == std::string_view("seed"))
            {
                seed_broadcast_found = true;
                break;
            }
        }
        EXPECT_TRUE(seed_broadcast_found);

        // 边界口清单进编译产物（输入 seed / 输出 result）。
        const auto boundary_ports =
            (*plan_obj)[std::string_view("boundaryPorts")].as_object();
        ASSERT_TRUE(boundary_ports.has_value());
        const auto inputs =
            (*boundary_ports)[std::string_view("inputs")].as_array();
        ASSERT_TRUE(inputs.has_value());
        ASSERT_FALSE(inputs->empty());
        auto first_input = (*inputs)[0].as_object();
        ASSERT_TRUE(first_input.has_value());
        EXPECT_EQ(
            (*first_input)[std::string_view("portId")].as_string().value_or(""),
            std::string_view("seed"));
        const auto outputs =
            (*boundary_ports)[std::string_view("outputs")].as_array();
        ASSERT_TRUE(outputs.has_value());
        ASSERT_FALSE(outputs->empty());
        auto first_output = (*outputs)[0].as_object();
        ASSERT_TRUE(first_output.has_value());
        EXPECT_EQ(
            (*first_output)[std::string_view("portId")].as_string().value_or(
                ""),
            std::string_view("result"));
    }

    TEST(GraphAuthoringSessionTest, E2E_UpgradeIsLossless)
    {
        // Seed linear, capture nodes, upgrade (lossless), and verify the
        // viewKind flips without dropping node ids.
        constexpr std::string_view item_a =
            R"({"itemId":"a","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":"{ga}","pluginGuid":"{p}"}},"settings":{}})";
        constexpr std::string_view item_b =
            R"({"itemId":"b","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":"{gb}","pluginGuid":"{p}"}},"settings":{}})";
        const SessionHandle s{
            std::string(R"({"items":[)") + std::string(item_a) + ","
            + std::string(item_b) + "]}"};

        // Lossless upgrade: viewKind flips to graph, nodes preserved.
        ASSERT_EQ(GraphAuthoringSessionUpgradeToGraph(s.p), DAS_S_OK);
        const auto after_upgrade = Compact(
            Das::Utils::SerializeYyjsonValue(DocumentOf(s.p)).value_or(""));
        EXPECT_EQ(
            after_upgrade.find("\"viewKind\":\"formSequence\""),
            std::string::npos);
        EXPECT_NE(
            after_upgrade.find("\"viewKind\":\"graph\""),
            std::string::npos);
        EXPECT_NE(after_upgrade.find("\"id\":\"a\""), std::string::npos);
        EXPECT_NE(after_upgrade.find("\"id\":\"b\""), std::string::npos);
    }

    // -----------------------------------------------------------------------
    // Plan 01 validator cross-check: the public GraphRuntime wire must satisfy
    // the strict TaskScheduler AuthoringDocument validator.
    // -----------------------------------------------------------------------

    // Production-path regression: GraphAuthoringSessionGetDocument now emits
    // the strict Phase 84 envelope {document, diagnostics}. The envelope must
    // pass ValidateAuthoringGetResult (which requires top-level diagnostics
    // array) AND the nested document must still satisfy the document
    // validator. This locks the wire shape that SchedulerService relies on —
    // a previous regression returned a bare document and the scheduler failed
    // with "AuthoringGetResult.diagnostics required".
    TEST(GraphAuthoringSessionTest, ValidatorAccepts_FormSequenceOutput)
    {
        constexpr std::string_view item_a =
            R"({"itemId":"a","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":"{ga}","pluginGuid":"{p}"}},"settings":{}})";
        const SessionHandle s{
            std::string(R"({"items":[)") + std::string(item_a) + "]}"};
        const auto envelope_json = JsonToUtf8(GetDocument(s.p).Get());
        auto       parsed = yyjson::read(envelope_json);
        auto       envelope_result =
            Das::Core::TaskScheduler::ValidateAuthoringGetResult(parsed);
        EXPECT_TRUE(envelope_result.valid)
            << envelope_result.error_kind << ": " << envelope_result.message;
        auto doc_result = Das::Core::TaskScheduler::ValidateAuthoringDocument(
            DocumentOf(s.p));
        EXPECT_TRUE(doc_result.valid)
            << doc_result.error_kind << ": " << doc_result.message;
    }

    TEST(GraphAuthoringSessionTest, ValidatorAccepts_GraphOutput)
    {
        const SessionHandle s{""};
        SetActionCatalog(s.p, R"([{"actionId":"{g1}"}])");
        ApplyChange(
            s.p,
            WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"));
        const auto envelope_json = JsonToUtf8(GetDocument(s.p).Get());
        auto       parsed = yyjson::read(envelope_json);
        auto       envelope_result =
            Das::Core::TaskScheduler::ValidateAuthoringGetResult(parsed);
        EXPECT_TRUE(envelope_result.valid)
            << envelope_result.error_kind << ": " << envelope_result.message;
        auto doc_result = Das::Core::TaskScheduler::ValidateAuthoringDocument(
            DocumentOf(s.p));
        EXPECT_TRUE(doc_result.valid)
            << doc_result.error_kind << ": " << doc_result.message;
    }

    TEST(GraphAuthoringSessionTest, ValidatorAccepts_ApplyResultEnvelope)
    {
        const SessionHandle s{""};
        SetActionCatalog(s.p, R"([{"actionId":"{g1}"}])");
        auto apply_json = JsonToUtf8(
            ApplyChange(
                s.p,
                WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"))
                .Get());
        auto parsed = yyjson::read(apply_json);
        // baseRevision == 0, so success document.revision must equal 1.
        auto result = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(
            parsed,
            /*baseRevision=*/0);
        EXPECT_TRUE(result.valid)
            << result.error_kind << ": " << result.message;
    }

    // -----------------------------------------------------------------------
    // moveGraphNode — C ABI end-to-end (wire envelope -> viewData -> Get)
    // -----------------------------------------------------------------------

    /// Read viewData.positions[node_id].axis from a GetDocument document
    /// blob; nullopt when the partition, the node entry, or the axis is
    /// absent.
    std::optional<double> DocumentViewPositionOf(
        const yyjson::value& document,
        std::string_view     node_id,
        std::string_view     axis)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("viewData")))
        {
            return std::nullopt;
        }
        auto view = (*obj)[std::string_view("viewData")].as_object();
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

    /// Root revision of a GetDocument document blob; nullopt when absent.
    std::optional<int64_t> DocumentRevisionOf(const yyjson::value& document)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("revision")))
        {
            return std::nullopt;
        }
        return (*obj)[std::string_view("revision")].as_int();
    }

    /// Id of the first graph node carrying the given componentGuid in the
    /// document's graph projection; nullopt when no such node exists. Empty
    /// graph sessions preset begin/end boundary nodes, so tests must address
    /// their own added nodes by componentGuid, never by position.
    std::optional<std::string> FirstGraphNodeOf(
        const yyjson::value& document,
        std::string_view     component_guid)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("graph")))
        {
            return std::nullopt;
        }
        auto graph = (*obj)[std::string_view("graph")].as_object();
        if (!graph || !graph->contains(std::string_view("nodes")))
        {
            return std::nullopt;
        }
        auto nodes = (*graph)[std::string_view("nodes")].as_array();
        if (!nodes || nodes->size() == 0)
        {
            return std::nullopt;
        }
        for (const auto& node : *nodes)
        {
            auto node_obj = node.as_object();
            if (!node_obj || !node_obj->contains(std::string_view("id")))
            {
                continue;
            }
            auto node_guid =
                (*node_obj)[std::string_view("componentGuid")].as_string();
            if (!node_guid || *node_guid != component_guid)
            {
                continue;
            }
            auto id = (*node_obj)[std::string_view("id")].as_string();
            if (!id)
            {
                return std::nullopt;
            }
            return std::string(*id);
        }
        return std::nullopt;
    }

    // Optional port default-value channel end to end through the C ABI
    // session: an input data port whose node settings carry a matching key
    // projects the raw value verbatim; a settings-less port, a signal input
    // and the outputs (even with a same-named settings key) keep the key
    // absent. Locks the wire shape the UI consumes for editing unconnected
    // input defaults (branch.condition-style ports).
    TEST(GraphAuthoringSessionTest, InputDataPortsCarryOptionalValueKey)
    {
        const SessionHandle s{""}; // graph mode
        ASSERT_EQ(
            SetActionCatalog(
                s.p,
                R"json([{"actionId":"{g1}","label":"das.flow.branch","ports":[
                    {"portId":"condition","portType":"bool","direction":"input"},
                    {"portId":"in","portType":"signal","direction":"input"},
                    {"portId":"true","portType":"signal","direction":"output"},
                    {"portId":"index","portType":"int","direction":"output"}
                ]}])json"),
            DAS_S_OK);
        ASSERT_EQ(
            Compact(
                JsonToUtf8(
                    ApplyChange(
                        s.p,
                        WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"))
                        .Get()))
                .find("\"severity\":\"error\""),
            std::string::npos)
            << "addGraphNode must succeed";
        const auto node_id =
            FirstGraphNodeOf(DocumentOf(s.p), std::string_view("{g1}"));
        ASSERT_TRUE(node_id.has_value());

        const auto port_of =
            [&s](std::string_view port_id) -> std::optional<yyjson::value>
        {
            const auto ports =
                NodePortsOfComponent(DocumentOf(s.p), std::string_view("{g1}"));
            if (!ports.has_value())
            {
                return std::nullopt;
            }
            auto arr = ports->as_array();
            if (!arr.has_value())
            {
                return std::nullopt;
            }
            for (const auto& port : *arr)
            {
                auto port_obj = port.as_object();
                if (!port_obj)
                {
                    continue;
                }
                auto id = (*port_obj)[std::string_view("portId")].as_string();
                if (id.has_value() && *id == port_id)
                {
                    return Das::Utils::CloneYyjsonValue(port);
                }
            }
            return std::nullopt;
        };

        // Before any setValue the settings are empty: every port stays at
        // exactly the five locked keys — the value key is absent, not null.
        const auto bare_condition = port_of("condition");
        ASSERT_TRUE(bare_condition.has_value());
        auto bare_obj = bare_condition->as_object();
        ASSERT_TRUE(bare_obj.has_value());
        EXPECT_EQ(bare_obj->size(), 5u);
        EXPECT_FALSE(bare_obj->contains(std::string_view("value")));

        // setValue lands in node.settings: condition mirrors its raw bound
        // value; the same-named keys written for a signal input and an
        // output must NOT surface a value on those ports.
        const auto set_value =
            [&](std::string_view field_name, std::string_view raw_value)
        {
            const std::string payload =
                std::string(R"({"target":{"kind":"graphNode","id":")")
                + *node_id + R"("},"fieldName":")" + std::string(field_name)
                + R"(","value":)" + std::string(raw_value) + "}";
            const auto apply = Compact(JsonToUtf8(
                ApplyChange(s.p, WrapEnvelope("setValue", payload)).Get()));
            EXPECT_FALSE(apply.empty());
            EXPECT_EQ(apply.find("\"severity\":\"error\""), std::string::npos)
                << apply;
        };
        set_value("condition", "true");
        set_value("in", "false");
        set_value("true", "42");

        const auto condition = port_of("condition");
        ASSERT_TRUE(condition.has_value());
        auto condition_obj = condition->as_object();
        ASSERT_TRUE(condition_obj.has_value());
        EXPECT_EQ(condition_obj->size(), 6u);
        ASSERT_TRUE(condition_obj->contains(std::string_view("value")));
        EXPECT_EQ(
            (*condition_obj)[std::string_view("value")].as_bool(),
            std::optional<bool>{true});

        const auto signal_input = port_of("in");
        ASSERT_TRUE(signal_input.has_value());
        auto signal_input_obj = signal_input->as_object();
        ASSERT_TRUE(signal_input_obj.has_value());
        EXPECT_FALSE(signal_input_obj->contains(std::string_view("value")));

        const auto signal_output = port_of("true");
        ASSERT_TRUE(signal_output.has_value());
        auto signal_output_obj = signal_output->as_object();
        ASSERT_TRUE(signal_output_obj.has_value());
        EXPECT_FALSE(signal_output_obj->contains(std::string_view("value")));

        const auto data_output = port_of("index");
        ASSERT_TRUE(data_output.has_value());
        auto data_output_obj = data_output->as_object();
        ASSERT_TRUE(data_output_obj.has_value());
        EXPECT_FALSE(data_output_obj->contains(std::string_view("value")));

        // The value-bearing document stays valid strict wire (validator and
        // schema treat the key as optional).
        auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(
            DocumentOf(s.p));
        EXPECT_TRUE(result.valid)
            << result.error_kind << ": " << result.message;
    }

    /// Build the moveGraphNode payload {nodeId, position:{x,y}} for the
    /// strict envelope.
    std::string MakeMovePayload(std::string_view node_id, double x, double y)
    {
        return std::string(R"({"nodeId":")") + std::string(node_id)
               + R"(","position":{"x":)" + std::to_string(x) + R"(,"y":)"
               + std::to_string(y) + R"(}})";
    }

    TEST(GraphAuthoringSessionTest, MoveGraphNodeEndToEndWritesViewData)
    {
        const SessionHandle s{""};
        SetActionCatalog(s.p, R"([{"actionId":"{g1}"}])");
        ASSERT_EQ(
            Compact(
                JsonToUtf8(
                    ApplyChange(
                        s.p,
                        WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"))
                        .Get()))
                .find("\"severity\":\"error\""),
            std::string::npos)
            << "addGraphNode must succeed";
        const auto node_id =
            FirstGraphNodeOf(DocumentOf(s.p), std::string_view("{g1}"));
        ASSERT_TRUE(node_id.has_value());

        // Wire coordinates formatted as integers exercise the as_num path.
        const auto apply =
            Compact(JsonToUtf8(ApplyChange(
                                   s.p,
                                   WrapEnvelope(
                                       "moveGraphNode",
                                       MakeMovePayload(*node_id, 120.5, 88.0)))
                                   .Get()));
        EXPECT_EQ(apply.find("\"severity\":\"error\""), std::string::npos)
            << apply;

        const auto document = DocumentOf(s.p);
        ASSERT_TRUE(
            DocumentViewPositionOf(document, *node_id, "x").has_value());
        EXPECT_DOUBLE_EQ(
            *DocumentViewPositionOf(document, *node_id, "x"),
            120.5);
        ASSERT_TRUE(
            DocumentViewPositionOf(document, *node_id, "y").has_value());
        EXPECT_DOUBLE_EQ(
            *DocumentViewPositionOf(document, *node_id, "y"),
            88.0);
    }

    TEST(GraphAuthoringSessionTest, MoveGraphNodeOverwritesExistingPosition)
    {
        const SessionHandle s{""};
        SetActionCatalog(s.p, R"([{"actionId":"{g1}"}])");
        ApplyChange(
            s.p,
            WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"));
        const auto node_id =
            FirstGraphNodeOf(DocumentOf(s.p), std::string_view("{g1}"));
        ASSERT_TRUE(node_id.has_value());

        ApplyChange(
            s.p,
            WrapEnvelope("moveGraphNode", MakeMovePayload(*node_id, 1.0, 2.0)));
        ApplyChange(
            s.p,
            WrapEnvelope("moveGraphNode", MakeMovePayload(*node_id, 9.0, 9.0)));

        const auto document = DocumentOf(s.p);
        ASSERT_TRUE(
            DocumentViewPositionOf(document, *node_id, "x").has_value());
        EXPECT_DOUBLE_EQ(*DocumentViewPositionOf(document, *node_id, "x"), 9.0);
        ASSERT_TRUE(
            DocumentViewPositionOf(document, *node_id, "y").has_value());
        EXPECT_DOUBLE_EQ(*DocumentViewPositionOf(document, *node_id, "y"), 9.0);
    }

    TEST(GraphAuthoringSessionTest, MoveGraphNodeKeepsRevisionStable)
    {
        const SessionHandle s{""};
        SetActionCatalog(s.p, R"([{"actionId":"{g1}"}])");
        ApplyChange(
            s.p,
            WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"));
        const auto node_id =
            FirstGraphNodeOf(DocumentOf(s.p), std::string_view("{g1}"));
        ASSERT_TRUE(node_id.has_value());
        const auto revision_before = DocumentRevisionOf(DocumentOf(s.p));
        ASSERT_TRUE(revision_before.has_value());
        ASSERT_GT(*revision_before, 0);

        const auto apply =
            Compact(JsonToUtf8(ApplyChange(
                                   s.p,
                                   WrapEnvelope(
                                       "moveGraphNode",
                                       MakeMovePayload(*node_id, 3.0, 4.0)))
                                   .Get()));
        EXPECT_EQ(apply.find("\"severity\":\"error\""), std::string::npos)
            << apply;

        // The lightweight coordinate channel leaves the revision untouched.
        const auto revision_after = DocumentRevisionOf(DocumentOf(s.p));
        ASSERT_TRUE(revision_after.has_value());
        EXPECT_EQ(*revision_after, *revision_before);
    }

    TEST(GraphAuthoringSessionTest, MoveGraphNodeSurvivesReseedFromProperties)
    {
        // The scheduler persists acceptedProperties and re-seeds sessions
        // from it — the viewData partition must survive that round-trip.
        const SessionHandle s1{""};
        SetActionCatalog(s1.p, R"([{"actionId":"{g1}"}])");
        ApplyChange(
            s1.p,
            WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"));
        const auto node_id =
            FirstGraphNodeOf(DocumentOf(s1.p), std::string_view("{g1}"));
        ASSERT_TRUE(node_id.has_value());

        const auto persisted = ExtractAcceptedProperties(
            ApplyChange(
                s1.p,
                WrapEnvelope(
                    "moveGraphNode",
                    MakeMovePayload(*node_id, 7.5, 12.25)))
                .Get());
        ASSERT_FALSE(persisted.empty());

        const std::string context =
            std::string(R"({"properties":)") + persisted + "}";
        const SessionHandle s2{context};
        const auto          document = DocumentOf(s2.p);
        ASSERT_TRUE(
            DocumentViewPositionOf(document, *node_id, "x").has_value());
        EXPECT_DOUBLE_EQ(*DocumentViewPositionOf(document, *node_id, "x"), 7.5);
        ASSERT_TRUE(
            DocumentViewPositionOf(document, *node_id, "y").has_value());
        EXPECT_DOUBLE_EQ(
            *DocumentViewPositionOf(document, *node_id, "y"),
            12.25);
    }

    // -----------------------------------------------------------------------
    // removeGraphNodes / moveGraphNodes — C ABI end-to-end (one gesture =
    // one batch message through the session shell)
    // -----------------------------------------------------------------------

    /// All node ids in a GetDocument document blob's graph projection. When a
    /// component_guid filter is given, only nodes carrying it are collected —
    /// empty graph sessions preset begin/end boundary nodes, so batch tests
    /// scope to their own added nodes.
    std::vector<std::string> DocumentGraphNodeIds(
        const yyjson::value& document,
        std::string_view     component_guid = {})
    {
        std::vector<std::string> ids;
        auto                     obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("graph")))
        {
            return ids;
        }
        auto graph = (*obj)[std::string_view("graph")].as_object();
        if (!graph || !graph->contains(std::string_view("nodes")))
        {
            return ids;
        }
        auto nodes = (*graph)[std::string_view("nodes")].as_array();
        if (!nodes)
        {
            return ids;
        }
        for (const auto& node : *nodes)
        {
            auto node_obj = node.as_object();
            if (node_obj && node_obj->contains(std::string_view("id")))
            {
                if (!component_guid.empty())
                {
                    auto node_guid =
                        (*node_obj)[std::string_view("componentGuid")]
                            .as_string();
                    if (!node_guid || *node_guid != component_guid)
                    {
                        continue;
                    }
                }
                if (auto id = (*node_obj)[std::string_view("id")].as_string())
                {
                    ids.emplace_back(*id);
                }
            }
        }
        return ids;
    }

    /// All nodeId strings inside an acceptedProperties source blob (the
    /// internal GraphDocumentDto wire). Same optional component_guid filter —
    /// the source blob nests the guid under target.componentRef.
    std::vector<std::string> SourceGraphNodeIds(
        const yyjson::value& source,
        std::string_view     component_guid = {})
    {
        std::vector<std::string> ids;
        auto                     obj = source.as_object();
        if (!obj || !obj->contains(std::string_view("nodes")))
        {
            return ids;
        }
        auto nodes = (*obj)[std::string_view("nodes")].as_array();
        if (!nodes)
        {
            return ids;
        }
        for (const auto& node : *nodes)
        {
            auto node_obj = node.as_object();
            if (node_obj && node_obj->contains(std::string_view("nodeId")))
            {
                if (!component_guid.empty())
                {
                    auto target =
                        (*node_obj)[std::string_view("target")].as_object();
                    if (!target)
                    {
                        continue;
                    }
                    auto component_ref =
                        (*target)[std::string_view("componentRef")].as_object();
                    if (!component_ref)
                    {
                        continue;
                    }
                    auto node_guid =
                        (*component_ref)[std::string_view("componentGuid")]
                            .as_string();
                    if (!node_guid || *node_guid != component_guid)
                    {
                        continue;
                    }
                }
                if (auto id =
                        (*node_obj)[std::string_view("nodeId")].as_string())
                {
                    ids.emplace_back(*id);
                }
            }
        }
        return ids;
    }

    /// Clone the acceptedProperties value out of an ApplyChange response.
    yyjson::value AcceptedPropertiesOf(Das::ExportInterface::IDasJson* response)
    {
        auto full = JsonToUtf8(response);
        auto obj = yyjson::read(full).as_object();
        if (!obj || !obj->contains(std::string_view("acceptedProperties")))
        {
            return Das::Utils::MakeYyjsonObject();
        }
        return Das::Utils::CloneYyjsonValue(
            (*obj)[std::string_view("acceptedProperties")]);
    }

    /// Build the removeGraphNodes payload {"nodeIds":[...]}.
    std::string MakeRemoveGraphNodesPayload(
        const std::vector<std::string>& node_ids)
    {
        std::string payload = R"({"nodeIds":[)";
        for (std::size_t index = 0; index < node_ids.size(); ++index)
        {
            if (index != 0)
            {
                payload += ',';
            }
            payload += "\"" + node_ids[index] + "\"";
        }
        payload += "]}";
        return payload;
    }

    /// Build the moveGraphNodes payload
    /// {"moves":[{"nodeId":..,"position":{"x":..,"y":..}},...]}.
    std::string MakeMoveGraphNodesPayload(
        const std::vector<std::tuple<std::string, double, double>>& moves)
    {
        std::string payload = R"({"moves":[)";
        for (std::size_t index = 0; index < moves.size(); ++index)
        {
            if (index != 0)
            {
                payload += ',';
            }
            const auto& [node_id, x, y] = moves[index];
            payload += "{\"nodeId\":\"" + node_id + R"(","position":{"x":)"
                       + std::to_string(x) + R"(,"y":)" + std::to_string(y)
                       + "}}";
        }
        payload += "]}";
        return payload;
    }

    TEST(GraphAuthoringSessionTest, RemoveGraphNodesEndToEndDropsBatch)
    {
        const SessionHandle s{""};
        // The declared data port surface the edge-type derivation resolves
        // through ("o" output / "i" input on {g1}).
        SetActionCatalog(
            s.p,
            R"([{"actionId":"{g1}","ports":[)"
            R"({"portId":"o","portType":"int","direction":"output"},)"
            R"({"portId":"i","portType":"int","direction":"input"}]}])");
        for (int node_count = 0; node_count < 3; ++node_count)
        {
            ApplyChange(
                s.p,
                WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"));
        }
        const auto node_ids =
            DocumentGraphNodeIds(DocumentOf(s.p), std::string_view("{g1}"));
        ASSERT_EQ(node_ids.size(), 3u);
        // Wire n0 -> n1 -> n2 so the batch removal also proves edge cascade.
        ApplyChange(
            s.p,
            WrapEnvelope(
                "connectGraphPorts",
                R"({"fromNodeId":")" + node_ids[0] + R"(","fromPortId":"o",)"
                    + R"("toNodeId":")" + node_ids[1]
                    + R"(","toPortId":"i"})"));
        ApplyChange(
            s.p,
            WrapEnvelope(
                "connectGraphPorts",
                R"({"fromNodeId":")" + node_ids[1] + R"(","fromPortId":"o",)"
                    + R"("toNodeId":")" + node_ids[2]
                    + R"(","toPortId":"i"})"));
        const auto base_revision = DocumentRevisionOf(DocumentOf(s.p));
        ASSERT_TRUE(base_revision.has_value());

        const auto response = ApplyChange(
            s.p,
            WrapEnvelope(
                "removeGraphNodes",
                MakeRemoveGraphNodesPayload({node_ids[0], node_ids[1]})));
        const auto apply = Compact(JsonToUtf8(response.Get()));
        EXPECT_EQ(apply.find("\"severity\":\"error\""), std::string::npos)
            << apply;

        // The document blob no longer carries the deleted nodes (nor any
        // trace of their ids — connections included); the preset begin/end
        // boundary nodes are outside the {g1} scope and survive untouched.
        const auto document = DocumentOf(s.p);
        const auto remaining =
            DocumentGraphNodeIds(document, std::string_view("{g1}"));
        ASSERT_EQ(remaining.size(), 1u);
        EXPECT_EQ(remaining[0], node_ids[2]);
        const auto document_text =
            Compact(Das::Utils::SerializeYyjsonValue(document).value_or(""));
        EXPECT_EQ(document_text.find(node_ids[0]), std::string::npos);
        EXPECT_EQ(document_text.find(node_ids[1]), std::string::npos);

        // The persisted source blob agrees, and the semantic revision moved
        // exactly once for the whole batch.
        const auto source_ids = SourceGraphNodeIds(
            AcceptedPropertiesOf(response.Get()),
            std::string_view("{g1}"));
        ASSERT_EQ(source_ids.size(), 1u);
        EXPECT_EQ(source_ids[0], node_ids[2]);
        const auto after_revision = DocumentRevisionOf(document);
        ASSERT_TRUE(after_revision.has_value());
        EXPECT_EQ(*after_revision, *base_revision + 1);
    }

    TEST(GraphAuthoringSessionTest, MoveGraphNodesEndToEndWritesBatch)
    {
        const SessionHandle s{""};
        SetActionCatalog(s.p, R"([{"actionId":"{g1}"}])");
        ApplyChange(
            s.p,
            WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"));
        ApplyChange(
            s.p,
            WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"));
        const auto node_ids =
            DocumentGraphNodeIds(DocumentOf(s.p), std::string_view("{g1}"));
        ASSERT_EQ(node_ids.size(), 2u);
        const auto revision_before = DocumentRevisionOf(DocumentOf(s.p));
        ASSERT_TRUE(revision_before.has_value());

        const auto response = ApplyChange(
            s.p,
            WrapEnvelope(
                "moveGraphNodes",
                MakeMoveGraphNodesPayload(
                    {{node_ids[0], 15.5, 16.0}, {node_ids[1], 25.0, 26.25}})));
        const auto apply = Compact(JsonToUtf8(response.Get()));
        EXPECT_EQ(apply.find("\"severity\":\"error\""), std::string::npos)
            << apply;

        // Both blobs (public document + persisted source) carry the batch
        // coordinates under viewData.positions.
        const auto document = DocumentOf(s.p);
        ASSERT_TRUE(
            DocumentViewPositionOf(document, node_ids[0], "x").has_value());
        EXPECT_DOUBLE_EQ(
            *DocumentViewPositionOf(document, node_ids[0], "x"),
            15.5);
        ASSERT_TRUE(
            DocumentViewPositionOf(document, node_ids[0], "y").has_value());
        EXPECT_DOUBLE_EQ(
            *DocumentViewPositionOf(document, node_ids[0], "y"),
            16.0);
        ASSERT_TRUE(
            DocumentViewPositionOf(document, node_ids[1], "x").has_value());
        EXPECT_DOUBLE_EQ(
            *DocumentViewPositionOf(document, node_ids[1], "x"),
            25.0);
        ASSERT_TRUE(
            DocumentViewPositionOf(document, node_ids[1], "y").has_value());
        EXPECT_DOUBLE_EQ(
            *DocumentViewPositionOf(document, node_ids[1], "y"),
            26.25);
        const auto source = AcceptedPropertiesOf(response.Get());
        ASSERT_TRUE(
            DocumentViewPositionOf(source, node_ids[0], "x").has_value());
        EXPECT_DOUBLE_EQ(
            *DocumentViewPositionOf(source, node_ids[0], "x"),
            15.5);
        ASSERT_TRUE(
            DocumentViewPositionOf(source, node_ids[1], "y").has_value());
        EXPECT_DOUBLE_EQ(
            *DocumentViewPositionOf(source, node_ids[1], "y"),
            26.25);

        // The lightweight coordinate channel leaves the revision untouched.
        const auto revision_after = DocumentRevisionOf(document);
        ASSERT_TRUE(revision_after.has_value());
        EXPECT_EQ(*revision_after, *revision_before);
    }

    // -----------------------------------------------------------------------
    // renameGraphNode — C ABI end-to-end: the user display label lands on
    // both blobs (public document + persisted source), the node id and the
    // semantic revision invariant hold, and the label survives a re-seed
    // from acceptedProperties (the scheduler's persistence round-trip).
    // -----------------------------------------------------------------------

    /// String field of the graph node carrying the given componentGuid in a
    /// document blob's graph projection; nullopt when the node or the key is
    /// absent. Empty graph sessions preset begin/end boundary nodes, so tests
    /// address their own added nodes by componentGuid, never by position.
    std::optional<std::string> FirstGraphNodeFieldOf(
        const yyjson::value& document,
        std::string_view     key,
        std::string_view     component_guid)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("graph")))
        {
            return std::nullopt;
        }
        auto graph = (*obj)[std::string_view("graph")].as_object();
        if (!graph || !graph->contains(std::string_view("nodes")))
        {
            return std::nullopt;
        }
        auto nodes = (*graph)[std::string_view("nodes")].as_array();
        if (!nodes || nodes->size() == 0)
        {
            return std::nullopt;
        }
        for (const auto& node : *nodes)
        {
            auto node_obj = node.as_object();
            if (!node_obj)
            {
                continue;
            }
            auto node_guid =
                (*node_obj)[std::string_view("componentGuid")].as_string();
            if (!node_guid || *node_guid != component_guid)
            {
                continue;
            }
            if (!node_obj->contains(key))
            {
                return std::nullopt;
            }
            auto value = (*node_obj)[key].as_string();
            if (!value)
            {
                return std::nullopt;
            }
            return std::string(*value);
        }
        return std::nullopt;
    }

    /// String field of the source-blob node carrying the given componentGuid
    /// (the internal GraphDocumentDto wire, camelCase Dto keys; the guid
    /// nests under target.componentRef); nullopt when absent.
    std::optional<std::string> FirstSourceNodeFieldOf(
        const yyjson::value& source,
        std::string_view     key,
        std::string_view     component_guid)
    {
        auto obj = source.as_object();
        if (!obj || !obj->contains(std::string_view("nodes")))
        {
            return std::nullopt;
        }
        auto nodes = (*obj)[std::string_view("nodes")].as_array();
        if (!nodes || nodes->size() == 0)
        {
            return std::nullopt;
        }
        for (const auto& node : *nodes)
        {
            auto node_obj = node.as_object();
            if (!node_obj)
            {
                continue;
            }
            auto target = (*node_obj)[std::string_view("target")].as_object();
            if (!target)
            {
                continue;
            }
            auto component_ref =
                (*target)[std::string_view("componentRef")].as_object();
            if (!component_ref)
            {
                continue;
            }
            auto node_guid =
                (*component_ref)[std::string_view("componentGuid")].as_string();
            if (!node_guid || *node_guid != component_guid)
            {
                continue;
            }
            if (!node_obj->contains(key))
            {
                return std::nullopt;
            }
            auto value = (*node_obj)[key].as_string();
            if (!value)
            {
                return std::nullopt;
            }
            return std::string(*value);
        }
        return std::nullopt;
    }

    TEST(GraphAuthoringSessionTest, RenameGraphNodeEndToEndSurvivesReseed)
    {
        const SessionHandle s1{""};
        SetActionCatalog(s1.p, R"([{"actionId":"{g1}"}])");
        ApplyChange(
            s1.p,
            WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"));
        const auto node_id =
            FirstGraphNodeOf(DocumentOf(s1.p), std::string_view("{g1}"));
        ASSERT_TRUE(node_id.has_value());
        const auto base_revision = DocumentRevisionOf(DocumentOf(s1.p));
        ASSERT_TRUE(base_revision.has_value());

        const auto response = ApplyChange(
            s1.p,
            WrapEnvelope(
                "renameGraphNode",
                R"({"nodeId":")" + *node_id + R"(","label":"我的延迟节点"})"));
        const auto apply = Compact(JsonToUtf8(response.Get()));
        EXPECT_EQ(apply.find("\"severity\":\"error\""), std::string::npos)
            << apply;

        // The public document blob carries the new label; the node id is
        // untouched (label is pure presentation, never an address); the
        // semantic revision moved exactly once.
        const auto document = DocumentOf(s1.p);
        const auto wire_label = FirstGraphNodeFieldOf(
            document,
            std::string_view("label"),
            std::string_view("{g1}"));
        ASSERT_TRUE(wire_label.has_value());
        EXPECT_EQ(*wire_label, "我的延迟节点");
        const auto wire_id = FirstGraphNodeFieldOf(
            document,
            std::string_view("id"),
            std::string_view("{g1}"));
        ASSERT_TRUE(wire_id.has_value());
        EXPECT_EQ(*wire_id, *node_id);
        const auto revision_after = DocumentRevisionOf(document);
        ASSERT_TRUE(revision_after.has_value());
        EXPECT_EQ(*revision_after, *base_revision + 1);

        // The persisted source blob (acceptedProperties) agrees.
        const auto source = AcceptedPropertiesOf(response.Get());
        const auto source_label = FirstSourceNodeFieldOf(
            source,
            std::string_view("label"),
            std::string_view("{g1}"));
        ASSERT_TRUE(source_label.has_value());
        EXPECT_EQ(*source_label, "我的延迟节点");
        const auto source_id = FirstSourceNodeFieldOf(
            source,
            std::string_view("nodeId"),
            std::string_view("{g1}"));
        ASSERT_TRUE(source_id.has_value());
        EXPECT_EQ(*source_id, *node_id);

        // Re-seed from acceptedProperties: the label survives the
        // scheduler's persistence round-trip.
        const std::string context =
            std::string(R"({"properties":)")
            + Das::Utils::SerializeYyjsonValue(source).value_or(std::string{})
            + "}";
        const SessionHandle s2{context};
        const auto          reseeded = DocumentOf(s2.p);
        const auto          reseeded_label = FirstGraphNodeFieldOf(
            reseeded,
            std::string_view("label"),
            std::string_view("{g1}"));
        ASSERT_TRUE(reseeded_label.has_value());
        EXPECT_EQ(*reseeded_label, "我的延迟节点");
        const auto reseeded_id = FirstGraphNodeFieldOf(
            reseeded,
            std::string_view("id"),
            std::string_view("{g1}"));
        ASSERT_TRUE(reseeded_id.has_value());
        EXPECT_EQ(*reseeded_id, *node_id);
    }

    // -----------------------------------------------------------------------
    // Config channel — SetActionCatalog parses the manifest config
    // declarations ({name,type,default?} three-key lockstep like the port
    // channel) and the node fields[] projection merges them so a freshly
    // added node is immediately editable.
    // -----------------------------------------------------------------------

    /// Field lookup in the graph node carrying the given componentGuid in a
    /// document blob's graph projection; nullopt when the node carries no
    /// such field. Empty graph sessions preset begin/end boundary nodes, so
    /// tests address their own added nodes by componentGuid, never by
    /// position.
    std::optional<yyjson::value> FirstGraphNodeFieldDescriptorOf(
        const yyjson::value& document,
        std::string_view     field_name,
        std::string_view     component_guid)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("graph")))
        {
            return std::nullopt;
        }
        auto graph = (*obj)[std::string_view("graph")].as_object();
        if (!graph || !graph->contains(std::string_view("nodes")))
        {
            return std::nullopt;
        }
        auto nodes = (*graph)[std::string_view("nodes")].as_array();
        if (!nodes || nodes->size() == 0)
        {
            return std::nullopt;
        }
        for (const auto& node : *nodes)
        {
            auto node_obj = node.as_object();
            if (!node_obj)
            {
                continue;
            }
            auto node_guid =
                (*node_obj)[std::string_view("componentGuid")].as_string();
            if (!node_guid || *node_guid != component_guid)
            {
                continue;
            }
            if (!node_obj->contains(std::string_view("fields")))
            {
                return std::nullopt;
            }
            auto fields = (*node_obj)[std::string_view("fields")].as_array();
            if (!fields)
            {
                return std::nullopt;
            }
            for (const auto& field : *fields)
            {
                auto field_obj = field.as_object();
                if (!field_obj
                    || !field_obj->contains(std::string_view("name")))
                {
                    continue;
                }
                auto name = (*field_obj)[std::string_view("name")].as_string();
                if (name && *name == field_name)
                {
                    return Das::Utils::CloneYyjsonValue(field);
                }
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    TEST(GraphAuthoringSessionTest, SetActionCatalogConfigProjectsIntoFields)
    {
        const SessionHandle s{""};
        ASSERT_EQ(
            SetActionCatalog(
                s.p,
                R"([{"actionId":"{g1}","label":"das.flow.delay","config":[)"
                R"({"name":"delay_ms","type":"int64","default":1000}]}])"),
            DAS_S_OK);

        ApplyChange(
            s.p,
            WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"));

        // The fresh node (settings {}) already carries the declared default
        // as an editable field.
        const auto document = DocumentOf(s.p);
        const auto delay_field = FirstGraphNodeFieldDescriptorOf(
            document,
            std::string_view("delay_ms"),
            std::string_view("{g1}"));
        ASSERT_TRUE(delay_field.has_value());
        auto field_obj = delay_field->as_object();
        ASSERT_TRUE(field_obj.has_value());
        auto value = (*field_obj)[std::string_view("value")].as_int();
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(*value, 1000);
        auto value_type = (*field_obj)[std::string_view("valueType")].as_int();
        ASSERT_TRUE(value_type.has_value());
        EXPECT_EQ(*value_type, 0);
    }

    // Malformed config channel entries reject the whole catalog (same
    // lockstep as the port channel); the previously accepted catalog stays
    // in place so a single bad producer cannot silently blank the session.
    TEST(GraphAuthoringSessionTest, SetActionCatalogConfigMalformedRejected)
    {
        const SessionHandle s{""};
        ASSERT_EQ(
            SetActionCatalog(
                s.p,
                R"([{"actionId":"{g1}","config":[{"name":"delay_ms",)"
                R"("type":"int64","default":1000}]}])"),
            DAS_S_OK);

        // Unknown key on a config element.
        EXPECT_EQ(
            SetActionCatalog(
                s.p,
                R"([{"actionId":"{g1}","config":[{"name":"d","type":"int",)"
                R"("default":1,"extra":2}]}])"),
            DAS_E_INVALID_ARGUMENT);
        // Missing name / missing type / empty type.
        EXPECT_EQ(
            SetActionCatalog(
                s.p,
                R"([{"actionId":"{g1}","config":[{"type":"int"}]}])"),
            DAS_E_INVALID_ARGUMENT);
        EXPECT_EQ(
            SetActionCatalog(
                s.p,
                R"([{"actionId":"{g1}","config":[{"name":"d"}]}])"),
            DAS_E_INVALID_ARGUMENT);
        EXPECT_EQ(
            SetActionCatalog(
                s.p,
                R"([{"actionId":"{g1}","config":[{"name":"d","type":""}]}])"),
            DAS_E_INVALID_ARGUMENT);
        // Non-array config value.
        EXPECT_EQ(
            SetActionCatalog(
                s.p,
                R"([{"actionId":"{g1}","config":{"delay_ms":1}}])"),
            DAS_E_INVALID_ARGUMENT);

        // The first (well-formed) catalog survived the failed sets: the
        // session still projects the delay_ms field.
        ApplyChange(
            s.p,
            WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"));
        const auto document = DocumentOf(s.p);
        EXPECT_TRUE(FirstGraphNodeFieldDescriptorOf(
                        document,
                        std::string_view("delay_ms"),
                        std::string_view("{g1}"))
                        .has_value());
    }

    // The optional taskGuid declaration on a config element rides the
    // catalog channel onto the projected field (engaged-only wire key); a
    // non-string taskGuid is a shape violation rejecting the whole catalog.
    TEST(GraphAuthoringSessionTest, SetActionCatalogConfigCarriesTaskGuid)
    {
        const SessionHandle s{""};
        ASSERT_EQ(
            SetActionCatalog(
                s.p,
                R"([{"actionId":"{g1}","label":"das.maapi.run","config":[)"
                R"({"name":"repositoryRef","type":"taskRepositoryRef",)"
                R"("taskGuid":"69F20001-0000-4000-8000-000000000001"},)"
                R"({"name":"runtimeInputs","type":"object"}]}])"),
            DAS_S_OK);

        ApplyChange(
            s.p,
            WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"));

        const auto document = DocumentOf(s.p);
        const auto ref_field = FirstGraphNodeFieldDescriptorOf(
            document,
            std::string_view("repositoryRef"),
            std::string_view("{g1}"));
        ASSERT_TRUE(ref_field.has_value());
        auto ref_obj = ref_field->as_object();
        ASSERT_TRUE(ref_obj.has_value());
        ASSERT_TRUE(ref_obj->contains(std::string_view("taskGuid")));
        EXPECT_EQ(
            (*ref_obj)[std::string_view("taskGuid")].as_string().value_or(""),
            "69F20001-0000-4000-8000-000000000001");

        const auto inputs_field = FirstGraphNodeFieldDescriptorOf(
            document,
            std::string_view("runtimeInputs"),
            std::string_view("{g1}"));
        ASSERT_TRUE(inputs_field.has_value());
        auto inputs_obj = inputs_field->as_object();
        ASSERT_TRUE(inputs_obj.has_value());
        EXPECT_FALSE(inputs_obj->contains(std::string_view("taskGuid")));

        // Non-string taskGuid rejects the whole catalog; the accepted
        // catalog stays in place (same lockstep as the other shape rules).
        EXPECT_EQ(
            SetActionCatalog(
                s.p,
                R"([{"actionId":"{g1}","config":[{"name":"d","type":"int",)"
                R"("taskGuid":7}]}])"),
            DAS_E_INVALID_ARGUMENT);
        ApplyChange(
            s.p,
            WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"));
        const auto after_reject = DocumentOf(s.p);
        const auto survived = FirstGraphNodeFieldDescriptorOf(
            after_reject,
            std::string_view("repositoryRef"),
            std::string_view("{g1}"));
        ASSERT_TRUE(survived.has_value());
        auto survived_obj = survived->as_object();
        ASSERT_TRUE(survived_obj.has_value());
        EXPECT_TRUE(survived_obj->contains(std::string_view("taskGuid")));
    }

    // The required declaration on a config element rides the catalog channel
    // onto the projected field (fields[].required); a non-bool required is a
    // shape violation rejecting the whole catalog — the whitelist opens for
    // exactly the bool-typed key and nothing else.
    TEST(GraphAuthoringSessionTest, SetActionCatalogConfigCarriesRequired)
    {
        const SessionHandle s{""};
        ASSERT_EQ(
            SetActionCatalog(
                s.p,
                R"([{"actionId":"{g1}","label":"das.flow.launch","config":[)"
                R"({"name":"exePath","type":"string","required":true},)"
                R"({"name":"args","type":"array<string>"}]}])"),
            DAS_S_OK);

        ApplyChange(
            s.p,
            WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"));

        const auto document = DocumentOf(s.p);
        const auto exe_field = FirstGraphNodeFieldDescriptorOf(
            document,
            std::string_view("exePath"),
            std::string_view("{g1}"));
        ASSERT_TRUE(exe_field.has_value());
        auto exe_obj = exe_field->as_object();
        ASSERT_TRUE(exe_obj.has_value());
        ASSERT_TRUE(exe_obj->contains(std::string_view("required")));
        EXPECT_TRUE(
            (*exe_obj)[std::string_view("required")].as_bool().value_or(false));

        const auto args_field = FirstGraphNodeFieldDescriptorOf(
            document,
            std::string_view("args"),
            std::string_view("{g1}"));
        ASSERT_TRUE(args_field.has_value());
        auto args_obj = args_field->as_object();
        ASSERT_TRUE(args_obj.has_value());
        ASSERT_TRUE(args_obj->contains(std::string_view("required")));
        EXPECT_FALSE(
            (*args_obj)[std::string_view("required")].as_bool().value_or(true));

        // Non-bool required rejects the whole catalog; the accepted catalog
        // stays in place (same lockstep as taskGuid/label shape rules).
        EXPECT_EQ(
            SetActionCatalog(
                s.p,
                R"([{"actionId":"{g1}","config":[{"name":"d","type":"int",)"
                R"("required":"yes"}]}])"),
            DAS_E_INVALID_ARGUMENT);
        ApplyChange(
            s.p,
            WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"));
        const auto after_reject = DocumentOf(s.p);
        const auto survived = FirstGraphNodeFieldDescriptorOf(
            after_reject,
            std::string_view("exePath"),
            std::string_view("{g1}"));
        ASSERT_TRUE(survived.has_value());
        auto survived_obj = survived->as_object();
        ASSERT_TRUE(survived_obj.has_value());
        EXPECT_TRUE(
            (*survived_obj)[std::string_view("required")].as_bool().value_or(
                false));
    }

    // -----------------------------------------------------------------------
    // begin/end 编译约束（session graph 视图契约）：begin 恰一 / end 至多
    // 一；formSequence 视图与 Core 编译器本体不受影响（无 begin 的图文档
    // 在三场景必须继续可编译）。GUID 与 manifest 字面量独立钉值。
    // -----------------------------------------------------------------------

    constexpr std::string_view BOUNDARY_BEGIN_GUID =
        "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E";
    constexpr std::string_view BOUNDARY_END_GUID =
        "E1B7C9D3-5F4A-4E2B-8D6C-7A9E1F3B5C2D";

    /// Raw graph-doc node literal with the given id and componentGuid.
    std::string RawGraphNodeOf(
        std::string_view node_id,
        std::string_view component_guid)
    {
        return std::string(R"({"nodeId":")") + std::string(node_id)
               + R"(","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":")"
               + std::string(component_guid)
               + R"(","pluginGuid":"{p}"}},"settings":{}})";
    }

    /// Build a raw graph context {"nodes":[...],"edges":[...]} from node
    /// literals plus optional edge literals.
    std::string RawGraphContextOf(
        const std::vector<std::string>& nodes,
        const std::vector<std::string>& edges = {})
    {
        std::string context = R"({"nodes":[)";
        for (std::size_t index = 0; index < nodes.size(); ++index)
        {
            if (index != 0)
            {
                context += ',';
            }
            context += nodes[index];
        }
        context += R"(],"edges":[)";
        for (std::size_t index = 0; index < edges.size(); ++index)
        {
            if (index != 0)
            {
                context += ',';
            }
            context += edges[index];
        }
        context += "]}";
        return context;
    }

    struct SessionCompileOutcome
    {
        std::vector<yyjson::value> diagnostics;
        std::vector<std::string>   execution_order;
    };

    /// Compile outcome parts the constraint tests assert on: the envelope
    /// diagnostics array (UI ErrorLens consumer) and the inner plan's
    /// executionOrder (the fatal-precedent clear target).
    SessionCompileOutcome CompileOutcomeOf(GraphAuthoringSessionState* p)
    {
        SessionCompileOutcome                  outcome;
        DasPtr<Das::ExportInterface::IDasJson> compiled;
        GraphAuthoringSessionCompile(p, nullptr, compiled.Put());
        if (compiled.Get() == nullptr)
        {
            return outcome;
        }
        DasPtr<IDasReadOnlyString> compiled_text;
        if (DAS::IsFailed(compiled->ToString(0, compiled_text.Put()))
            || compiled_text.Get() == nullptr)
        {
            return outcome;
        }
        const char* utf8 = nullptr;
        if (DAS::IsFailed(compiled_text->GetUtf8(&utf8)) || utf8 == nullptr)
        {
            return outcome;
        }
        auto root = yyjson::read(std::string(utf8));
        auto root_obj = root.as_object();
        if (!root_obj.has_value())
        {
            return outcome;
        }
        if (root_obj->contains(std::string_view("diagnostics")))
        {
            if (auto diagnostics =
                    (*root_obj)[std::string_view("diagnostics")].as_array())
            {
                for (const auto& diagnostic : *diagnostics)
                {
                    outcome.diagnostics.push_back(
                        Das::Utils::CloneYyjsonValue(diagnostic));
                }
            }
        }
        if (root_obj->contains(std::string_view("compile")))
        {
            auto compile_section =
                (*root_obj)[std::string_view("compile")].as_object();
            if (compile_section.has_value())
            {
                auto shell =
                    (*compile_section)[std::string_view("executionInput")]
                        .as_object();
                if (shell.has_value())
                {
                    auto plan_obj =
                        (*shell)[std::string_view("plan")].as_object();
                    if (plan_obj.has_value()
                        && plan_obj->contains(
                            std::string_view("executionOrder")))
                    {
                        if (auto order =
                                (*plan_obj)[std::string_view("executionOrder")]
                                    .as_array())
                        {
                            for (const auto& node_id : *order)
                            {
                                if (auto id = node_id.as_string())
                                {
                                    outcome.execution_order.push_back(
                                        std::string(*id));
                                }
                            }
                        }
                    }
                }
            }
        }
        return outcome;
    }

    bool DiagnosticsContainKind(
        const SessionCompileOutcome&                   outcome,
        Das::Core::GraphRuntime::CompileDiagnosticKind kind)
    {
        const std::string kind_text = std::to_string(static_cast<int>(kind));
        for (const auto& diagnostic : outcome.diagnostics)
        {
            auto diagnostic_obj = diagnostic.as_object();
            if (!diagnostic_obj.has_value())
            {
                continue;
            }
            if ((*diagnostic_obj)[std::string_view("kind")]
                    .as_string()
                    .value_or("")
                == kind_text)
            {
                return true;
            }
        }
        return false;
    }

    std::string DiagnosticMessageOfKind(
        const SessionCompileOutcome&                   outcome,
        Das::Core::GraphRuntime::CompileDiagnosticKind kind)
    {
        const std::string kind_text = std::to_string(static_cast<int>(kind));
        for (const auto& diagnostic : outcome.diagnostics)
        {
            auto diagnostic_obj = diagnostic.as_object();
            if (!diagnostic_obj.has_value())
            {
                continue;
            }
            if ((*diagnostic_obj)[std::string_view("kind")]
                    .as_string()
                    .value_or("")
                == kind_text)
            {
                return std::string(
                    (*diagnostic_obj)[std::string_view("message")]
                        .as_string()
                        .value_or(""));
            }
        }
        return {};
    }

    // 缺 begin：graph 文档零 begin 节点 → BeginNodeMissing（fatal：无执行
    // 序）+ message 指引文案。
    TEST(GraphAuthoringSessionTest, CompileGraphViewRequiresBeginNode)
    {
        const SessionHandle s{
            RawGraphContextOf({RawGraphNodeOf("only", "{g1}")})};
        SetActionCatalog(s.p, R"([{"actionId":"{g1}"}])");

        const auto outcome = CompileOutcomeOf(s.p);
        EXPECT_TRUE(DiagnosticsContainKind(
            outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::BeginNodeMissing));
        EXPECT_NE(
            DiagnosticMessageOfKind(
                outcome,
                Das::Core::GraphRuntime::CompileDiagnosticKind::
                    BeginNodeMissing)
                .find("add a Begin node"),
            std::string::npos);
        EXPECT_TRUE(outcome.execution_order.empty());
    }

    // 双 begin → BeginNodeDuplicate（fatal）+ message 指明重复节点 id。
    TEST(GraphAuthoringSessionTest, CompileGraphViewRejectsDuplicateBeginNodes)
    {
        const SessionHandle s{RawGraphContextOf(
            {RawGraphNodeOf("dupA", BOUNDARY_BEGIN_GUID),
             RawGraphNodeOf("dupB", BOUNDARY_BEGIN_GUID)})};

        const auto outcome = CompileOutcomeOf(s.p);
        EXPECT_TRUE(DiagnosticsContainKind(
            outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::
                BeginNodeDuplicate));
        const auto message = DiagnosticMessageOfKind(
            outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::BeginNodeDuplicate);
        EXPECT_NE(message.find("dupA"), std::string::npos);
        EXPECT_NE(message.find("dupB"), std::string::npos);
        EXPECT_TRUE(outcome.execution_order.empty());
    }

    // 双 end（begin 恰一）→ EndNodeDuplicate（fatal）。
    TEST(GraphAuthoringSessionTest, CompileGraphViewRejectsDuplicateEndNodes)
    {
        const SessionHandle s{RawGraphContextOf(
            {RawGraphNodeOf("n_begin", BOUNDARY_BEGIN_GUID),
             RawGraphNodeOf("endA", BOUNDARY_END_GUID),
             RawGraphNodeOf("endB", BOUNDARY_END_GUID)})};

        const auto outcome = CompileOutcomeOf(s.p);
        EXPECT_TRUE(DiagnosticsContainKind(
            outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::EndNodeDuplicate));
        EXPECT_FALSE(DiagnosticsContainKind(
            outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::BeginNodeMissing));
        EXPECT_TRUE(outcome.execution_order.empty());
    }

    // 缺 end（begin 恰一）→ 宽容：零 begin/end 约束诊断、执行序非空。
    TEST(GraphAuthoringSessionTest, CompileGraphViewToleratesMissingEndNode)
    {
        const SessionHandle s{RawGraphContextOf(
            {RawGraphNodeOf("n_begin", BOUNDARY_BEGIN_GUID),
             RawGraphNodeOf("worker", "{g1}")})};
        SetActionCatalog(s.p, R"([{"actionId":"{g1}"}])");

        const auto outcome = CompileOutcomeOf(s.p);
        EXPECT_FALSE(DiagnosticsContainKind(
            outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::BeginNodeMissing));
        EXPECT_FALSE(DiagnosticsContainKind(
            outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::EndNodeDuplicate));
        EXPECT_FALSE(outcome.execution_order.empty());
    }

    // formSequence 门：LINEAR 文档（无 begin）编译零 begin/end 检查诊断。
    TEST(GraphAuthoringSessionTest, CompileLinearViewSkipsBeginEndConstraints)
    {
        constexpr std::string_view item_a =
            R"({"itemId":"a","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":"{ga}","pluginGuid":"{p}"},"settings":{}}})";
        const SessionHandle s{
            std::string(R"({"documentId":"d1","version":0,"items":[)")
            + std::string(item_a) + "]}"};

        const auto outcome = CompileOutcomeOf(s.p);
        EXPECT_FALSE(DiagnosticsContainKind(
            outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::BeginNodeMissing));
        EXPECT_FALSE(DiagnosticsContainKind(
            outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::
                BeginNodeDuplicate));
        EXPECT_FALSE(DiagnosticsContainKind(
            outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::EndNodeDuplicate));
    }

    // 值口 int 锁：浮点源口 → end.值 拒（TypeMismatch）；整数族内
    // （int64 源）放行——113 数字等价类经 session 声明面生效。
    TEST(GraphAuthoringSessionTest, CompileLocksEndValuePortToIntegerFamily)
    {
        const auto edge_literal =
            [](std::string_view source_id, std::string_view source_type)
        {
            return std::string(R"({"edgeId":"e1","sourceNodeId":")")
                   + std::string(source_id)
                   + R"(","sourcePortId":"值","targetNodeId":"n_end","targetPortId":"值","edgeType":"data"})";
        };

        // 浮点源 → TypeMismatch 拒。
        {
            const SessionHandle s{RawGraphContextOf(
                {RawGraphNodeOf("n_begin", BOUNDARY_BEGIN_GUID),
                 RawGraphNodeOf("n_probe", "{g1}"),
                 RawGraphNodeOf("n_end", BOUNDARY_END_GUID)},
                {edge_literal("n_probe", "float")})};
            ASSERT_EQ(
                SetActionCatalog(
                    s.p,
                    std::string(
                        R"([{"actionId":"{g1}","ports":[{"portId":"值","portType":"float","direction":"output"}]},)")
                        + R"({"actionId":")" + std::string(BOUNDARY_END_GUID)
                        + R"(","ports":[{"portId":"值","portType":"int","direction":"input"},{"portId":"值","portType":"int","direction":"output"}]}])"),
                DAS_S_OK);

            const auto outcome = CompileOutcomeOf(s.p);
            EXPECT_TRUE(DiagnosticsContainKind(
                outcome,
                Das::Core::GraphRuntime::CompileDiagnosticKind::TypeMismatch));
        }

        // int64 源（整数族内）→ 放行：零 TypeMismatch、执行序非空。
        {
            const SessionHandle s{RawGraphContextOf(
                {RawGraphNodeOf("n_begin", BOUNDARY_BEGIN_GUID),
                 RawGraphNodeOf("n_probe", "{g1}"),
                 RawGraphNodeOf("n_end", BOUNDARY_END_GUID)},
                {edge_literal("n_probe", "int64")})};
            ASSERT_EQ(
                SetActionCatalog(
                    s.p,
                    std::string(
                        R"([{"actionId":"{g1}","ports":[{"portId":"值","portType":"int64","direction":"output"}]},)")
                        + R"({"actionId":")" + std::string(BOUNDARY_END_GUID)
                        + R"(","ports":[{"portId":"值","portType":"int","direction":"input"},{"portId":"值","portType":"int","direction":"output"}]}])"),
                DAS_S_OK);

            const auto outcome = CompileOutcomeOf(s.p);
            EXPECT_FALSE(DiagnosticsContainKind(
                outcome,
                Das::Core::GraphRuntime::CompileDiagnosticKind::TypeMismatch));
            EXPECT_FALSE(outcome.execution_order.empty());
        }
    }

    // A5 裁决：formSequence 升级 graph 不自动补 begin——升级后的图缺
    // begin 由编译拒绝 + 诊断指引（用户手动放置），升级入口零节点注入
    // （向用户既有图插节点是破坏性突变）。
    TEST(GraphAuthoringSessionTest, UpgradeToGraphDoesNotPresetBoundaryNodes)
    {
        constexpr std::string_view item =
            R"({"itemId":"a","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":"{ga}","pluginGuid":"{p}"},"settings":{}}})";
        const SessionHandle s{
            std::string(R"({"documentId":"d1","version":0,"items":[)")
            + std::string(item) + "]}"};

        ASSERT_EQ(GraphAuthoringSessionUpgradeToGraph(s.p), DAS_S_OK);

        // 升级后文档不含预置 begin/end 节点，视图已翻转为 graph。
        const auto document_text = Compact(
            Das::Utils::SerializeYyjsonValue(DocumentOf(s.p)).value_or(""));
        EXPECT_NE(
            document_text.find("\"viewKind\":\"graph\""),
            std::string::npos);
        EXPECT_EQ(document_text.find(BOUNDARY_BEGIN_GUID), std::string::npos);
        EXPECT_EQ(document_text.find(BOUNDARY_END_GUID), std::string::npos);

        // 编译拒绝：BeginNodeMissing（fatal）+ 指引文案可见。
        const auto outcome = CompileOutcomeOf(s.p);
        EXPECT_TRUE(DiagnosticsContainKind(
            outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::BeginNodeMissing));
        EXPECT_NE(
            DiagnosticMessageOfKind(
                outcome,
                Das::Core::GraphRuntime::CompileDiagnosticKind::
                    BeginNodeMissing)
                .find("add a Begin node"),
            std::string::npos);
        EXPECT_TRUE(outcome.execution_order.empty());
    }

    // 预置边界：调度器形态上下文（taskId + properties 携带非空图文档）
    // 不追加 begin/end——预置仅限「新建空图」。
    TEST(
        GraphAuthoringSessionTest,
        SchedulerPropertiesContextDoesNotPresetBoundaryNodes)
    {
        const SessionHandle s{
            std::string(R"({"taskId":7,"revision":1,"properties":{"nodes":[)")
            + RawGraphNodeOf("x1", "{g9}") + R"(],"edges":[]}})"};

        const auto node_ids = DocumentGraphNodeIds(DocumentOf(s.p));
        ASSERT_EQ(node_ids.size(), 1u);
        EXPECT_EQ(node_ids[0], "x1");
    }

    // -----------------------------------------------------------------------
    // begin/end 动态口进编译声明面（D-17 方案 A：session provider 闭包按
    // 当前文档动态展开）。空 context 会话预置 begin(n_0)/end(n_1)，后续
    // addGraphNode 顺延 n_2。
    // -----------------------------------------------------------------------

    /// 应用一个 change 并断言无错误诊断（成功路径）；返回原始结果文本。
    std::string ApplyChangeExpectOk(
        GraphAuthoringSessionState* p,
        std::string_view            kind,
        std::string_view            payload)
    {
        const auto result = Compact(
            JsonToUtf8(ApplyChange(p, WrapEnvelope(kind, payload)).Get()));
        EXPECT_EQ(result.find("\"severity\":\"error\""), std::string::npos)
            << kind << ": " << result;
        return result;
    }

    /// 取指定组件首个 dynamic:true 投影口的指定字符串键值（portId/
    /// direction），无动态口时返回空串。
    std::string DynamicPortFieldOfComponent(
        const yyjson::value& document,
        std::string_view     component_guid,
        std::string_view     field)
    {
        const auto ports = NodePortsOfComponent(document, component_guid);
        if (!ports.has_value())
        {
            return {};
        }
        auto arr = ports->as_array();
        if (!arr.has_value())
        {
            return {};
        }
        for (const auto& port : *arr)
        {
            auto port_object = port.as_object();
            if (!port_object.has_value()
                || !port_object->contains(std::string_view("dynamic")))
            {
                continue;
            }
            if ((*port_object)[std::string_view("dynamic")].as_bool().value_or(
                    false))
            {
                return std::string((*port_object)[std::string_view(field)]
                                       .as_string()
                                       .value_or(""));
            }
        }
        return {};
    }

    // 声明面动态展开 + 哨兵收敛：begin 长出入参口 左值(int) → begin 口对
    // 连出到消费节点 → 编译零 PortNotFound/TypeMismatch（动态口被声明面
    // 解析）；begin 的 in 口悬空 → $graph_input 哨兵绑定 target=该口；
    // 边界输入面携带该口（display=口名）。
    TEST(GraphAuthoringSessionTest, CompileResolvesDynamicPortEdgesAndSentinel)
    {
        const SessionHandle s{""};
        ASSERT_EQ(
            SetActionCatalog(
                s.p,
                std::string(
                    R"([{"actionId":"{g1}","ports":[{"portId":"seed","portType":"int","direction":"input"},{"portId":"result","portType":"string","direction":"output"}]},)")
                    + R"({"actionId":")" + std::string(BOUNDARY_BEGIN_GUID)
                    + R"("},{"actionId":")" + std::string(BOUNDARY_END_GUID)
                    + R"(","ports":[{"portId":"值","portType":"int","direction":"input"},{"portId":"值","portType":"int","direction":"output"}]}])"),
            DAS_S_OK);

        ApplyChangeExpectOk(
            s.p,
            "addNodePort",
            R"({"nodeId":"n_0","name":"左值","direction":"input","valueType":"int"})");
        ApplyChangeExpectOk(s.p, "addGraphNode", R"({"actionId":"{g1}"})");

        const auto dynamic_port_id = DynamicPortFieldOfComponent(
            DocumentOf(s.p),
            std::string_view(BOUNDARY_BEGIN_GUID),
            std::string_view("portId"));
        ASSERT_FALSE(dynamic_port_id.empty());

        ApplyChangeExpectOk(
            s.p,
            "connectGraphPorts",
            std::string(R"({"fromNodeId":"n_0","fromPortId":")")
                + dynamic_port_id + R"(","toNodeId":"n_2","toPortId":"seed"})");

        const auto outcome = CompileOutcomeOf(s.p);
        EXPECT_FALSE(DiagnosticsContainKind(
            outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::PortNotFound));
        EXPECT_FALSE(DiagnosticsContainKind(
            outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::TypeMismatch));

        // 编译产物细读：哨兵绑定 target=begin 的动态口（悬空 in 口自动
        // 收敛）+ 边界输入面携带该口。
        DasPtr<Das::ExportInterface::IDasJson> compiled;
        ASSERT_EQ(
            GraphAuthoringSessionCompile(s.p, nullptr, compiled.Put()),
            DAS_S_OK);
        auto root = yyjson::read(JsonToUtf8(compiled.Get())).as_object();
        ASSERT_TRUE(root.has_value());
        auto compile_section = (*root)[std::string_view("compile")].as_object();
        ASSERT_TRUE(compile_section.has_value());
        auto shell =
            (*compile_section)[std::string_view("executionInput")].as_object();
        ASSERT_TRUE(shell.has_value());
        auto plan_object = (*shell)[std::string_view("plan")].as_object();
        ASSERT_TRUE(plan_object.has_value());

        auto binding_plan =
            (*plan_object)[std::string_view("bindingPlan")].as_object();
        ASSERT_TRUE(binding_plan.has_value());
        auto bindings =
            (*binding_plan)[std::string_view("bindings")].as_array();
        ASSERT_TRUE(bindings.has_value());
        bool sentinel_target_found = false;
        for (const auto& binding : *bindings)
        {
            auto binding_object = binding.as_object();
            if (!binding_object.has_value())
            {
                continue;
            }
            if ((*binding_object)[std::string_view("sourceNodeId")]
                        .as_string()
                        .value_or("")
                    == std::string_view("$graph_input")
                && (*binding_object)[std::string_view("targetNodeId")]
                           .as_string()
                           .value_or("")
                       == std::string_view("n_0")
                && (*binding_object)[std::string_view("targetPortId")]
                           .as_string()
                           .value_or("")
                       == std::string_view(dynamic_port_id))
            {
                sentinel_target_found = true;
                break;
            }
        }
        EXPECT_TRUE(sentinel_target_found);

        auto boundary_ports =
            (*plan_object)[std::string_view("boundaryPorts")].as_object();
        ASSERT_TRUE(boundary_ports.has_value());
        auto boundary_inputs =
            (*boundary_ports)[std::string_view("inputs")].as_array();
        ASSERT_TRUE(boundary_inputs.has_value());
        bool boundary_dynamic_found = false;
        for (const auto& boundary_input : *boundary_inputs)
        {
            auto input_object = boundary_input.as_object();
            if (!input_object.has_value())
            {
                continue;
            }
            if ((*input_object)[std::string_view("portId")]
                    .as_string()
                    .value_or("")
                != std::string_view(dynamic_port_id))
            {
                continue;
            }
            boundary_dynamic_found = true;
            EXPECT_EQ(
                (*input_object)[std::string_view("displayLabel")]
                    .as_string()
                    .value_or(""),
                std::string_view("左值"));
        }
        EXPECT_TRUE(boundary_dynamic_found);
    }

    // 会话级 Get 投影：动态口 merge 进呈现面（方向翻转 + dynamic 键），
    // 且整份文档过严格 wire 校验（validator 词表含 dynamic）。
    TEST(
        GraphAuthoringSessionTest,
        GetProjectionCarriesDynamicPortOnSessionWire)
    {
        const SessionHandle s{""};
        ASSERT_EQ(
            SetActionCatalog(
                s.p,
                R"([{"actionId":")" + std::string(BOUNDARY_BEGIN_GUID)
                    + R"("},{"actionId":")" + std::string(BOUNDARY_END_GUID)
                    + R"(","ports":[{"portId":"值","portType":"int","direction":"input"},{"portId":"值","portType":"int","direction":"output"}]}])"),
            DAS_S_OK);

        ApplyChangeExpectOk(
            s.p,
            "addNodePort",
            R"({"nodeId":"n_0","name":"左值","direction":"input","valueType":"int"})");
        ApplyChangeExpectOk(
            s.p,
            "addNodePort",
            R"({"nodeId":"n_1","name":"右值","direction":"output","valueType":"image"})");

        const auto document = DocumentOf(s.p);
        auto       document_result =
            Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
        EXPECT_TRUE(document_result.valid)
            << document_result.error_kind << ": " << document_result.message;

        // begin 的入参口投影在右侧 output 锚；end 的出参口投影在左侧
        // input 锚（锚点方向=组件角色翻转）。
        EXPECT_EQ(
            DynamicPortFieldOfComponent(
                document,
                std::string_view(BOUNDARY_BEGIN_GUID),
                std::string_view("direction")),
            "output");
        EXPECT_EQ(
            DynamicPortFieldOfComponent(
                document,
                std::string_view(BOUNDARY_END_GUID),
                std::string_view("direction")),
            "input");
    }
} // namespace
