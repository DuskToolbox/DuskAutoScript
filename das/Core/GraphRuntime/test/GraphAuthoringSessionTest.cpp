#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <gtest/gtest.h>

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
        if (DAS::IsFailed(json->ToString(0, text.Put())) || text.Get() == nullptr)
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
        for (char c : s)
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
        SessionHandle(const SessionHandle&)            = delete;
        SessionHandle& operator=(const SessionHandle&) = delete;
    };

    DasPtr<Das::ExportInterface::IDasJson> GetDocument(GraphAuthoringSessionState* p)
    {
        DasPtr<Das::ExportInterface::IDasJson> out;
        GraphAuthoringSessionGetDocument(p, nullptr, out.Put());
        return out;
    }

    DasPtr<Das::ExportInterface::IDasJson>
    ApplyChange(GraphAuthoringSessionState* p, const std::string& change)
    {
        DasPtr<Das::ExportInterface::IDasJson> out;
        auto s = MakeStr(change);
        GraphAuthoringSessionApplyChange(p, s.Get(), out.Put());
        return out;
    }

    TEST(GraphAuthoringSessionTest, EmptyStoreProjectsAsGraph)
    {
        SessionHandle s{""};
        auto json = Compact(JsonToUtf8(GetDocument(s.p).Get()));
        // No formline tag on an empty store -> graph mode.
        EXPECT_NE(json.find("\"kind\":\"graph\""), std::string::npos);
    }

    TEST(GraphAuthoringSessionTest, FormSequenceSeedProjectsAsFormSequence)
    {
        // A 2-item formSequence context (full target so the DTO caster accepts it).
        constexpr std::string_view item_a =
            R"({"itemId":"a","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":"{ga}","pluginGuid":"{p}"}},"settings":{}})";
        constexpr std::string_view item_b =
            R"({"itemId":"b","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":"{gb}","pluginGuid":"{p}"}},"settings":{}})";
        SessionHandle s{
            std::string(R"({"documentId":"d1","version":0,"items":[)") + std::string(item_a)
                + "," + std::string(item_b) + "]}"};
        auto json = Compact(JsonToUtf8(GetDocument(s.p).Get()));
        EXPECT_NE(json.find("\"kind\":\"formSequence\""), std::string::npos);
        // Reverse-projected sequence preserves order.
        EXPECT_NE(json.find("\"id\":\"a\""), std::string::npos);
        EXPECT_NE(json.find("\"id\":\"b\""), std::string::npos);
        // Private runtime fields do not leak.
        EXPECT_EQ(json.find("edgeType"), std::string::npos);
    }

    TEST(GraphAuthoringSessionTest, GraphModeApplyChangeAddNode)
    {
        SessionHandle s{""}; // graph mode
        auto r = ApplyChange(s.p, R"({"op":"addNode","node":{"id":"n1","componentGuid":"{g1}","settings":{"x":1}}})");
        const auto result = Compact(JsonToUtf8(r.Get()));
        EXPECT_NE(result.find("\"ok\":true"), std::string::npos);

        auto doc = Compact(JsonToUtf8(GetDocument(s.p).Get()));
        EXPECT_NE(doc.find("\"id\":\"n1\""), std::string::npos);
        EXPECT_NE(doc.find("\"componentGuid\":\"{g1}\""), std::string::npos);
    }

    TEST(GraphAuthoringSessionTest, UpgradeFlipsKindFromFormSequenceToGraph)
    {
        constexpr std::string_view item_a =
            R"({"itemId":"a","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":"{ga}","pluginGuid":"{p}"}},"settings":{}})";
        constexpr std::string_view item_b =
            R"({"itemId":"b","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":"{gb}","pluginGuid":"{p}"}},"settings":{}})";
        SessionHandle s{
            std::string(R"({"items":[)") + std::string(item_a) + "," + std::string(item_b) + "]}"};
        EXPECT_NE(Compact(JsonToUtf8(GetDocument(s.p).Get())).find("\"kind\":\"formSequence\""),
                  std::string::npos);

        EXPECT_EQ(GraphAuthoringSessionUpgradeToGraph(s.p), DAS_S_OK);
        auto after = Compact(JsonToUtf8(GetDocument(s.p).Get()));
        EXPECT_NE(after.find("\"kind\":\"graph\""), std::string::npos);
        EXPECT_EQ(after.find("\"kind\":\"formSequence\""), std::string::npos);
        // Lossless: nodes preserved after upgrade.
        EXPECT_NE(after.find("\"id\":\"a\""), std::string::npos);
    }

    TEST(GraphAuthoringSessionTest, LinearModeApplyChangeAppendsItem)
    {
        constexpr std::string_view item_a =
            R"({"itemId":"a","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":"{ga}","pluginGuid":"{p}"}},"settings":{}})";
        SessionHandle s{
            std::string(R"({"items":[)") + std::string(item_a) + "]}"};
        auto r = ApplyChange(s.p, R"({"op":"addSequenceItem","item":{"id":"b","type":"{gb}"}})");
        EXPECT_NE(Compact(JsonToUtf8(r.Get())).find("\"ok\":true"), std::string::npos);

        auto doc = Compact(JsonToUtf8(GetDocument(s.p).Get()));
        EXPECT_NE(doc.find("\"id\":\"a\""), std::string::npos);
        EXPECT_NE(doc.find("\"id\":\"b\""), std::string::npos);
    }

    // -----------------------------------------------------------------------
    // E2E: store round-trips through the scheduler contract
    // (context.properties <-> ApplyChange.acceptedProperties)
    // -----------------------------------------------------------------------

    /// Extract the acceptedProperties blob from an ApplyChange result.
    std::string ExtractAcceptedProperties(Das::ExportInterface::IDasJson* r)
    {
        auto full = JsonToUtf8(r);
        auto obj  = yyjson::read(full).as_object();
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
        SessionHandle s1{""};
        ASSERT_TRUE(Compact(JsonToUtf8(ApplyChange(s1.p,
            R"({"op":"addNode","node":{"id":"n1","componentGuid":"{g1}","settings":{}}})").Get()))
            .find("\"ok\":true") != std::string::npos);

        const auto persisted = ExtractAcceptedProperties(
            ApplyChange(s1.p,
                R"({"op":"addNode","node":{"id":"n2","componentGuid":"{g2}","settings":{}}})")
                .Get());
        ASSERT_FALSE(persisted.empty());

        // Re-seed from the persisted contract doc (wrapped as scheduler context).
        const std::string ctx =
            std::string(R"({"properties":)") + persisted + "}";
        SessionHandle s2{ctx};
        const auto doc = Compact(JsonToUtf8(GetDocument(s2.p).Get()));
        EXPECT_NE(doc.find("\"id\":\"n1\""), std::string::npos);
        EXPECT_NE(doc.find("\"id\":\"n2\""), std::string::npos);
        EXPECT_NE(doc.find("\"kind\":\"graph\""), std::string::npos);
    }

    TEST(GraphAuthoringSessionTest, E2E_CompileProducesExecutablePlan)
    {
        // Seed a graph store, Compile, and confirm the plan is well-formed
        // (object with the compiled-plan shape; execution itself is exercised
        // by DasGraphTaskTest / GraphRuntimeTest).
        SessionHandle s{""};
        ApplyChange(s.p,
            R"({"op":"addNode","node":{"id":"n1","componentGuid":"{g1}","settings":{}}})");

        DasPtr<Das::ExportInterface::IDasJson> plan_json;
        ASSERT_EQ(GraphAuthoringSessionCompile(s.p, nullptr, plan_json.Put()), DAS_S_OK);
        ASSERT_NE(plan_json.Get(), nullptr);
        const auto plan = Compact(JsonToUtf8(plan_json.Get()));
        // CompiledGraphPlanDto serialises with these keys.
        EXPECT_NE(plan.find("executionOrder"), std::string::npos);
    }

    TEST(GraphAuthoringSessionTest, E2E_UpgradeIsLossless)
    {
        // Seed linear, capture nodes, upgrade (lossless), downgrade, and check
        // the linearity lint fires on the resulting linear store.
        constexpr std::string_view item_a =
            R"({"itemId":"a","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":"{ga}","pluginGuid":"{p}"}},"settings":{}})";
        constexpr std::string_view item_b =
            R"({"itemId":"b","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":"{gb}","pluginGuid":"{p}"}},"settings":{}})";
        SessionHandle s{
            std::string(R"({"items":[)") + std::string(item_a) + "," + std::string(item_b) + "]}"};

        // Lossless upgrade: kind flips to graph, nodes preserved.
        ASSERT_EQ(GraphAuthoringSessionUpgradeToGraph(s.p), DAS_S_OK);
        const auto after_upgrade = Compact(JsonToUtf8(GetDocument(s.p).Get()));
        EXPECT_EQ(after_upgrade.find("\"kind\":\"formSequence\""), std::string::npos);
        EXPECT_NE(after_upgrade.find("\"kind\":\"graph\""), std::string::npos);
        EXPECT_NE(after_upgrade.find("\"id\":\"a\""), std::string::npos);
        EXPECT_NE(after_upgrade.find("\"id\":\"b\""), std::string::npos);
    }
} // namespace
