#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/GraphRuntime/GraphCompiler.h>
#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/Core/TaskScheduler/TaskAuthoringContract.h>
#include <das/Core/TaskScheduler/TaskCapabilityRegistry.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/PathText.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentCreator.Implements.hpp>
#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
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
            CreateGraphAuthoringSession(nullptr, s.Get(), &p);
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

    // Compose a session context whose actionCatalog key seeds the catalog
    // through SeedFromContext — the production wiring shape (the scheduler
    // assembles the registry snapshot into the session context; the plugin
    // forwards the whole context verbatim to CreateGraphAuthoringSession, so
    // the Core-side parse is the seeding channel).
    std::string WithActionCatalog(
        std::string_view context,
        std::string_view catalog_json)
    {
        yyjson::value root(Das::Utils::MakeYyjsonObject());
        if (!context.empty())
        {
            root = yyjson::read(std::string(context));
        }
        auto obj = root.as_object();
        if (!obj)
        {
            return std::string(context);
        }
        (*obj)[std::string_view("actionCatalog")] =
            yyjson::read(std::string(catalog_json));
        auto serialized = Das::Utils::SerializeYyjsonValue(root);
        return serialized.value_or(std::string(context));
    }

    // Catalog accept/reject semantics assertion: seed the catalog through the
    // context key and report whether it took (a well-formed catalog projects
    // the actionCatalog section; a malformed one degrades to zero catalog —
    // the session keeps running with no actionCatalog key at all).
    bool CatalogSeededViaContext(std::string_view catalog_json)
    {
        const SessionHandle s{WithActionCatalog("", catalog_json)};
        const auto          document = DocumentOf(s.p);
        auto                obj = document.as_object();
        return obj.has_value()
               && obj->contains(std::string_view("actionCatalog"));
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

    // ---- creator 携带通道（D-07 通道半）----

    // 最小 creator 替身：两法均具名拒（本组用例只断言指针携带，不驱动
    // 会话行为）。
    class NullCreatorHost final
        : public Das::PluginInterface::DasTaskComponentCreatorImplBase<
              NullCreatorHost>
    {
    public:
        DasResult CreateTaskComponent(
            const DasGuid&                            component_guid,
            Das::PluginInterface::IDasTaskComponent** pp_out_component) override
        {
            std::ignore = component_guid;
            if (pp_out_component == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_component = nullptr;
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult CreateAuthoringSession(
            const DasGuid&                                   task_guid,
            Das::ExportInterface::IDasJson*                  p_context_json,
            Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session)
            override
        {
            std::ignore = task_guid;
            std::ignore = p_context_json;
            if (pp_out_session == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_session = nullptr;
            return DAS_E_NO_IMPLEMENTATION;
        }
    };

    TEST(GraphAuthoringSessionTest, CreateSessionCarriesCreatorIntoState)
    {
        auto                        creator = NullCreatorHost::Make();
        auto                        ctx = MakeStr("");
        GraphAuthoringSessionState* state = nullptr;
        ASSERT_EQ(
            CreateGraphAuthoringSession(creator.Get(), ctx.Get(), &state),
            DAS_S_OK);
        ASSERT_NE(state, nullptr);
        // state 持有非空 creator 借用指针（测试缝判定）
        EXPECT_TRUE(GraphAuthoringSessionHasCreatorForTest(state));
        DestroyGraphAuthoringSession(state);
    }

    TEST(GraphAuthoringSessionTest, CreateSessionWithoutCreatorKeepsBehavior)
    {
        // 不传（nullptr）时与现状一致：state 不携带 creator，文档投影正常
        auto                        ctx = MakeStr("");
        GraphAuthoringSessionState* state = nullptr;
        ASSERT_EQ(
            CreateGraphAuthoringSession(nullptr, ctx.Get(), &state),
            DAS_S_OK);
        ASSERT_NE(state, nullptr);
        EXPECT_FALSE(GraphAuthoringSessionHasCreatorForTest(state));
        DestroyGraphAuthoringSession(state);
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
        const SessionHandle s{
            WithActionCatalog("", R"([{"actionId":"{g1}"}])")}; // graph mode
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
        const SessionHandle s{WithActionCatalog(
            std::string(R"({"items":[)") + std::string(item_a) + "]}",
            R"([{"actionId":"{gb}"}])")};
        auto                r = ApplyChange(
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
        const SessionHandle s{WithActionCatalog(
            "",
            R"([{"actionId":"{g1}","label":"das.flow.launch"},{"actionId":"{g2}"}])")};
        const auto          document = DocumentOf(s.p);

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
        // Entry missing actionId / non-object entry (the legacy flat
        // guid-string array form is gone) / payload is not an array at all:
        // each degrades the whole key to zero catalog — the session keeps
        // running and projects no actionCatalog section.
        EXPECT_FALSE(CatalogSeededViaContext(R"([{"label":"x"}])"));
        EXPECT_FALSE(CatalogSeededViaContext(R"(["{g1}"])"));
        EXPECT_FALSE(CatalogSeededViaContext(R"({"actionId":"{g1}"})"));
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
        const SessionHandle s{WithActionCatalog(
            "",
            R"json([{"actionId":"{g1}","label":"das.flow.branch","ports":[{"portId":"condition","portType":"bool","direction":"input"},{"portId":"true","portType":"signal","direction":"output"}]}])json")}; // graph mode
        // The registry snapshot form the Scheduler injects into the context:
        // {actionId,label} entries plus the per-entry static port surface
        // read from the component manifest definition
        // (inputs → "input", outputs → "output").
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
        EXPECT_FALSE(CatalogSeededViaContext(
            R"json([{"actionId":"{g1}","ports":[{"portId":"x","portType":"bool","direction":"sideways"}]}])json"));
        EXPECT_FALSE(CatalogSeededViaContext(
            R"json([{"actionId":"{g1}","ports":[{"portId":"x","portType":"","direction":"input"}]}])json"));
        EXPECT_FALSE(CatalogSeededViaContext(
            R"json([{"actionId":"{g1}","ports":[{"portId":"x","portType":"bool","direction":"input","extra":1}]}])json"));
        EXPECT_FALSE(CatalogSeededViaContext(
            R"json([{"actionId":"{g1}","ports":["x"]}])json"));
        EXPECT_FALSE(CatalogSeededViaContext(
            R"json([{"actionId":"{g1}","ports":{"portId":"x"}}])json"));
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
        EXPECT_TRUE(CatalogSeededViaContext(
            R"json([{"actionId":"{g1}","ports":[{"portId":"index","portType":"int64","direction":"output","loopCarried":true}]}])json"));
        EXPECT_FALSE(CatalogSeededViaContext(
            R"json([{"actionId":"{g1}","ports":[{"portId":"index","portType":"int64","direction":"output","loopCarried":"yes"}]}])json"));
    }

    // Five-key port projection end to end through the C ABI session: the
    // registry snapshot carries the channel form (with displayLabel, as the
    // scheduler produces since the label backfill), and GetDocument projects
    // each node port with exactly the five locked keys — signal ports
    // category "execution" with a non-empty displayLabel, everything else
    // "data". This locks the wire shape the UI fixture consumes.
    TEST(GraphAuthoringSessionTest, NodePortsProjectFiveKeysEndToEnd)
    {
        const SessionHandle s{WithActionCatalog(
            "",
            R"json([{"actionId":"{g1}","label":"das.maapi.run","ports":[
                    {"portId":"executionInput","portType":"object","direction":"input","displayLabel":"Execution Input"},
                    {"portId":"completedTasks","portType":"array<string>","direction":"output","displayLabel":"Completed Tasks"},
                    {"portId":"succeeded","portType":"signal","direction":"output","displayLabel":"Succeeded"},
                    {"portId":"failed","portType":"signal","direction":"output","displayLabel":"Failed"}
                ]}])json")}; // graph mode
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
        const SessionHandle s1{WithActionCatalog(
            "",
            R"([{"actionId":"{g1}"},{"actionId":"{g2}"}])")};
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
        const SessionHandle s{
            WithActionCatalog("", R"([{"actionId":"{g1}"}])")};
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
        const SessionHandle s{WithActionCatalog(
            "",
            R"json([{"actionId":"{g1}","ports":[{"portId":"seed","portType":"int","direction":"input"},{"portId":"result","portType":"string","direction":"output"}]}])json")};
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

    // 口面清理后的 catalog 声明面在会话编译通道同样收缩：报告输出口保留
    // 投影、零输入声明不再派生边界输入。
    TEST(
        GraphAuthoringSessionTest,
        CompileDerivesNoBoundaryInputsFromPortCleanedCatalog)
    {
        const SessionHandle s{WithActionCatalog(
            "",
            R"json([{"actionId":"{g1}","ports":[{"portId":"nextTime","portType":"string","direction":"output"},{"portId":"stopped","portType":"bool","direction":"output"}]}])json")};
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

        const auto boundary_ports =
            (*plan_obj)[std::string_view("boundaryPorts")].as_object();
        ASSERT_TRUE(boundary_ports.has_value());
        const auto inputs =
            (*boundary_ports)[std::string_view("inputs")].as_array();
        ASSERT_TRUE(inputs.has_value());
        EXPECT_TRUE(inputs->empty());

        const auto outputs =
            (*boundary_ports)[std::string_view("outputs")].as_array();
        ASSERT_TRUE(outputs.has_value());
        EXPECT_EQ(outputs->size(), 2u);
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
        const SessionHandle s{
            WithActionCatalog("", R"([{"actionId":"{g1}"}])")};
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
        const SessionHandle s{
            WithActionCatalog("", R"([{"actionId":"{g1}"}])")};
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
        const SessionHandle s{WithActionCatalog(
            "",
            R"json([{"actionId":"{g1}","label":"das.flow.branch","ports":[
                    {"portId":"condition","portType":"bool","direction":"input"},
                    {"portId":"in","portType":"signal","direction":"input"},
                    {"portId":"true","portType":"signal","direction":"output"},
                    {"portId":"index","portType":"int","direction":"output"}
                ]}])json")}; // graph mode
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
        const SessionHandle s{
            WithActionCatalog("", R"([{"actionId":"{g1}"}])")};
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
        const SessionHandle s{
            WithActionCatalog("", R"([{"actionId":"{g1}"}])")};
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
        const SessionHandle s{
            WithActionCatalog("", R"([{"actionId":"{g1}"}])")};
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
        const SessionHandle s1{
            WithActionCatalog("", R"([{"actionId":"{g1}"}])")};
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
        const SessionHandle s{WithActionCatalog(
            "",
            R"([{"actionId":"{g1}","ports":[)"
            R"({"portId":"o","portType":"int","direction":"output"},)"
            R"({"portId":"i","portType":"int","direction":"input"}]}])")};
        // The declared data port surface the edge-type derivation resolves
        // through ("o" output / "i" input on {g1}).
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
        const SessionHandle s{
            WithActionCatalog("", R"([{"actionId":"{g1}"}])")};
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
        const SessionHandle s1{
            WithActionCatalog("", R"([{"actionId":"{g1}"}])")};
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
    // Config channel — the context actionCatalog key parses the manifest
    // config declarations ({name,type,default?} three-key lockstep like the
    // port channel) and the node fields[] projection merges them so a
    // freshly added node is immediately editable.
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

    TEST(GraphAuthoringSessionTest, ActionCatalogConfigProjectsIntoFields)
    {
        const SessionHandle s{WithActionCatalog(
            "",
            R"([{"actionId":"{g1}","label":"das.flow.delay","config":[)"
            R"({"name":"delay_ms","type":"int64","default":1000}]}])")};
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
    // lockstep as the port channel): each malformed seed degrades to zero
    // catalog so a single bad producer cannot silently half-apply.
    TEST(GraphAuthoringSessionTest, ActionCatalogConfigMalformedRejected)
    {
        // Unknown key on a config element.
        EXPECT_FALSE(CatalogSeededViaContext(
            R"([{"actionId":"{g1}","config":[{"name":"d","type":"int",)"
            R"("default":1,"extra":2}]}])"));
        // Missing name / missing type / empty type.
        EXPECT_FALSE(CatalogSeededViaContext(
            R"([{"actionId":"{g1}","config":[{"type":"int"}]}])"));
        EXPECT_FALSE(CatalogSeededViaContext(
            R"([{"actionId":"{g1}","config":[{"name":"d"}]}])"));
        EXPECT_FALSE(CatalogSeededViaContext(
            R"([{"actionId":"{g1}","config":[{"name":"d","type":""}]}])"));
        // Non-array config value.
        EXPECT_FALSE(CatalogSeededViaContext(
            R"([{"actionId":"{g1}","config":{"delay_ms":1}}])"));

        // The well-formed control: the same config shape seeds and projects
        // the delay_ms field.
        const SessionHandle s{WithActionCatalog(
            "",
            R"([{"actionId":"{g1}","config":[{"name":"delay_ms",)"
            R"("type":"int64","default":1000}]}])")};
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
    TEST(GraphAuthoringSessionTest, ActionCatalogConfigCarriesTaskGuid)
    {
        const SessionHandle s{WithActionCatalog(
            "",
            R"([{"actionId":"{g1}","label":"das.maapi.run","config":[)"
            R"({"name":"repositoryRef","type":"taskRepositoryRef",)"
            R"("taskGuid":"69F20001-0000-4000-8000-000000000001"},)"
            R"({"name":"runtimeInputs","type":"object"}]}])")};
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

        // Non-string taskGuid rejects the whole catalog (zero-catalog
        // degradation, same lockstep as the other shape rules).
        EXPECT_FALSE(CatalogSeededViaContext(
            R"([{"actionId":"{g1}","config":[{"name":"d","type":"int",)"
            R"("taskGuid":7}]}])"));
    }

    // The required declaration on a config element rides the catalog channel
    // onto the projected field (fields[].required); a non-bool required is a
    // shape violation rejecting the whole catalog — the whitelist opens for
    // exactly the bool-typed key and nothing else.
    TEST(GraphAuthoringSessionTest, ActionCatalogConfigCarriesRequired)
    {
        const SessionHandle s{WithActionCatalog(
            "",
            R"([{"actionId":"{g1}","label":"das.flow.launch","config":[)"
            R"({"name":"exePath","type":"string","required":true},)"
            R"({"name":"args","type":"array<string>"}]}])")};
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

        // Non-bool required rejects the whole catalog (zero-catalog
        // degradation, same lockstep as taskGuid/label shape rules).
        EXPECT_FALSE(CatalogSeededViaContext(
            R"([{"actionId":"{g1}","config":[{"name":"d","type":"int",)"
            R"("required":"yes"}]}])"));
    }

    // The optional description declaration on a config element rides the
    // catalog channel onto the projected field (engaged-only wire key —
    // absent declaration never emits an empty string); a non-string
    // description is a shape violation rejecting the whole catalog.
    TEST(GraphAuthoringSessionTest, ActionCatalogConfigCarriesDescription)
    {
        const SessionHandle s{WithActionCatalog(
            "",
            R"([{"actionId":"{g1}","label":"das.flow.launch","config":[)"
            R"({"name":"exePath","type":"string","description":)"
            R"("Bare command names resolve via PATH."},)"
            R"({"name":"args","type":"array<string>"}]}])")};
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
        ASSERT_TRUE(exe_obj->contains(std::string_view("description")));
        EXPECT_EQ(
            (*exe_obj)[std::string_view("description")].as_string().value_or(
                ""),
            "Bare command names resolve via PATH.");

        // Undeclared description never emits the key (engaged-only).
        const auto args_field = FirstGraphNodeFieldDescriptorOf(
            document,
            std::string_view("args"),
            std::string_view("{g1}"));
        ASSERT_TRUE(args_field.has_value());
        auto args_obj = args_field->as_object();
        ASSERT_TRUE(args_obj.has_value());
        EXPECT_FALSE(args_obj->contains(std::string_view("description")));

        // Non-string description rejects the whole catalog (zero-catalog
        // degradation, same lockstep as taskGuid/label/required).
        EXPECT_FALSE(CatalogSeededViaContext(
            R"([{"actionId":"{g1}","config":[{"name":"d","type":"int",)"
            R"("description":7}]}])"));
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
        for (const auto& diagnostic : outcome.diagnostics)
        {
            auto diagnostic_obj = diagnostic.as_object();
            if (!diagnostic_obj.has_value())
            {
                continue;
            }
            if ((*diagnostic_obj)[std::string_view("code")].as_sint().value_or(
                    -1)
                == static_cast<std::int64_t>(kind))
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
        for (const auto& diagnostic : outcome.diagnostics)
        {
            auto diagnostic_obj = diagnostic.as_object();
            if (!diagnostic_obj.has_value())
            {
                continue;
            }
            if ((*diagnostic_obj)[std::string_view("code")].as_sint().value_or(
                    -1)
                == static_cast<std::int64_t>(kind))
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
        const SessionHandle s{WithActionCatalog(
            RawGraphContextOf({RawGraphNodeOf("only", "{g1}")}),
            R"([{"actionId":"{g1}"}])")};
        const auto          outcome = CompileOutcomeOf(s.p);
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
        const SessionHandle s{WithActionCatalog(
            RawGraphContextOf(
                {RawGraphNodeOf("n_begin", BOUNDARY_BEGIN_GUID),
                 RawGraphNodeOf("worker", "{g1}")}),
            R"([{"actionId":"{g1}"}])")};
        const auto          outcome = CompileOutcomeOf(s.p);
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
            const SessionHandle s{WithActionCatalog(
                RawGraphContextOf(
                    {RawGraphNodeOf("n_begin", BOUNDARY_BEGIN_GUID),
                     RawGraphNodeOf("n_probe", "{g1}"),
                     RawGraphNodeOf("n_end", BOUNDARY_END_GUID)},
                    {edge_literal("n_probe", "float")}),
                std::string(
                    R"([{"actionId":"{g1}","ports":[{"portId":"值","portType":"float","direction":"output"}]},)")
                    + R"({"actionId":")" + std::string(BOUNDARY_END_GUID)
                    + R"(","ports":[{"portId":"值","portType":"int","direction":"input"},{"portId":"值","portType":"int","direction":"output"}]}])")};
            const auto          outcome = CompileOutcomeOf(s.p);
            EXPECT_TRUE(DiagnosticsContainKind(
                outcome,
                Das::Core::GraphRuntime::CompileDiagnosticKind::TypeMismatch));
        }

        // int64 源（整数族内）→ 放行：零 TypeMismatch、执行序非空。
        {
            const SessionHandle s{WithActionCatalog(
                RawGraphContextOf(
                    {RawGraphNodeOf("n_begin", BOUNDARY_BEGIN_GUID),
                     RawGraphNodeOf("n_probe", "{g1}"),
                     RawGraphNodeOf("n_end", BOUNDARY_END_GUID)},
                    {edge_literal("n_probe", "int64")}),
                std::string(
                    R"([{"actionId":"{g1}","ports":[{"portId":"值","portType":"int64","direction":"output"}]},)")
                    + R"({"actionId":")" + std::string(BOUNDARY_END_GUID)
                    + R"(","ports":[{"portId":"值","portType":"int","direction":"input"},{"portId":"值","portType":"int","direction":"output"}]}])")};
            const auto          outcome = CompileOutcomeOf(s.p);
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

    // 声明面动态展开 + 哨兵收敛（faces 通道形态）：begin 的文档口面经
    // nodeFaces 注入（口对双侧并入——出侧供图内连线）→ begin 口连出到消
    // 费节点 → 编译零 PortNotFound/TypeMismatch（faces 是一路声明源）；
    // begin 的 in 口悬空 → $graph_input 哨兵绑定 target=该口；边界输入面
    // 携带该口（display=口名）。存档分区已退役——节点 JSON 携带的旧分区
    // 由会话创建兼容读取段等价迁入 settings.boundaryPorts（apply 期端点
    // 解析的数据源），编译口面恒走 faces。
    TEST(GraphAuthoringSessionTest, CompileResolvesDynamicPortEdgesAndSentinel)
    {
        const std::string begin_with_port =
            std::string(
                R"({"nodeId":"n_0","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":")")
            + std::string(BOUNDARY_BEGIN_GUID)
            + R"(","pluginGuid":"{p}"}},"settings":{},)"
            + R"("ports":[{"id":"左值","name":"左值","direction":"input","valueType":"int"}]})";
        const std::string context =
            std::string(R"({"nodes":[)") + begin_with_port + ","
            + RawGraphNodeOf("n_1", BOUNDARY_END_GUID)
            + R"(],"edges":[],"nodeFaces":[{"nodeId":"n_0","ports":[)"
            + R"({"id":"左值","type":"int","direction":"input","label":"左值"}]}]})";
        const SessionHandle s{WithActionCatalog(
            context,
            std::string(
                R"([{"actionId":"{g1}","ports":[{"portId":"seed","portType":"int","direction":"input"},{"portId":"result","portType":"string","direction":"output"}]},)")
                + R"({"actionId":")" + std::string(BOUNDARY_BEGIN_GUID)
                + R"("},{"actionId":")" + std::string(BOUNDARY_END_GUID)
                + R"(","ports":[{"portId":"值","portType":"int","direction":"input"},{"portId":"值","portType":"int","direction":"output"}]}])")};
        ApplyChangeExpectOk(s.p, "addGraphNode", R"({"actionId":"{g1}"})");

        const auto dynamic_port_id = DynamicPortFieldOfComponent(
            DocumentOf(s.p),
            std::string_view(BOUNDARY_BEGIN_GUID),
            std::string_view("portId"));
        ASSERT_EQ(dynamic_port_id, "左值");

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

    /// 应用一个 change 并断言携带 error 级诊断（拒绝路径）；返回原始结果
    /// 文本。
    std::string ApplyChangeExpectRejected(
        GraphAuthoringSessionState* p,
        std::string_view            kind,
        std::string_view            payload)
    {
        const auto result = Compact(
            JsonToUtf8(ApplyChange(p, WrapEnvelope(kind, payload)).Get()));
        EXPECT_NE(result.find("\"severity\":\"error\""), std::string::npos)
            << kind << ": " << result;
        return result;
    }

    // 动态口方向门：begin 的动态口只可作连线源（口对的值来源是调用方图
    // 输入，图内入边会静默顶掉它），end 的动态口只可作连线目标（out 侧是
    // 图输出坐标，不向图内发值）——错向连线在分派层拒绝并出诊断。口 id=
    // 口名（边界条目单 id 形态）。
    TEST(
        GraphAuthoringSessionTest,
        ConnectRejectsBoundaryDynamicPortDirectionViolation)
    {
        const SessionHandle s{WithActionCatalog(
            "",
            std::string(
                R"([{"actionId":"{g1}","ports":[{"portId":"seed","portType":"int","direction":"input"},{"portId":"result","portType":"int","direction":"output"}]},)")
                + R"({"actionId":")" + std::string(BOUNDARY_BEGIN_GUID)
                + R"("},{"actionId":")" + std::string(BOUNDARY_END_GUID)
                + R"(","ports":[{"portId":"值","portType":"int","direction":"input"},{"portId":"值","portType":"int","direction":"output"}]}])")};
        ApplyChangeExpectOk(
            s.p,
            "addNodePort",
            R"({"nodeId":"n_0","name":"左值","direction":"input","valueType":"int"})");
        ApplyChangeExpectOk(
            s.p,
            "addNodePort",
            R"({"nodeId":"n_1","name":"右值","direction":"output","valueType":"int"})");
        ApplyChangeExpectOk(s.p, "addGraphNode", R"({"actionId":"{g1}"})");

        // 错向一：图内边指向 begin 的动态口。
        ApplyChangeExpectRejected(
            s.p,
            "connectGraphPorts",
            R"({"fromNodeId":"n_2","fromPortId":"result","toNodeId":"n_0","toPortId":"左值"})");

        // 错向二：图内边从 end 的动态口发出。
        ApplyChangeExpectRejected(
            s.p,
            "connectGraphPorts",
            R"({"fromNodeId":"n_1","fromPortId":"右值","toNodeId":"n_2","toPortId":"seed"})");
    }

    // 调用节点签名口物化通道（addNodePort portId 稳定物化 id）已随图文档
    // ports 分区退役（绑条目节点口面唯一源=注入 pass 编译期会话现算——
    // ReferenceClosureInjectionPassTest 族锁定；分派期对 faces 口的连线端
    // 点解析已随 nodeFaces 通道落地——ConnectResolvesFacesChannelEndpoints
    // 锁定 UI 预检与 Apply 分派语义同源）。原
    // InvokeNodePortMaterializationResolvesThroughDispatchAndCompile 用例
    // 测的正是被退役的物化通道，随之删除。

    // 预置空图（begin/end 零连线）的编译签名：end 的悬空口（静态「值」与
    // 动态口）不得被推导为图级入参——否则每个新建空图都携带幻影输入
    // 「值」，调用方传值经广播绑定写进 end 转写输出，原值回声进出参面
    // （任务返回码可被图输入直接注入）。
    TEST(
        GraphAuthoringSessionTest,
        CompileEmptyPresetGraphHasNoPhantomBoundaryInputs)
    {
        const SessionHandle s{WithActionCatalog(
            "",
            R"([{"actionId":")" + std::string(BOUNDARY_BEGIN_GUID)
                + R"("},{"actionId":")" + std::string(BOUNDARY_END_GUID)
                + R"(","ports":[{"portId":"值","portType":"int","direction":"input"},{"portId":"值","portType":"int","direction":"output"}]}])")};
        // end 长出参口「评分」（悬空），与静态「值」同为幻影入参候选。
        ApplyChangeExpectOk(
            s.p,
            "addNodePort",
            R"({"nodeId":"n_1","name":"评分","direction":"output","valueType":"int"})");

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
        auto boundary_ports =
            (*plan_object)[std::string_view("boundaryPorts")].as_object();
        ASSERT_TRUE(boundary_ports.has_value());
        auto boundary_inputs =
            (*boundary_ports)[std::string_view("inputs")].as_array();
        ASSERT_TRUE(boundary_inputs.has_value());
        EXPECT_TRUE(boundary_inputs->empty())
            << "preset empty graph must not derive phantom boundary inputs";
    }

    // 会话级 Get 投影（faces 通道形态）：注入的文档口面 merge 进呈现面
    // （方向按 face 条目原样 + dynamic 键），且整份文档过严格 wire 校验
    // （validator 词表含 dynamic）。
    TEST(
        GraphAuthoringSessionTest,
        GetProjectionCarriesDynamicPortOnSessionWire)
    {
        const SessionHandle s{WithActionCatalog(
            std::string(R"({"nodes":[)")
                + RawGraphNodeOf("n_0", BOUNDARY_BEGIN_GUID) + ","
                + RawGraphNodeOf("n_1", BOUNDARY_END_GUID)
                + R"(],"edges":[],"nodeFaces":[)"
                + R"({"nodeId":"n_0","ports":[{"id":"左值","type":"int","direction":"input","label":"左值"}]},)"
                + R"({"nodeId":"n_1","ports":[{"id":"右值","type":"image","direction":"output","label":"右值"}]}]})",
            R"([{"actionId":")" + std::string(BOUNDARY_BEGIN_GUID)
                + R"("},{"actionId":")" + std::string(BOUNDARY_END_GUID)
                + R"(","ports":[{"portId":"值","portType":"int","direction":"input"},{"portId":"值","portType":"int","direction":"output"}]}])")};

        const auto document = DocumentOf(s.p);
        auto       document_result =
            Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
        EXPECT_TRUE(document_result.valid)
            << document_result.error_kind << ": " << document_result.message;

        // begin 的动态口方向按 face 条目（input）；end 同款（output）——
        // faces 是投影动态口的唯一通道。
        EXPECT_EQ(
            DynamicPortFieldOfComponent(
                document,
                std::string_view(BOUNDARY_BEGIN_GUID),
                std::string_view("direction")),
            "input");
        EXPECT_EQ(
            DynamicPortFieldOfComponent(
                document,
                std::string_view(BOUNDARY_END_GUID),
                std::string_view("direction")),
            "output");
    }

    // ======================================================================
    // faces 注入通道（nodeFaces context 键 → Core 既有解析点 → 会话状态
    // → 编译器按节点并集）
    // ======================================================================

    /// A graph context whose n_dst node carries only one declared-face port
    /// (none): the dyn_in port exists solely through the nodeFaces key. Two
    /// edges ride the graph — one targeting the face port, one targeting an
    /// unknown port — so one compile pass exercises both verdicts.
    std::string NodeFacesGraphContext(std::string_view node_faces_json)
    {
        return std::string(R"({"properties":)")
               + RawGraphContextOf(
                   {RawGraphNodeOf("n_begin", BOUNDARY_BEGIN_GUID),
                    RawGraphNodeOf("n_src", "{g1}"),
                    RawGraphNodeOf("n_dst", "{g2}"),
                    RawGraphNodeOf("n_end", BOUNDARY_END_GUID)},
                   {R"({"edgeId":"e_ok","sourceNodeId":"n_src","sourcePortId":"result","targetNodeId":"n_dst","targetPortId":"dyn_in","edgeType":"data"})",
                    R"({"edgeId":"e_bad","sourceNodeId":"n_src","sourcePortId":"result","targetNodeId":"n_dst","targetPortId":"no_such_port","edgeType":"data"})"})
               + R"(,"actionCatalog":[{"actionId":"{g1}","ports":[{"portId":"result","portType":"string","direction":"output"}]},{"actionId":"{g2}"}],)"
               + R"("nodeFaces":)" + std::string(node_faces_json) + "}";
    }

    constexpr std::string_view kValidNodeFaces =
        R"([{"nodeId":"n_dst","ports":[{"id":"dyn_in","type":"string","direction":"input"}]}])";

    // 端到端（tracer 缝）：nodeFaces 给 n_dst 一个声明面没有的口
    // （dyn_in，string，input）——经会话 Compile，编译产物该节点输入口
    // 面包含 dyn_in（绑定计划 target 携带该口）；target 该口的类型兼容
    // 边零诊断通过，target 未知口的边仍 PortNotFound（无效线的编译侧
    // 拒绝语义不变）。
    TEST(GraphAuthoringSessionTest, NodeFacesContextKeyFlowsIntoCompile)
    {
        const SessionHandle s{NodeFacesGraphContext(kValidNodeFaces)};

        const auto outcome = CompileOutcomeOf(s.p);
        // e_bad：未知口仍拒绝（编辑期无效线拒绝语义的编译对照）。
        EXPECT_TRUE(DiagnosticsContainKind(
            outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::PortNotFound));

        const auto not_found = DiagnosticMessageOfKind(
            outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::PortNotFound);
        EXPECT_NE(not_found.find("no_such_port"), std::string::npos);
        // e_ok：dyn_in 经 face 并入 n_dst 输入面，string→string 兼容——
        // 既无 PortNotFound 也无 TypeMismatch。
        EXPECT_EQ(not_found.find("dyn_in"), std::string::npos);
        EXPECT_FALSE(DiagnosticsContainKind(
            outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::TypeMismatch));

        // 编译产物输入口面包含 dyn_in：绑定计划 target 携带该口。
        DasPtr<Das::ExportInterface::IDasJson> compiled;
        ASSERT_EQ(
            GraphAuthoringSessionCompile(s.p, nullptr, compiled.Put()),
            DAS_S_OK);
        const auto plan_text = Compact(JsonToUtf8(compiled.Get()));
        EXPECT_NE(
            plan_text.find("\"targetPortId\":\"dyn_in\""),
            std::string::npos);
    }

    // 畸形 nodeFaces 键（缺 ports 数组 / direction 越权值 / 缺 nodeId）：
    // 该键整体零 faces + 会话继续加载文档——指向 dyn_in 的边退回
    // PortNotFound（降级到「无口」而非「全口」，actionCatalog Wire 降级
    // 同款）。
    TEST(GraphAuthoringSessionTest, NodeFacesMalformedKeyDegradesToZeroFaces)
    {
        const std::array<std::string_view, 3> malformed = {
            // 缺 ports 数组。
            R"([{"nodeId":"n_dst"}])",
            // direction 越权值。
            R"([{"nodeId":"n_dst","ports":[{"id":"dyn_in","type":"string","direction":"sideways"}]}])",
            // 缺 nodeId。
            R"([{"ports":[{"id":"dyn_in","type":"string","direction":"input"}]}])"};
        for (const auto& faces_json : malformed)
        {
            const SessionHandle s{NodeFacesGraphContext(faces_json)};
            // 会话创建成功、文档照常加载。
            const auto document = DocumentOf(s.p);
            auto       doc_obj = document.as_object();
            ASSERT_TRUE(doc_obj.has_value());
            ASSERT_TRUE(doc_obj->contains(std::string_view("graph")));
            // 降级=零 faces：dyn_in 不在口面，指向它的边 PortNotFound。
            const auto outcome = CompileOutcomeOf(s.p);
            EXPECT_TRUE(DiagnosticsContainKind(
                outcome,
                Das::Core::GraphRuntime::CompileDiagnosticKind::PortNotFound))
                << faces_json;
            const auto not_found = DiagnosticMessageOfKind(
                outcome,
                Das::Core::GraphRuntime::CompileDiagnosticKind::PortNotFound);
            EXPECT_NE(not_found.find("dyn_in"), std::string::npos)
                << faces_json;
        }
    }

    // ======================================================================
    // faces 通道（nodeFaces——非边界组件节点的文档口按节点并入）
    // ======================================================================

    // 分派期端点解析吃 faces 通道（UI 预检与服务端 Apply 语义同源）：非
    // 边界组件节点的 nodeFaces 文档口（dyn_in）经 connectGraphPorts 建立
    // 连线——Apply 端点解析消费与编译/投影同源的口面知识（face 胜出同
    // 名声明口），不再落 "port not declared" 拒绝。连线落地后编译零
    // PortNotFound/TypeMismatch（同一口能连=编译不拒）。绑条目节点的口
    // 面唯一源=注入 pass 编译期会话现算（ReferenceClosureInjectionPassTest
    // 族）。
    TEST(GraphAuthoringSessionTest, ConnectResolvesFacesChannelEndpoints)
    {
        constexpr std::string_view catalog =
            R"json([{"actionId":"{gs}","ports":[{"portId":"result","portType":"string","direction":"output"}]},{"actionId":"{g2}","ports":[{"portId":"take","portType":"string","direction":"input"}]}])json";
        const std::string context =
            std::string(R"({"properties":)")
            + RawGraphContextOf(
                {RawGraphNodeOf("n_begin", BOUNDARY_BEGIN_GUID),
                 RawGraphNodeOf("n_src", "{gs}"),
                 RawGraphNodeOf("n_free", "{g2}"),
                 RawGraphNodeOf("n_end", BOUNDARY_END_GUID)},
                {})
            + R"(,"actionCatalog":)" + std::string(catalog)
            + R"(,"nodeFaces":[{"nodeId":"n_free","ports":[)"
            + R"({"id":"dyn_in","type":"string","direction":"input"}]}]})";

        const SessionHandle s{context};

        // 非边界组件节点的 nodeFaces 文档口作 target。
        ApplyChangeExpectOk(
            s.p,
            "connectGraphPorts",
            R"({"fromNodeId":"n_src","fromPortId":"result","toNodeId":"n_free","toPortId":"dyn_in"})");

        // 连线落地后编译干净：faces 口是一路声明源，无 PortNotFound、
        // string→string 无 TypeMismatch。
        const auto outcome = CompileOutcomeOf(s.p);
        EXPECT_FALSE(DiagnosticsContainKind(
            outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::PortNotFound));
        EXPECT_FALSE(DiagnosticsContainKind(
            outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::TypeMismatch));

        // 编译产物携带 faces 口的绑定坐标（连线端到端有效，非仅 Apply 放
        // 行）。
        DasPtr<Das::ExportInterface::IDasJson> compiled;
        ASSERT_EQ(
            GraphAuthoringSessionCompile(s.p, nullptr, compiled.Put()),
            DAS_S_OK);
        const auto plan_text = Compact(JsonToUtf8(compiled.Get()));
        EXPECT_NE(
            plan_text.find("\"targetPortId\":\"dyn_in\""),
            std::string::npos);
    }

    // ======================================================================
    // 编译期自动断线（存储边口面演化复核）：Apply 写回 + 三读路径零修剪
    // ======================================================================

    /// Graph whose n_dst face evolved AFTER the stored edges were drawn:
    /// dyn_in flipped to int (edge e_stale was drawn string→string), keep_in
    /// stayed string (edge e_ok survives). Both edges predate the evolution —
    /// the review must mark e_stale for disconnect and keep e_ok.
    std::string StaleEdgeGraphContext(std::string_view node_faces_json)
    {
        constexpr std::string_view catalog =
            R"json([{"actionId":"{g1}","ports":[{"portId":"result","portType":"string","direction":"output"}]},{"actionId":"{g2}"}])json";
        return std::string(R"({"properties":)")
               + RawGraphContextOf(
                   {RawGraphNodeOf("n_begin", BOUNDARY_BEGIN_GUID),
                    RawGraphNodeOf("n_src", "{g1}"),
                    RawGraphNodeOf("n_dst", "{g2}"),
                    RawGraphNodeOf("n_end", BOUNDARY_END_GUID)},
                   {R"({"edgeId":"e_stale","sourceNodeId":"n_src","sourcePortId":"result","targetNodeId":"n_dst","targetPortId":"dyn_in","edgeType":"data"})",
                    R"({"edgeId":"e_ok","sourceNodeId":"n_src","sourcePortId":"result","targetNodeId":"n_dst","targetPortId":"keep_in","edgeType":"data"})"})
               + R"(,"actionCatalog":)" + std::string(catalog)
               + R"(,"nodeFaces":)" + std::string(node_faces_json) + "}";
    }

    constexpr std::string_view kEvolvedNodeFaces =
        R"([{"nodeId":"n_dst","ports":[{"id":"dyn_in","type":"int","direction":"input"},{"id":"keep_in","type":"string","direction":"input"}]}])";

    /// Apply 序列（口面已演化→apply）：失效存储边被修剪——edges 删除、
    /// revision 推进（恰好 +1，修剪搭本变更的车不另计）、acceptedProperties
    /// 落盘内容正确（修剪后文档=持久化载荷）、WARN 诊断随响应可见；健康
    /// 边与显式级联通道（removeNodePort）语义不重叠，不受波及。
    TEST(GraphAuthoringSessionTest, ApplyPrunesStaleEdgesWithRevision)
    {
        const SessionHandle s{StaleEdgeGraphContext(kEvolvedNodeFaces)};

        // 前置：读路径（compile 预览）已能看见断线诊断，但文档未被修剪。
        const auto pre_outcome = CompileOutcomeOf(s.p);
        EXPECT_TRUE(DiagnosticsContainKind(
            pre_outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::
                StaleEdgeDisconnect));
        const auto stale_message = DiagnosticMessageOfKind(
            pre_outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::
                StaleEdgeDisconnect);
        EXPECT_NE(stale_message.find("e_stale"), std::string::npos);
        EXPECT_EQ(stale_message.find("e_ok"), std::string::npos);

        // Apply 一次 bumping 变更（setValue 落节点 settings）触发复核写回。
        const auto apply_text = Compact(JsonToUtf8(
            ApplyChange(
                s.p,
                WrapEnvelope(
                    "setValue",
                    R"({"target":{"kind":"graphNode","id":"n_src"},"fieldName":"marker","value":7})"))
                .Get()));
        ASSERT_FALSE(apply_text.empty());
        // 无 error 级诊断（修剪 WARN 不构成 applyRejected）。
        EXPECT_EQ(apply_text.find("\"severity\":\"error\""), std::string::npos)
            << apply_text;
        // 断线 WARN 随 Apply 响应可见。
        EXPECT_NE(apply_text.find("e_stale"), std::string::npos) << apply_text;

        // revision 恰好 +1（本变更推进一次；修剪不另计——批量推进域
        // [base, base+1] 的硬约束）。
        EXPECT_NE(apply_text.find("\"revision\":1"), std::string::npos)
            << apply_text;

        // acceptedProperties（Apply 管线既有 persist 的载荷）= 修剪后文档。
        auto apply_root = yyjson::read(apply_text);
        auto apply_obj = apply_root.as_object();
        ASSERT_TRUE(apply_obj.has_value());
        ASSERT_TRUE(
            apply_obj->contains(std::string_view("acceptedProperties")));
        const auto accepted = Compact(
            Das::Utils::SerializeYyjsonValue(
                (*apply_obj)[std::string_view("acceptedProperties")])
                .value_or(""));
        EXPECT_EQ(accepted.find("\"edgeId\":\"e_stale\""), std::string::npos)
            << accepted;
        EXPECT_NE(accepted.find("\"edgeId\":\"e_ok\""), std::string::npos)
            << accepted;
        // 源载荷 revision（version 字段）同步推进。
        EXPECT_NE(accepted.find("\"version\":1"), std::string::npos)
            << accepted;

        // 视图文档同样只余健康边（Get 侧权威快照；图视图以 connections
        // 投影边——toPortId 是边的目标坐标，口本身仍留在节点口面）。
        const auto document = Compact(
            Das::Utils::SerializeYyjsonValue(DocumentOf(s.p)).value_or(""));
        EXPECT_EQ(document.find("\"toPortId\":\"dyn_in\""), std::string::npos)
            << document;
        EXPECT_NE(document.find("\"toPortId\":\"keep_in\""), std::string::npos)
            << document;
    }

    /// 纯读零修剪：Get 与 Compile（三读入口——UI 预览 compile/仓条目
    /// Get/OnTick 执行编译共用 session->Compile）之后文档与 revision 逐
    /// 字节不变；断线诊断在编译出口可见（读路径只产诊断）。
    TEST(GraphAuthoringSessionTest, PureGetAndCompileNeverPrune)
    {
        const SessionHandle s{StaleEdgeGraphContext(kEvolvedNodeFaces)};

        const auto before = Compact(JsonToUtf8(GetDocument(s.p).Get()));

        // OnTick 执行编译形态（执行请求体）与 UI 预览 compile 同走
        // session->Compile——单次调用即覆盖三入口的会话侧行为。
        DasPtr<Das::ExportInterface::IDasJson> compiled;
        ASSERT_EQ(
            GraphAuthoringSessionCompile(s.p, nullptr, compiled.Put()),
            DAS_S_OK);
        // 断线诊断经编译出口可见。
        const auto outcome = CompileOutcomeOf(s.p);
        EXPECT_TRUE(DiagnosticsContainKind(
            outcome,
            Das::Core::GraphRuntime::CompileDiagnosticKind::
                StaleEdgeDisconnect));

        const auto after = Compact(JsonToUtf8(GetDocument(s.p).Get()));
        // 逐字节不变（读无副作用——文档与 revision 均未动）。失效边仍
        // 在 connections（toPortId 是边坐标；口本身仍留在节点口面）。
        EXPECT_EQ(before, after);
        EXPECT_NE(before.find("\"toPortId\":\"dyn_in\""), std::string::npos)
            << before;
        EXPECT_NE(before.find("\"revision\":0"), std::string::npos) << before;
    }

    // ======================================================================
    // faces 画布投影（BuildPortsValue 调用侧合并形态）
    // ======================================================================

    /// Locate one port object of a component's node in the Get graph view by
    /// portId; nullopt when the node or the port is absent.
    std::optional<yyjson::value> NodePortByIdOfComponent(
        const yyjson::value& document,
        std::string_view     component_guid,
        std::string_view     port_id)
    {
        const auto ports = NodePortsOfComponent(document, component_guid);
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
    }

    // context 带 faces 的会话 GetDocument：图视图该节点 ports 数组含动
    // 态口条目——五键（portId/displayLabel/portType/direction/category）
    // 齐全 + 既有 dynamic 标记，不新增投影键。
    TEST(GraphAuthoringSessionTest, NodeFacesProjectIntoCanvasPorts)
    {
        const SessionHandle s{NodeFacesGraphContext(kValidNodeFaces)};

        const auto document = DocumentOf(s.p);
        const auto dyn_in = NodePortByIdOfComponent(
            document,
            std::string_view("{g2}"),
            std::string_view("dyn_in"));
        ASSERT_TRUE(dyn_in.has_value());
        auto port_obj = dyn_in->as_object();
        ASSERT_TRUE(port_obj.has_value());
        EXPECT_TRUE(port_obj->contains(std::string_view("portId")));
        EXPECT_TRUE(port_obj->contains(std::string_view("displayLabel")));
        EXPECT_TRUE(port_obj->contains(std::string_view("portType")));
        EXPECT_TRUE(port_obj->contains(std::string_view("direction")));
        EXPECT_TRUE(port_obj->contains(std::string_view("category")));
        // dynamic 标记是既有通道：faces 口经它呈现，静态口无该键。
        EXPECT_TRUE(port_obj->contains(std::string_view("dynamic")));
        auto direction = (*port_obj)[std::string_view("direction")].as_string();
        ASSERT_TRUE(direction.has_value());
        EXPECT_EQ(*direction, "input");
        auto port_type = (*port_obj)[std::string_view("portType")].as_string();
        ASSERT_TRUE(port_type.has_value());
        EXPECT_EQ(*port_type, "string");
        // label 可选缺失即 id。
        auto label = (*port_obj)[std::string_view("displayLabel")].as_string();
        ASSERT_TRUE(label.has_value());
        EXPECT_EQ(*label, "dyn_in");

        // 声明口（catalog 静态面）不带 dynamic 键——faces 只标记注入的
        // 文档口。
        const auto declared = NodePortByIdOfComponent(
            document,
            std::string_view("{g1}"),
            std::string_view("result"));
        ASSERT_TRUE(declared.has_value());
        auto declared_obj = declared->as_object();
        ASSERT_TRUE(declared_obj.has_value());
        EXPECT_FALSE(declared_obj->contains(std::string_view("dynamic")));

        // 带 faces 的文档仍过严格校验器。
        auto result =
            Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
        EXPECT_TRUE(result.valid)
            << result.error_kind << ": " << result.message;
    }

    // 绑条目调用节点的画布口面投影（原 EntryFacesProjectIntoCanvasPorts
    // 用例的通道）已随 context 预计算 entryFaces 通道退役删除——画布对绑
    // 条目节点的文档口注入由插件侧 GetDocument 投影承担（条目边界口每次
    // Get 现算现注入，不落盘）；Core 侧 nodeFaces 画布投影由
    // NodeFacesProjectIntoCanvasPorts 用例锁定。

    // ------------------------------------------------------------------
    // Boundary component (graph begin/end) authoring session
    // ------------------------------------------------------------------

    constexpr std::string_view kBoundaryBeginGuidText =
        "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E";
    constexpr std::string_view kBoundaryEndGuidText =
        "E1B7C9D3-5F4A-4E2B-8D6C-7A9E1F3B5C2D";

    DasGuid BoundaryGuidOf(std::string_view text)
    {
        return Das::Core::ForeignInterfaceHost::MakeDasGuid(text);
    }

    // begin/end 会话 create 出口（DAS_C_API）直调包装。
    struct BoundarySession
    {
        DasPtr<Das::PluginInterface::IDasTaskAuthoringSession> session;
        explicit BoundarySession(
            std::string_view   guid_text,
            const std::string& context)
        {
            const DasGuid guid = BoundaryGuidOf(guid_text);
            auto          ctx = MakeStr(context);
            CreateGraphBoundaryAuthoringSession(
                &guid,
                ctx.Get(),
                session.Put());
        }
    };

    // GetDocument envelope 的 document 值（envelope = {document,
    // diagnostics}——与 MaaPI 会话同构）。
    yyjson::value BoundaryDocumentOf(
        Das::PluginInterface::IDasTaskAuthoringSession* session)
    {
        DasPtr<Das::ExportInterface::IDasJson> out;
        session->GetDocument(nullptr, out.Put());
        auto json = JsonToUtf8(out.Get());
        auto root = yyjson::read(json);
        auto obj = root.as_object();
        if (!obj || !obj->contains(std::string_view("document")))
        {
            return Das::Utils::MakeYyjsonObject();
        }
        return Das::Utils::CloneYyjsonValue(
            (*obj)[std::string_view("document")]);
    }

    TEST(BoundaryAuthoringSessionTest, CreateAcceptsBeginAndEndGuids)
    {
        for (const auto guid_text :
             {kBoundaryBeginGuidText, kBoundaryEndGuidText})
        {
            const BoundarySession s{guid_text, "{}"};
            ASSERT_NE(s.session.Get(), nullptr);
            void*           queried = nullptr;
            const DasResult hr = s.session->QueryInterface(
                DasIidOf<Das::PluginInterface::IDasTaskAuthoringSession>(),
                &queried);
            EXPECT_EQ(hr, DAS_S_OK);
            ASSERT_NE(queried, nullptr);
            static_cast<Das::PluginInterface::IDasTaskAuthoringSession*>(
                queried)
                ->Release();
        }
    }

    TEST(BoundaryAuthoringSessionTest, CreateRejectsNullPointer)
    {
        const DasGuid guid = BoundaryGuidOf(kBoundaryBeginGuidText);
        Das::PluginInterface::IDasTaskAuthoringSession* out = nullptr;
        EXPECT_EQ(
            CreateGraphBoundaryAuthoringSession(nullptr, nullptr, &out),
            DAS_E_INVALID_POINTER);
        EXPECT_EQ(
            CreateGraphBoundaryAuthoringSession(&guid, nullptr, nullptr),
            DAS_E_INVALID_POINTER);
    }

    TEST(BoundaryAuthoringSessionTest, CreateRejectsNonBoundaryGuid)
    {
        // 反证 guid：DasGraphTask 插件 guid（非 begin/end 边界组件）。
        const DasGuid other =
            BoundaryGuidOf("C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D");
        Das::PluginInterface::IDasTaskAuthoringSession* out = nullptr;
        EXPECT_EQ(
            CreateGraphBoundaryAuthoringSession(&other, nullptr, &out),
            DAS_E_INVALID_ARGUMENT);
        EXPECT_EQ(out, nullptr);
    }

    TEST(BoundaryAuthoringSessionTest, BeginGetDocumentProjectsBothChannels)
    {
        const BoundarySession s{
            kBoundaryBeginGuidText,
            R"({"revision":5,"boundaryPorts":[)"
            R"({"id":"p1","type":"string"},{"id":"p2","type":"int"}]})"};
        const auto document = BoundaryDocumentOf(s.session.Get());

        // 投影过严格校验器（formSequence + chrome 三键 + ports section
        // 与文档协议立法层 lockstep）。
        auto validation =
            Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
        EXPECT_TRUE(validation.valid)
            << validation.error_kind << ": " << validation.message;

        auto doc_obj = document.as_object();
        ASSERT_TRUE(doc_obj.has_value());
        EXPECT_TRUE(doc_obj->contains(std::string_view("revision")));
        auto revision = (*doc_obj)[std::string_view("revision")].as_int();
        ASSERT_TRUE(revision.has_value());
        EXPECT_EQ(*revision, 5);

        // items 写通道：与 boundaryPorts 一一对应（id 顺序一致）。
        auto form_seq = (*doc_obj)[std::string_view("formSequence")];
        auto fs_obj = form_seq.as_object();
        ASSERT_TRUE(fs_obj.has_value());
        auto items = (*fs_obj)[std::string_view("items")].as_array();
        ASSERT_TRUE(items.has_value());
        ASSERT_EQ(items->size(), 2);
        auto first_item = (*items)[0].as_object();
        ASSERT_TRUE(first_item.has_value());
        auto first_id = (*first_item)[std::string_view("id")].as_string();
        ASSERT_TRUE(first_id.has_value());
        EXPECT_EQ(*first_id, "p1");
        // 条目字段：id 文本 + type select（candidates=九词）。
        auto fields = (*first_item)[std::string_view("fields")].as_array();
        ASSERT_TRUE(fields.has_value());
        ASSERT_EQ(fields->size(), 2);
        auto type_field = (*fields)[1].as_object();
        ASSERT_TRUE(type_field.has_value());
        auto input_type = (*type_field)[std::string_view("inputType")].as_int();
        ASSERT_TRUE(input_type.has_value());
        EXPECT_EQ(*input_type, 1); // select
        auto candidates =
            (*type_field)[std::string_view("candidates")].as_array();
        ASSERT_TRUE(candidates.has_value());
        EXPECT_EQ(candidates->size(), 9);

        // chrome 三键非空字符串。
        for (const std::string_view chrome_key :
             {"listTitle", "addButtonLabel", "emptyStateHint"})
        {
            ASSERT_TRUE(fs_obj->contains(chrome_key));
            auto text = (*fs_obj)[chrome_key].as_string();
            ASSERT_TRUE(text.has_value());
            EXPECT_FALSE(text->empty()) << chrome_key;
        }

        // ports 读通道：begin 恒 input。
        auto ports = (*doc_obj)[std::string_view("ports")].as_array();
        ASSERT_TRUE(ports.has_value());
        ASSERT_EQ(ports->size(), 2);
        for (const auto& port : *ports)
        {
            auto port_obj = port.as_object();
            ASSERT_TRUE(port_obj.has_value());
            auto direction =
                (*port_obj)[std::string_view("direction")].as_string();
            ASSERT_TRUE(direction.has_value());
            EXPECT_EQ(*direction, "input");
        }
    }

    TEST(BoundaryAuthoringSessionTest, EndGetDocumentPortsAllOutput)
    {
        const BoundarySession s{
            kBoundaryEndGuidText,
            R"({"boundaryPorts":[{"id":"out1","type":"double"}]})"};
        const auto document = BoundaryDocumentOf(s.session.Get());
        auto       doc_obj = document.as_object();
        ASSERT_TRUE(doc_obj.has_value());
        auto ports = (*doc_obj)[std::string_view("ports")].as_array();
        ASSERT_TRUE(ports.has_value());
        ASSERT_EQ(ports->size(), 1);
        auto port_obj = (*ports)[0].as_object();
        ASSERT_TRUE(port_obj.has_value());
        auto direction = (*port_obj)[std::string_view("direction")].as_string();
        ASSERT_TRUE(direction.has_value());
        EXPECT_EQ(*direction, "output");
    }

    TEST(BoundaryAuthoringSessionTest, EmptyStartProjectsEmptyChannels)
    {
        // 无 boundaryPorts 键：engaged-only 起步态——两通道均为空数组。
        const BoundarySession s{kBoundaryBeginGuidText, R"({"other":1})"};
        const auto            document = BoundaryDocumentOf(s.session.Get());
        auto                  doc_obj = document.as_object();
        ASSERT_TRUE(doc_obj.has_value());
        auto form_seq = (*doc_obj)[std::string_view("formSequence")];
        auto fs_obj = form_seq.as_object();
        ASSERT_TRUE(fs_obj.has_value());
        auto items = (*fs_obj)[std::string_view("items")].as_array();
        ASSERT_TRUE(items.has_value());
        EXPECT_EQ(items->size(), 0);
        auto ports = (*doc_obj)[std::string_view("ports")].as_array();
        ASSERT_TRUE(ports.has_value());
        EXPECT_EQ(ports->size(), 0);
    }

    TEST(BoundaryAuthoringSessionTest, MalformedBoundaryPortsDegradesToEmpty)
    {
        // 畸形状态键整体降级为零口（faces 通道同款降级语义）——投影恒为
        // 合法空文档，不携畸形进 wire。
        const BoundarySession s{
            kBoundaryEndGuidText,
            R"({"boundaryPorts":[{"id":"bad","type":"signal"}]})"};
        const auto document = BoundaryDocumentOf(s.session.Get());
        auto       validation =
            Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
        EXPECT_TRUE(validation.valid)
            << validation.error_kind << ": " << validation.message;
        auto doc_obj = document.as_object();
        ASSERT_TRUE(doc_obj.has_value());
        auto ports = (*doc_obj)[std::string_view("ports")].as_array();
        ASSERT_TRUE(ports.has_value());
        EXPECT_EQ(ports->size(), 0);
    }

    // ------------------------------------------------------------------
    // Boundary session ApplyChange vocabulary（item 家族 + addNodePort
    // 家族 + setValue；payload 形状遵循文档协议 change 校验层锁形）
    // ------------------------------------------------------------------

    std::string BoundaryChange(
        int64_t          base_revision,
        std::string_view kind,
        std::string_view payload)
    {
        std::string out = R"({"baseRevision":)";
        out += std::to_string(base_revision);
        out += R"(,"kind":")";
        out += kind;
        out += R"(","payload":)";
        out += payload;
        out += "}";
        return out;
    }

    yyjson::value BoundaryApplyOf(
        Das::PluginInterface::IDasTaskAuthoringSession* session,
        const std::string&                              change)
    {
        DasPtr<Das::ExportInterface::IDasJson> request;
        ParseDasJsonFromString(change.c_str(), request.Put());
        DasPtr<Das::ExportInterface::IDasJson> out;
        session->ApplyChange(request.Get(), out.Put());
        return yyjson::read(JsonToUtf8(out.Get()));
    }

    // 取 apply 结果的 document。
    yyjson::value ResultDocumentOf(const yyjson::value& result)
    {
        auto obj = result.as_object();
        if (!obj || !obj->contains(std::string_view("document")))
        {
            return Das::Utils::MakeYyjsonObject();
        }
        return Das::Utils::CloneYyjsonValue(
            (*obj)[std::string_view("document")]);
    }

    std::vector<std::string> BoundaryPortIdsOf(const yyjson::value& document)
    {
        std::vector<std::string> ids;
        auto                     doc_obj = document.as_object();
        if (!doc_obj)
        {
            return ids;
        }
        auto ports = (*doc_obj)[std::string_view("ports")].as_array();
        if (!ports)
        {
            return ids;
        }
        for (const auto& port : *ports)
        {
            auto port_obj = port.as_object();
            if (!port_obj)
            {
                continue;
            }
            auto id = (*port_obj)[std::string_view("id")].as_string();
            if (id)
            {
                ids.emplace_back(*id);
            }
        }
        return ids;
    }

    std::string FirstDiagnosticMessageOf(const yyjson::value& result)
    {
        auto obj = result.as_object();
        if (!obj || !obj->contains(std::string_view("diagnostics")))
        {
            return {};
        }
        auto arr = (*obj)[std::string_view("diagnostics")].as_array();
        if (!arr || arr->empty())
        {
            return {};
        }
        auto first = (*arr)[0].as_object();
        if (!first)
        {
            return {};
        }
        auto message = (*first)[std::string_view("message")].as_string();
        return message ? std::string(*message) : std::string{};
    }

    TEST(BoundaryAuthoringSessionTest, AddFormSequenceItemAllocatesEntry)
    {
        // 侧别前缀：begin 会话铸 b_1,b_2,…、
        // end 会话铸 e_1,e_2,…——各侧会话零协调物理不撞。
        const BoundarySession s{kBoundaryBeginGuidText, "{}"};
        const std::string     payload = std::string(R"({"actionId":")")
                                        + std::string(kBoundaryBeginGuidText)
                                        + R"("})";
        const auto            result = BoundaryApplyOf(
            s.session.Get(),
            BoundaryChange(0, "addFormSequenceItem", payload));
        // 成功：document.revision = base+1；envelope 三键齐全。
        auto result_obj = result.as_object();
        ASSERT_TRUE(result_obj.has_value());
        EXPECT_TRUE(
            result_obj->contains(std::string_view("acceptedProperties")));
        EXPECT_TRUE(result_obj->contains(std::string_view("diagnostics")));
        const auto document = ResultDocumentOf(result);
        auto       doc_obj = document.as_object();
        ASSERT_TRUE(doc_obj.has_value());
        auto revision = (*doc_obj)[std::string_view("revision")].as_int();
        ASSERT_TRUE(revision.has_value());
        EXPECT_EQ(*revision, 1);
        const auto ids = BoundaryPortIdsOf(document);
        ASSERT_EQ(ids.size(), 1);
        EXPECT_EQ(ids[0], "b_1");
        // acceptedProperties 回写 boundaryPorts（含默认 type）。
        auto accepted_obj =
            (*result_obj)[std::string_view("acceptedProperties")].as_object();
        ASSERT_TRUE(accepted_obj.has_value());
        EXPECT_TRUE(accepted_obj->contains(std::string_view("boundaryPorts")));
    }

    TEST(BoundaryAuthoringSessionTest, AddFormSequenceItemAllocatesSidePrefix)
    {
        // 侧别前缀逐侧锁定：begin 会话续铸 b_2（占位防撞顺延）、end 会话
        // 铸 e_1——两侧计数器互不干扰，跨侧物理不撞（b_*/e_* 前缀钦定，
        // 图条目根 ports section 的同文档唯一契约不再依赖发射端垫子）。
        {
            const BoundarySession s{
                kBoundaryBeginGuidText,
                R"({"revision":3,"boundaryPorts":[{"id":"b_1","type":"int"}]})"};
            const std::string payload = std::string(R"({"actionId":")")
                                        + std::string(kBoundaryBeginGuidText)
                                        + R"("})";
            const auto        result = BoundaryApplyOf(
                s.session.Get(),
                BoundaryChange(3, "addFormSequenceItem", payload));
            const auto ids = BoundaryPortIdsOf(ResultDocumentOf(result));
            ASSERT_EQ(ids.size(), 2u);
            EXPECT_EQ(ids[0], "b_1");
            EXPECT_EQ(ids[1], "b_2");
        }
        {
            const BoundarySession s{kBoundaryEndGuidText, "{}"};
            const std::string     payload = std::string(R"({"actionId":")")
                                            + std::string(kBoundaryEndGuidText)
                                            + R"("})";
            const auto            result = BoundaryApplyOf(
                s.session.Get(),
                BoundaryChange(0, "addFormSequenceItem", payload));
            const auto ids = BoundaryPortIdsOf(ResultDocumentOf(result));
            ASSERT_EQ(ids.size(), 1u);
            EXPECT_EQ(ids[0], "e_1");
        }
    }

    TEST(BoundaryAuthoringSessionTest, AddNodePortCreatesExplicitEntry)
    {
        const BoundarySession s{kBoundaryBeginGuidText, "{}"};
        const auto            result = BoundaryApplyOf(
            s.session.Get(),
            BoundaryChange(
                0,
                "addNodePort",
                R"({"nodeId":"n_0","name":"p1","direction":"input","valueType":"string"})"));
        const auto document = ResultDocumentOf(result);
        const auto ids = BoundaryPortIdsOf(document);
        ASSERT_EQ(ids.size(), 1);
        EXPECT_EQ(ids[0], "p1");
        // items 写通道同步含 p1（同一文档两面）。
        auto doc_obj = document.as_object();
        ASSERT_TRUE(doc_obj.has_value());
        auto form_seq =
            (*doc_obj)[std::string_view("formSequence")].as_object();
        ASSERT_TRUE(form_seq.has_value());
        auto items = (*form_seq)[std::string_view("items")].as_array();
        ASSERT_TRUE(items.has_value());
        ASSERT_EQ(items->size(), 1);
        auto first_item = (*items)[0].as_object();
        ASSERT_TRUE(first_item.has_value());
        auto item_id = (*first_item)[std::string_view("id")].as_string();
        ASSERT_TRUE(item_id.has_value());
        EXPECT_EQ(*item_id, "p1");
    }

    TEST(BoundaryAuthoringSessionTest, AddNodePortDuplicateRejected)
    {
        const BoundarySession s{kBoundaryBeginGuidText, "{}"};
        const std::string     add_payload =
            R"({"nodeId":"n_0","name":"p1","direction":"input","valueType":"string"})";
        BoundaryApplyOf(
            s.session.Get(),
            BoundaryChange(0, "addNodePort", add_payload));
        const auto result = BoundaryApplyOf(
            s.session.Get(),
            BoundaryChange(1, "addNodePort", add_payload));
        // 重复 id 拒绝：error 诊断 + revision 不推进 + 零突变。
        const auto message = FirstDiagnosticMessageOf(result);
        EXPECT_NE(message.find("duplicates"), std::string::npos);
        const auto document = ResultDocumentOf(result);
        auto       doc_obj = document.as_object();
        ASSERT_TRUE(doc_obj.has_value());
        auto revision = (*doc_obj)[std::string_view("revision")].as_int();
        ASSERT_TRUE(revision.has_value());
        EXPECT_EQ(*revision, 1);
        EXPECT_EQ(BoundaryPortIdsOf(document).size(), 1);
    }

    TEST(BoundaryAuthoringSessionTest, AddNodePortDirectionGateRejectsOpposite)
    {
        // begin 只入：output 方向拒绝（方向门=文档校验规则）。
        const BoundarySession begin_session{kBoundaryBeginGuidText, "{}"};
        const auto            begin_result = BoundaryApplyOf(
            begin_session.session.Get(),
            BoundaryChange(
                0,
                "addNodePort",
                R"({"nodeId":"n_0","name":"p1","direction":"output","valueType":"string"})"));
        const auto begin_message = FirstDiagnosticMessageOf(begin_result);
        EXPECT_NE(begin_message.find("direction"), std::string::npos);
        EXPECT_EQ(BoundaryPortIdsOf(ResultDocumentOf(begin_result)).size(), 0);

        // end 只出：input 方向拒绝。
        const BoundarySession end_session{kBoundaryEndGuidText, "{}"};
        const auto            end_result = BoundaryApplyOf(
            end_session.session.Get(),
            BoundaryChange(
                0,
                "addNodePort",
                R"({"nodeId":"n_1","name":"p1","direction":"input","valueType":"string"})"));
        const auto end_message = FirstDiagnosticMessageOf(end_result);
        EXPECT_NE(end_message.find("direction"), std::string::npos);
        EXPECT_EQ(BoundaryPortIdsOf(ResultDocumentOf(end_result)).size(), 0);
    }

    TEST(
        BoundaryAuthoringSessionTest,
        AddNodePortDisciplineAndVocabularyRejects)
    {
        const BoundarySession s{kBoundaryBeginGuidText, "{}"};
        // 空 id、101 字节 id（>100 UTF-8 上限）、$ 前缀、九词外 type。
        const std::string long_id(101, 'a');
        const std::vector<std::pair<std::string_view, std::string>> cases = {
            {"long",
             std::string(R"({"nodeId":"n_0","name":")") + long_id
                 + R"(","direction":"input","valueType":"string"})"},
            {"empty",
             R"({"nodeId":"n_0","name":"","direction":"input","valueType":"string"})"},
            {"dollar",
             R"({"nodeId":"n_0","name":"$x","direction":"input","valueType":"string"})"},
            {"bad-type",
             R"({"nodeId":"n_0","name":"p1","direction":"input","valueType":"signal"})"}};
        for (const auto& [label, payload] : cases)
        {
            const auto result = BoundaryApplyOf(
                s.session.Get(),
                BoundaryChange(0, "addNodePort", payload));
            EXPECT_NE(
                FirstDiagnosticMessageOf(result).find("addNodePort"),
                std::string::npos)
                << label;
            EXPECT_EQ(BoundaryPortIdsOf(ResultDocumentOf(result)).size(), 0)
                << label;
        }
    }

    TEST(BoundaryAuthoringSessionTest, RemoveItemSurfacesRemovedPortId)
    {
        const BoundarySession s{kBoundaryBeginGuidText, "{}"};
        BoundaryApplyOf(
            s.session.Get(),
            BoundaryChange(
                0,
                "addNodePort",
                R"({"nodeId":"n_0","name":"p1","direction":"input","valueType":"string"})"));
        const auto result = BoundaryApplyOf(
            s.session.Get(),
            BoundaryChange(
                1,
                "removeFormSequenceItem",
                R"({"formSequenceItemId":"p1"})"));
        // diagnostics 携带被删口 id（宿主管线级联删边的对账通道）。
        const auto message = FirstDiagnosticMessageOf(result);
        EXPECT_NE(message.find("p1"), std::string::npos);
        // 条目消失。
        const auto document = ResultDocumentOf(result);
        EXPECT_EQ(BoundaryPortIdsOf(document).size(), 0);
        // removeNodePort 同义路径：删口 + 携带 id。
        BoundaryApplyOf(
            s.session.Get(),
            BoundaryChange(
                2,
                "addNodePort",
                R"({"nodeId":"n_0","name":"p2","direction":"input","valueType":"int"})"));
        const auto removed = BoundaryApplyOf(
            s.session.Get(),
            BoundaryChange(
                3,
                "removeNodePort",
                R"({"nodeId":"n_0","portId":"p2"})"));
        EXPECT_NE(
            FirstDiagnosticMessageOf(removed).find("p2"),
            std::string::npos);
        EXPECT_EQ(BoundaryPortIdsOf(ResultDocumentOf(removed)).size(), 0);
    }

    TEST(BoundaryAuthoringSessionTest, MoveFormSequenceItemReorders)
    {
        const BoundarySession s{kBoundaryBeginGuidText, "{}"};
        int64_t               base = 0;
        for (const auto port : {"p1", "p2", "p3"})
        {
            const std::string payload =
                std::string(R"({"nodeId":"n_0","name":")") + port
                + R"(","direction":"input","valueType":"string"})";
            BoundaryApplyOf(
                s.session.Get(),
                BoundaryChange(base, "addNodePort", payload));
            ++base;
        }
        const auto result = BoundaryApplyOf(
            s.session.Get(),
            BoundaryChange(
                3,
                "moveFormSequenceItem",
                R"({"formSequenceItemId":"p3","destinationIndex":0})"));
        const auto ids = BoundaryPortIdsOf(ResultDocumentOf(result));
        ASSERT_EQ(ids.size(), 3);
        EXPECT_EQ(ids[0], "p3");
        EXPECT_EQ(ids[1], "p1");
        EXPECT_EQ(ids[2], "p2");
    }

    TEST(BoundaryAuthoringSessionTest, RenameNodePortRenamesEntry)
    {
        const BoundarySession s{kBoundaryBeginGuidText, "{}"};
        BoundaryApplyOf(
            s.session.Get(),
            BoundaryChange(
                0,
                "addNodePort",
                R"({"nodeId":"n_0","name":"p1","direction":"input","valueType":"string"})"));
        const auto result = BoundaryApplyOf(
            s.session.Get(),
            BoundaryChange(
                1,
                "renameNodePort",
                R"({"nodeId":"n_0","portId":"p1","name":"renamed"})"));
        const auto ids = BoundaryPortIdsOf(ResultDocumentOf(result));
        ASSERT_EQ(ids.size(), 1);
        EXPECT_EQ(ids[0], "renamed");
    }

    TEST(BoundaryAuthoringSessionTest, SetValueEditsIdAndTypeFields)
    {
        const BoundarySession s{kBoundaryBeginGuidText, "{}"};
        BoundaryApplyOf(
            s.session.Get(),
            BoundaryChange(
                0,
                "addNodePort",
                R"({"nodeId":"n_0","name":"p1","direction":"input","valueType":"string"})"));
        // fieldName=id → 改条目 id。
        const auto rename = BoundaryApplyOf(
            s.session.Get(),
            BoundaryChange(
                1,
                "setValue",
                R"({"target":{"kind":"formSequenceItem","id":"p1"},"fieldName":"id","value":"p9"})"));
        const auto renamed_ids = BoundaryPortIdsOf(ResultDocumentOf(rename));
        ASSERT_EQ(renamed_ids.size(), 1);
        EXPECT_EQ(renamed_ids[0], "p9");
        // fieldName=type → 改条目 type（ports[].type 跟随）。
        const auto retype = BoundaryApplyOf(
            s.session.Get(),
            BoundaryChange(
                2,
                "setValue",
                R"({"target":{"kind":"formSequenceItem","id":"p9"},"fieldName":"type","value":"int"})"));
        const auto document = ResultDocumentOf(retype);
        auto       doc_obj = document.as_object();
        ASSERT_TRUE(doc_obj.has_value());
        auto ports = (*doc_obj)[std::string_view("ports")].as_array();
        ASSERT_TRUE(ports.has_value());
        auto first_port = (*ports)[0].as_object();
        ASSERT_TRUE(first_port.has_value());
        auto type = (*first_port)[std::string_view("type")].as_string();
        ASSERT_TRUE(type.has_value());
        EXPECT_EQ(*type, "int");
        // 未知 fieldName 拒绝（fail-closed）。
        const auto bogus = BoundaryApplyOf(
            s.session.Get(),
            BoundaryChange(
                3,
                "setValue",
                R"({"target":{"kind":"formSequenceItem","id":"p9"},"fieldName":"bogus","value":1})"));
        const auto bogus_message = FirstDiagnosticMessageOf(bogus);
        EXPECT_NE(bogus_message.find("fieldName"), std::string::npos);
    }

    TEST(BoundaryAuthoringSessionTest, UnknownKindRejected)
    {
        const BoundarySession s{kBoundaryBeginGuidText, "{}"};
        const auto            result = BoundaryApplyOf(
            s.session.Get(),
            BoundaryChange(0, "invokeCommand", R"({"commandId":"x"})"));
        const auto message = FirstDiagnosticMessageOf(result);
        EXPECT_NE(message.find("Unsupported change kind"), std::string::npos);
        const auto document = ResultDocumentOf(result);
        auto       doc_obj = document.as_object();
        ASSERT_TRUE(doc_obj.has_value());
        auto revision = (*doc_obj)[std::string_view("revision")].as_int();
        ASSERT_TRUE(revision.has_value());
        EXPECT_EQ(*revision, 0);
        EXPECT_EQ(BoundaryPortIdsOf(document).size(), 0);
    }

    TEST(BoundaryAuthoringSessionTest, BeginDirectionStaysInputAcrossEdits)
    {
        // 方向固化：begin 会话经任何成功变更路径产出的口恒 input。
        const BoundarySession s{kBoundaryBeginGuidText, "{}"};
        BoundaryApplyOf(
            s.session.Get(),
            BoundaryChange(
                0,
                "addNodePort",
                R"({"nodeId":"n_0","name":"p1","direction":"input","valueType":"string"})"));
        BoundaryApplyOf(
            s.session.Get(),
            BoundaryChange(
                1,
                "addFormSequenceItem",
                std::string(R"({"actionId":")")
                    + std::string(kBoundaryBeginGuidText) + R"("})"));
        BoundaryApplyOf(
            s.session.Get(),
            BoundaryChange(
                2,
                "renameNodePort",
                R"({"nodeId":"n_0","portId":"p1","name":"renamed"})"));
        const auto document = BoundaryDocumentOf(s.session.Get());
        auto       doc_obj = document.as_object();
        ASSERT_TRUE(doc_obj.has_value());
        auto ports = (*doc_obj)[std::string_view("ports")].as_array();
        ASSERT_TRUE(ports.has_value());
        EXPECT_EQ(ports->size(), 2);
        for (const auto& port : *ports)
        {
            auto port_obj = port.as_object();
            ASSERT_TRUE(port_obj.has_value());
            auto direction =
                (*port_obj)[std::string_view("direction")].as_string();
            ASSERT_TRUE(direction.has_value());
            EXPECT_EQ(*direction, "input");
        }
    }

    TEST(BoundaryAuthoringSessionTest, CompileReturnsBoundarySignature)
    {
        const BoundarySession s{
            kBoundaryEndGuidText,
            R"({"boundaryPorts":[{"id":"out1","type":"double"}]})"};
        DasPtr<Das::ExportInterface::IDasJson> out;
        s.session->Compile(nullptr, out.Put());
        const auto root = yyjson::read(JsonToUtf8(out.Get()));
        auto       root_obj = root.as_object();
        ASSERT_TRUE(root_obj.has_value());
        EXPECT_TRUE(root_obj->contains(std::string_view("compile")));
        EXPECT_TRUE(root_obj->contains(std::string_view("diagnostics")));
        auto compile_obj = (*root_obj)[std::string_view("compile")].as_object();
        ASSERT_TRUE(compile_obj.has_value());
        auto ports = (*compile_obj)[std::string_view("ports")].as_array();
        ASSERT_TRUE(ports.has_value());
        ASSERT_EQ(ports->size(), 1);
        auto first_port = (*ports)[0].as_object();
        ASSERT_TRUE(first_port.has_value());
        auto id = (*first_port)[std::string_view("id")].as_string();
        ASSERT_TRUE(id.has_value());
        EXPECT_EQ(*id, "out1");
        auto type = (*first_port)[std::string_view("type")].as_string();
        ASSERT_TRUE(type.has_value());
        EXPECT_EQ(*type, "double");
        auto direction =
            (*first_port)[std::string_view("direction")].as_string();
        ASSERT_TRUE(direction.has_value());
        EXPECT_EQ(*direction, "output");
    }

    // ------------------------------------------------------------------
    // begin/end capability 注册面（DasGraphTask 插件托管）的 registry 级
    // 单元验证：manifest tasks 条目（authoring.factoryGuid +
    // executionComponent.componentGuid=组件自身）经既有解析器进
    // TaskDescriptor、既有 AddTaskDescriptor 建 record——registry 现有查
    // 询面按 begin/end 组件 guid 命中（capability QI 链的发现前提；组件
    // guid→record 的反查查询面属后续 plan）。
    // ------------------------------------------------------------------

    TEST(
        BoundaryAuthoringSessionTest,
        BoundaryCapabilityRecordsReachableViaRegistry)
    {
        // 插件 manifest 跨模块钉值契约：注册面输入在 DasGraphTask 插件的
        // manifest 里（本测试在 Core 测试二进制内直读该文件）。
        const auto manifest_path =
            std::filesystem::path(DAS_PATH_LITERAL(__FILE__))
                .parent_path()
                .parent_path()
                .parent_path()
                .parent_path()
                .parent_path()
            / DAS_PATH_LITERAL("das") / DAS_PATH_LITERAL("Plugins")
            / DAS_PATH_LITERAL("DasGraphTask")
            / DAS_PATH_LITERAL("DasGraphTask.json");
        std::ifstream manifest_file(manifest_path);
        ASSERT_TRUE(manifest_file.is_open())
            << Das::Utils::PathToUtf8(manifest_path);
        std::ostringstream manifest_buffer;
        manifest_buffer << manifest_file.rdbuf();
        yyjson::value manifest_root = yyjson::read(manifest_buffer.str());
        auto          manifest_object = manifest_root.as_object();
        ASSERT_TRUE(manifest_object.has_value());
        const auto tasks =
            (*manifest_object)[std::string_view("tasks")].as_object();
        ASSERT_TRUE(tasks.has_value());

        constexpr std::string_view kPluginGuidText =
            "C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D";
        constexpr std::string_view kAuthoringFactoryGuidText =
            "EFC3D51E-B707-4702-97ED-8ADA923E2BF0";

        Das::Core::TaskScheduler::TaskCapabilityRegistry registry;
        uint64_t                                         feature_index = 3;
        for (const std::string_view guid_text :
             {kBoundaryBeginGuidText, kBoundaryEndGuidText})
        {
            ASSERT_TRUE(tasks->contains(guid_text));
            const auto entry = (*tasks)[guid_text].as_object();
            ASSERT_TRUE(entry.has_value());
            Das::Core::ForeignInterfaceHost::TaskDescriptor descriptor;
            ASSERT_NO_THROW(
                Das::Core::ForeignInterfaceHost::ParseTaskDescriptorFromJson(
                    *entry,
                    descriptor));
            registry.AddTaskDescriptor(
                BoundaryGuidOf(kPluginGuidText),
                BoundaryGuidOf(guid_text),
                feature_index,
                descriptor);
            ++feature_index;
        }

        const auto factory_guid = BoundaryGuidOf(kAuthoringFactoryGuidText);
        const auto plugin_guid = BoundaryGuidOf(kPluginGuidText);
        for (const std::string_view guid_text :
             {kBoundaryBeginGuidText, kBoundaryEndGuidText})
        {
            const auto component_guid = BoundaryGuidOf(guid_text);
            ASSERT_NE(registry.FindTask(component_guid), nullptr);
            const auto* authoring = registry.FindAuthoring(component_guid);
            ASSERT_NE(authoring, nullptr);
            EXPECT_TRUE(authoring->authoring_factory_guid == factory_guid);
            EXPECT_TRUE(authoring->plugin_guid == plugin_guid);
            const auto* execution =
                registry.FindExecutionComponent(component_guid);
            ASSERT_NE(execution, nullptr);
            EXPECT_TRUE(*execution == component_guid);
        }
    }

    // =====================================================================
    // 组件绑定扫描 / 节点写回（调度编排支持）——图形状判据知识的
    // GraphRuntime 侧测试。
    // =====================================================================

    DasPtr<Das::ExportInterface::IDasJson> ScanBindings(
        const std::string& document,
        const std::string& catalog = "{}")
    {
        DasPtr<Das::ExportInterface::IDasJson> out;
        const auto                             hr = GraphScanComponentBindings(
            MakeStr(document).Get(),
            MakeStr(catalog).Get(),
            out.Put());
        EXPECT_EQ(hr, DAS_S_OK);
        return out;
    }

    std::string ScanBindingsText(
        const std::string& document,
        const std::string& catalog = "{}")
    {
        const auto bindings = ScanBindings(document, catalog);
        return bindings.Get() ? JsonToUtf8(bindings.Get()) : std::string();
    }

    TEST(ComponentBindingsScanTest, ScanEmptyForNonGraphDocument)
    {
        // 非图文档（formSequence 契约形状，无顶层 nodes）→ 空数组。
        EXPECT_EQ(
            Compact(ScanBindingsText(R"json({
                "viewKind": "formSequence",
                "formSequence": {"items": []}
            })json")),
            "[]");
        EXPECT_EQ(Compact(ScanBindingsText(R"json({"version": 1})json")), "[]");
    }

    TEST(ComponentBindingsScanTest, ScanListsComponentRefNodesWithSettings)
    {
        const auto text = ScanBindingsText(R"json({
            "nodes": [
                {
                    "nodeId": "n_begin",
                    "target": {"targetKind": "componentRef",
                               "componentRef": {"kind": "componentRef",
                                                "componentGuid": "G-1"}},
                    "settings": {"boundaryPorts": [{"id": "p1"}]}
                },
                {"nodeId": "not_component_ref",
                 "target": {"targetKind": "other"}, "settings": {}}
            ],
            "edges": []
        })json");
        const auto compact = Compact(text);
        // componentRef 节点产出一条（nodeId/componentGuid/settings 随行，
        // 无 boundEntryId 键）；非 componentRef 节点跳过。
        EXPECT_NE(compact.find("\"nodeId\":\"n_begin\""), std::string::npos);
        EXPECT_NE(compact.find("\"componentGuid\":\"G-1\""), std::string::npos);
        EXPECT_NE(
            compact.find("\"settings\":{\"boundaryPorts\":[{\"id\":\"p1\"}]}"),
            std::string::npos);
        EXPECT_EQ(compact.find("boundEntryId"), std::string::npos);
        EXPECT_EQ(compact.find("not_component_ref"), std::string::npos);
    }

    TEST(ComponentBindingsScanTest, ScanResolvesBoundEntryViaTaskRepositoryRef)
    {
        const auto catalog = R"json([
            {"actionId": "G-1", "label": "Invoke",
             "config": [
                 {"name": "repositoryRef", "type": "taskRepositoryRef"},
                 {"name": "plain", "type": "int"}
             ]}
        ])json";
        const auto compact = Compact(ScanBindingsText(
            R"json({
                "nodes": [
                    {"nodeId": "n1",
                     "target": {"targetKind": "componentRef",
                                "componentRef": {"kind": "componentRef",
                                                 "componentGuid": "G-1"}},
                     "settings": {"repositoryRef": 7, "plain": 3}}
                ]
            })json",
            catalog));
        EXPECT_NE(compact.find("\"boundEntryId\":7"), std::string::npos);
    }

    TEST(
        ComponentBindingsScanTest,
        ScanSkipsNonPositiveOrMalformedRepositoryRefValues)
    {
        const auto catalog = R"json([
            {"actionId": "G-1", "label": "Invoke",
             "config": [{"name": "repositoryRef", "type": "taskRepositoryRef"}]}
        ])json";
        const auto document = R"json({
            "nodes": [
                {"nodeId": "zero",
                 "target": {"targetKind": "componentRef",
                            "componentRef": {"kind": "componentRef",
                                             "componentGuid": "G-1"}},
                 "settings": {"repositoryRef": 0}},
                {"nodeId": "negative",
                 "target": {"targetKind": "componentRef",
                            "componentRef": {"kind": "componentRef",
                                             "componentGuid": "G-1"}},
                 "settings": {"repositoryRef": -3}},
                {"nodeId": "string_ref",
                 "target": {"targetKind": "componentRef",
                            "componentRef": {"kind": "componentRef",
                                             "componentGuid": "G-1"}},
                 "settings": {"repositoryRef": "7"}},
                {"nodeId": "missing",
                 "target": {"targetKind": "componentRef",
                            "componentRef": {"kind": "componentRef",
                                             "componentGuid": "G-1"}},
                 "settings": {}}
            ]
        })json";
        // 非正/畸形/缺席的 ref 值一律不产 boundEntryId（判据边界：正值才
        // 绑定），节点本身仍列清单。
        EXPECT_EQ(
            Compact(ScanBindingsText(document, catalog)).find("boundEntryId"),
            std::string::npos);
        for (const std::string_view node_id :
             {"zero", "negative", "string_ref", "missing"})
        {
            const auto text = ScanBindingsText(document, catalog);
            EXPECT_NE(
                Compact(text).find(
                    std::string("\"nodeId\":\"") + std::string(node_id) + "\""),
                std::string::npos)
                << node_id;
        }
    }

    TEST(ComponentBindingsScanTest, ScanWithoutCatalogLeavesEntriesUnbound)
    {
        // catalog 缺席/非数组（{} 对象）：无绑定判据——全部按未绑输出，
        // 不报错不阻断。
        const auto document = R"json({
            "nodes": [
                {"nodeId": "n1",
                 "target": {"targetKind": "componentRef",
                            "componentRef": {"kind": "componentRef",
                                             "componentGuid": "G-1"}},
                 "settings": {"repositoryRef": 7}}
            ]
        })json";
        EXPECT_EQ(
            Compact(ScanBindingsText(document)).find("boundEntryId"),
            std::string::npos);
        EXPECT_EQ(
            Compact(ScanBindingsText(document, "{}")).find("boundEntryId"),
            std::string::npos);
    }

    TEST(ComponentBindingsScanTest, ScanRejectsInvalidDocumentJson)
    {
        DasPtr<Das::ExportInterface::IDasJson> out;
        EXPECT_EQ(
            GraphScanComponentBindings(
                MakeStr("{not json").Get(),
                nullptr,
                out.Put()),
            DAS_E_INVALID_JSON);
        EXPECT_EQ(
            GraphScanComponentBindings(nullptr, nullptr, out.Put()),
            DAS_E_INVALID_POINTER);
    }

    std::string WriteNodeDocumentText(
        const std::string& document,
        const std::string& node_id,
        const std::string& settings,
        const std::string& removed_ids = "[]")
    {
        DasPtr<Das::ExportInterface::IDasJson> out;
        const auto hr = GraphWriteComponentNodeDocument(
            MakeStr(document).Get(),
            MakeStr(node_id).Get(),
            MakeStr(settings).Get(),
            MakeStr(removed_ids).Get(),
            out.Put());
        EXPECT_EQ(hr, DAS_S_OK);
        return out.Get() ? JsonToUtf8(out.Get()) : std::string();
    }

    TEST(ComponentBindingsWriteTest, WriteReplacesNodeSettingsAndKeepsRest)
    {
        const auto updated = WriteNodeDocumentText(
            R"json({
                "documentId": "doc",
                "version": 3,
                "nodes": [
                    {"nodeId": "n1",
                     "target": {"targetKind": "componentRef",
                                "componentRef": {"kind": "componentRef",
                                                 "componentGuid": "G-1"}},
                     "settings": {"old": true}}
                ],
                "edges": [],
                "viewData": {"positions": {"n1": [1, 2]}}
            })json",
            "n1",
            R"json({"boundaryPorts": [{"id": "p1", "type": "string"}]})json");
        const auto compact = Compact(updated);
        // settings 整体替换（旧键消失）。
        EXPECT_EQ(compact.find("\"old\"", 0), std::string::npos);
        EXPECT_NE(compact.find("\"boundaryPorts\""), std::string::npos);
        // 其余顶层键与其它节点字段原样保留。
        EXPECT_NE(compact.find("\"documentId\":\"doc\""), std::string::npos);
        EXPECT_NE(compact.find("\"version\":3"), std::string::npos);
        EXPECT_NE(compact.find("\"viewData\""), std::string::npos);
    }

    TEST(ComponentBindingsWriteTest, WriteCascadesEdgesOnRemovedPorts)
    {
        const auto updated = WriteNodeDocumentText(
            R"json({
                "nodes": [
                    {"nodeId": "n1",
                     "target": {"targetKind": "componentRef",
                                "componentRef": {"kind": "componentRef",
                                                 "componentGuid": "G-1"}},
                     "settings": {}},
                    {"nodeId": "n2",
                     "target": {"targetKind": "componentRef",
                                "componentRef": {"kind": "componentRef",
                                                 "componentGuid": "G-2"}},
                     "settings": {}}
                ],
                "edges": [
                    {"edgeId": "e1", "sourceNodeId": "n1",
                     "sourcePortId": "p1", "targetNodeId": "n2",
                     "targetPortId": "in", "edgeType": "data"},
                    {"edgeId": "e2", "sourceNodeId": "n2",
                     "sourcePortId": "out", "targetNodeId": "n1",
                     "targetPortId": "p9", "edgeType": "data"}
                ]
            })json",
            "n1",
            "{}",
            R"json(["p1"])json");
        const auto compact = Compact(updated);
        // 引用（n1, p1）的 e1 双侧删；目标口不在删集的 e2 保留。
        EXPECT_EQ(compact.find("\"edgeId\":\"e1\"", 0), std::string::npos);
        EXPECT_NE(compact.find("\"edgeId\":\"e2\""), std::string::npos);
    }

    TEST(ComponentBindingsWriteTest, WriteUnknownNodeReturnsVerbatimCopy)
    {
        const auto document = R"json({
            "nodes": [
                {"nodeId": "n1",
                 "target": {"targetKind": "componentRef",
                            "componentRef": {"kind": "componentRef",
                                             "componentGuid": "G-1"}},
                 "settings": {"old": true}}
            ]
        })json";
        const auto updated = WriteNodeDocumentText(
            document,
            "missing_node",
            R"json({"boundaryPorts": []})json");
        EXPECT_EQ(Compact(updated), Compact(document));
    }
} // namespace
