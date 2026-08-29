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

    /// Ports array of the first graph node inside a Get document; nullopt
    /// when the node carries no "ports" key at all.
    std::optional<yyjson::value> FirstNodePortsOf(const yyjson::value& document)
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
        auto node_obj = (*nodes)[0].as_object();
        if (!node_obj || !node_obj->contains(std::string_view("ports")))
        {
            return std::nullopt;
        }
        return Das::Utils::CloneYyjsonValue(
            (*node_obj)[std::string_view("ports")]);
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
        const auto ports = FirstNodePortsOf(document);
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

        const auto ports = FirstNodePortsOf(DocumentOf(s.p));
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

    /// Id of the first node in the document's graph projection; nullopt when
    /// the document carries no node.
    std::optional<std::string> FirstGraphNodeOf(const yyjson::value& document)
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
        auto first = (*nodes)[0].as_object();
        if (!first || !first->contains(std::string_view("id")))
        {
            return std::nullopt;
        }
        auto id = (*first)[std::string_view("id")].as_string();
        if (!id)
        {
            return std::nullopt;
        }
        return std::string(*id);
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
        const auto node_id = FirstGraphNodeOf(DocumentOf(s.p));
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
        const auto node_id = FirstGraphNodeOf(DocumentOf(s.p));
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
        const auto node_id = FirstGraphNodeOf(DocumentOf(s.p));
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
        const auto node_id = FirstGraphNodeOf(DocumentOf(s1.p));
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

    /// All node ids in a GetDocument document blob's graph projection.
    std::vector<std::string> DocumentGraphNodeIds(const yyjson::value& document)
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
                if (auto id = (*node_obj)[std::string_view("id")].as_string())
                {
                    ids.emplace_back(*id);
                }
            }
        }
        return ids;
    }

    /// All nodeId strings inside an acceptedProperties source blob (the
    /// internal GraphDocumentDto wire).
    std::vector<std::string> SourceGraphNodeIds(const yyjson::value& source)
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
        SetActionCatalog(s.p, R"([{"actionId":"{g1}"}])");
        for (int node_count = 0; node_count < 3; ++node_count)
        {
            ApplyChange(
                s.p,
                WrapEnvelope("addGraphNode", R"({"actionId":"{g1}"})"));
        }
        const auto node_ids = DocumentGraphNodeIds(DocumentOf(s.p));
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
        // trace of their ids — connections included).
        const auto document = DocumentOf(s.p);
        const auto remaining = DocumentGraphNodeIds(document);
        ASSERT_EQ(remaining.size(), 1u);
        EXPECT_EQ(remaining[0], node_ids[2]);
        const auto document_text =
            Compact(Das::Utils::SerializeYyjsonValue(document).value_or(""));
        EXPECT_EQ(document_text.find(node_ids[0]), std::string::npos);
        EXPECT_EQ(document_text.find(node_ids[1]), std::string::npos);

        // The persisted source blob agrees, and the semantic revision moved
        // exactly once for the whole batch.
        const auto source_ids =
            SourceGraphNodeIds(AcceptedPropertiesOf(response.Get()));
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
        const auto node_ids = DocumentGraphNodeIds(DocumentOf(s.p));
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
} // namespace
