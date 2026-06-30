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
} // namespace
