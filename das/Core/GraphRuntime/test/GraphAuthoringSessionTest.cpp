#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/Core/TaskScheduler/TaskAuthoringContract.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

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
} // namespace
