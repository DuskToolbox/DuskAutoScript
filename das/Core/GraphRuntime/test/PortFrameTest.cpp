#include <das/Core/GraphRuntime/PortFrame.h>

#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace
{
    using namespace Das::Core::GraphRuntime;

    // Helper: build a DasGuid from a canonical string.
    DasGuid MakeNode(const std::string_view s)
    {
        return Das::Core::ForeignInterfaceHost::MakeDasGuid(s);
    }

    // Fixed GUIDs for tests.
    const DasGuid kNode1 = MakeNode("11111111-1111-1111-1111-111111111111");
    const DasGuid kNode2 = MakeNode("22222222-2222-2222-2222-222222222222");
    const DasGuid kNode3 = MakeNode("33333333-3333-3333-3333-333333333333");

    PortKey Key(const DasGuid& node, const std::string& port)
    {
        return PortKey{node, port};
    }
} // namespace

// ===========================================================================
// PortValue — type queries
// ===========================================================================

TEST(PortFrameTest, DefaultIsNull)
{
    PortValue v;
    EXPECT_TRUE(v.IsNull());
    EXPECT_EQ(v.GetType(), PortValueType::Null);
    EXPECT_EQ(v.AsInt(), nullptr);
    EXPECT_EQ(v.AsFloat(), nullptr);
}

TEST(PortFrameTest, IntValue)
{
    PortValue v(int64_t{42});
    EXPECT_TRUE(v.IsInt());
    EXPECT_FALSE(v.IsNull());
    EXPECT_EQ(v.GetType(), PortValueType::Int);
    ASSERT_NE(v.AsInt(), nullptr);
    EXPECT_EQ(*v.AsInt(), 42);
}

TEST(PortFrameTest, FloatValue)
{
    PortValue v(3.14);
    EXPECT_TRUE(v.IsFloat());
    EXPECT_EQ(v.GetType(), PortValueType::Float);
    ASSERT_NE(v.AsFloat(), nullptr);
    EXPECT_DOUBLE_EQ(*v.AsFloat(), 3.14);
}

TEST(PortFrameTest, StringValueFromStd)
{
    PortValue v(std::string("hello"));
    EXPECT_TRUE(v.IsString());
    EXPECT_EQ(v.GetType(), PortValueType::String);
    ASSERT_NE(v.AsString(), nullptr);
    EXPECT_EQ(*v.AsString(), "hello");
}

TEST(PortFrameTest, StringValueFromLiteral)
{
    PortValue v("world");
    EXPECT_TRUE(v.IsString());
    ASSERT_NE(v.AsString(), nullptr);
    EXPECT_EQ(*v.AsString(), "world");
}

TEST(PortFrameTest, BoolValue)
{
    PortValue v_true(true);
    PortValue v_false(false);
    EXPECT_TRUE(v_true.IsBool());
    EXPECT_EQ(v_true.GetType(), PortValueType::Bool);
    ASSERT_NE(v_true.AsBool(), nullptr);
    EXPECT_TRUE(*v_true.AsBool());
    ASSERT_NE(v_false.AsBool(), nullptr);
    EXPECT_FALSE(*v_false.AsBool());
}

TEST(PortFrameTest, BaseValueNullptr)
{
    PortValue v(BaseHandle{DAS::DasPtr<IDasBase>{}});
    EXPECT_TRUE(v.IsBase());
    EXPECT_EQ(v.GetType(), PortValueType::Base);
    ASSERT_NE(v.AsBase(), nullptr);
    EXPECT_EQ(v.AsBase()->ptr.Get(), nullptr);
}

TEST(PortFrameTest, ComponentValueNullptr)
{
    PortValue v(ComponentHandle{DAS::DasPtr<IDasBase>{}});
    EXPECT_TRUE(v.IsComponent());
    EXPECT_EQ(v.GetType(), PortValueType::Component);
    ASSERT_NE(v.AsComponent(), nullptr);
    EXPECT_EQ(v.AsComponent()->ptr.Get(), nullptr);
}

TEST(PortFrameTest, ImageValue)
{
    std::vector<uint8_t> pixels = {0xFF, 0x00, 0x80, 0x40};
    PortValue            v(ImageData{std::move(pixels)});
    EXPECT_TRUE(v.IsImage());
    EXPECT_EQ(v.GetType(), PortValueType::Image);
    ASSERT_NE(v.AsImage(), nullptr);
    EXPECT_EQ(v.AsImage()->bytes.size(), 4u);
    EXPECT_EQ(v.AsImage()->bytes[0], 0xFF);
    EXPECT_EQ(v.AsImage()->bytes[3], 0x40);
}

TEST(PortFrameTest, JsonValue)
{
    auto      doc = yyjson::read(R"({"key": 42})");
    PortValue v(JsonData{doc});
    EXPECT_TRUE(v.IsJson());
    EXPECT_EQ(v.GetType(), PortValueType::Json);
    ASSERT_NE(v.AsJson(), nullptr);
}

TEST(PortFrameTest, SignalValue)
{
    PortValue v = PortValue::Signal();
    EXPECT_TRUE(v.IsSignal());
    EXPECT_EQ(v.GetType(), PortValueType::Signal);
    EXPECT_FALSE(v.IsNull());
}

// ===========================================================================
// PortValue — type mismatch returns nullptr
// ===========================================================================

TEST(PortFrameTest, TypeMismatchAccessors)
{
    PortValue v(int64_t{99});
    EXPECT_EQ(v.AsFloat(), nullptr);
    EXPECT_EQ(v.AsString(), nullptr);
    EXPECT_EQ(v.AsBool(), nullptr);
    EXPECT_EQ(v.AsBase(), nullptr);
    EXPECT_EQ(v.AsComponent(), nullptr);
    EXPECT_EQ(v.AsImage(), nullptr);
    EXPECT_EQ(v.AsJson(), nullptr);
}

// ===========================================================================
// PortKey — equality and hashing
// ===========================================================================

TEST(PortFrameTest, PortKeyEqual)
{
    auto a = Key(kNode1, "out");
    auto b = Key(kNode1, "out");
    EXPECT_EQ(a, b);
    EXPECT_FALSE(a != b);
}

TEST(PortFrameTest, PortKeyDiffNode)
{
    auto a = Key(kNode1, "out");
    auto b = Key(kNode2, "out");
    EXPECT_NE(a, b);
}

TEST(PortFrameTest, PortKeyDiffPort)
{
    auto a = Key(kNode1, "out_a");
    auto b = Key(kNode1, "out_b");
    EXPECT_NE(a, b);
}

TEST(PortFrameTest, PortKeyHashEqual)
{
    PortKeyHash hasher;
    auto        a = Key(kNode1, "port");
    auto        b = Key(kNode1, "port");
    EXPECT_EQ(hasher(a), hasher(b));
}

TEST(PortFrameTest, PortKeyHashDiffKey)
{
    PortKeyHash hasher;
    auto        a = Key(kNode1, "port");
    auto        b = Key(kNode2, "port");
    // Hash *should* differ for different nodes (not a strict requirement,
    // but we verify it does in practice).
    EXPECT_NE(hasher(a), hasher(b));
}

// ===========================================================================
// PortFrame — basic CRUD
// ===========================================================================

TEST(PortFrameTest, EmptyFrame)
{
    PortFrame frame;
    EXPECT_TRUE(frame.Empty());
    EXPECT_EQ(frame.Size(), 0u);
    EXPECT_EQ(frame.Find(Key(kNode1, "x")), nullptr);
    EXPECT_FALSE(frame.Contains(Key(kNode1, "x")));
}

TEST(PortFrameTest, SetAndFind)
{
    PortFrame frame;
    frame.Set(Key(kNode1, "output"), PortValue(int64_t{7}));

    EXPECT_FALSE(frame.Empty());
    EXPECT_EQ(frame.Size(), 1u);
    EXPECT_TRUE(frame.Contains(Key(kNode1, "output")));

    auto* pv = frame.Find(Key(kNode1, "output"));
    ASSERT_NE(pv, nullptr);
    EXPECT_TRUE(pv->IsInt());
    EXPECT_EQ(*pv->AsInt(), 7);
}

TEST(PortFrameTest, SetWithSeparateArgs)
{
    PortFrame frame;
    frame.Set(kNode1, std::string("result"), PortValue(2.5));

    auto* pv = frame.Find(Key(kNode1, "result"));
    ASSERT_NE(pv, nullptr);
    EXPECT_TRUE(pv->IsFloat());
    EXPECT_DOUBLE_EQ(*pv->AsFloat(), 2.5);
}

TEST(PortFrameTest, OverwriteExisting)
{
    PortFrame frame;
    frame.Set(Key(kNode1, "out"), PortValue(int64_t{1}));
    frame.Set(Key(kNode1, "out"), PortValue(int64_t{2}));

    EXPECT_EQ(frame.Size(), 1u);
    auto* pv = frame.Find(Key(kNode1, "out"));
    ASSERT_NE(pv, nullptr);
    EXPECT_EQ(*pv->AsInt(), 2);
}

TEST(PortFrameTest, RemoveExisting)
{
    PortFrame frame;
    frame.Set(Key(kNode1, "out"), PortValue(int64_t{1}));
    EXPECT_TRUE(frame.Remove(Key(kNode1, "out")));
    EXPECT_TRUE(frame.Empty());
    EXPECT_EQ(frame.Find(Key(kNode1, "out")), nullptr);
}

TEST(PortFrameTest, RemoveNonexistent)
{
    PortFrame frame;
    EXPECT_FALSE(frame.Remove(Key(kNode1, "out")));
}

TEST(PortFrameTest, Clear)
{
    PortFrame frame;
    frame.Set(Key(kNode1, "a"), PortValue(int64_t{1}));
    frame.Set(Key(kNode2, "b"), PortValue(3.14));
    EXPECT_EQ(frame.Size(), 2u);

    frame.Clear();
    EXPECT_TRUE(frame.Empty());
}

// ===========================================================================
// PortFrame — multiple entries
// ===========================================================================

TEST(PortFrameTest, MultipleEntries)
{
    PortFrame frame;
    frame.Set(Key(kNode1, "out_int"), PortValue(int64_t{42}));
    frame.Set(Key(kNode1, "out_str"), PortValue(std::string("hello")));
    frame.Set(Key(kNode2, "out_flt"), PortValue(1.5));
    frame.Set(Key(kNode3, "out_sig"), PortValue::Signal());

    EXPECT_EQ(frame.Size(), 4u);

    EXPECT_TRUE(frame.Find(Key(kNode1, "out_int"))->IsInt());
    EXPECT_TRUE(frame.Find(Key(kNode1, "out_str"))->IsString());
    EXPECT_TRUE(frame.Find(Key(kNode2, "out_flt"))->IsFloat());
    EXPECT_TRUE(frame.Find(Key(kNode3, "out_sig"))->IsSignal());
}

// ===========================================================================
// PortFrame — iteration
// ===========================================================================

TEST(PortFrameTest, IterateEmpty)
{
    PortFrame   frame;
    std::size_t count = 0;
    for (auto it = frame.begin(); it != frame.end(); ++it)
    {
        ++count;
    }
    EXPECT_EQ(count, 0u);
}

TEST(PortFrameTest, IterateNonEmpty)
{
    PortFrame frame;
    frame.Set(Key(kNode1, "a"), PortValue(int64_t{1}));
    frame.Set(Key(kNode2, "b"), PortValue(int64_t{2}));

    std::size_t count = 0;
    for (const auto& [key, value] : frame)
    {
        EXPECT_TRUE(value.IsInt());
        (void)key;
        ++count;
    }
    EXPECT_EQ(count, 2u);
}

// ===========================================================================
// PortFrame — all 10 value types round-trip
// ===========================================================================

TEST(PortFrameTest, AllValueTypes)
{
    PortFrame frame;

    frame.Set(Key(kNode1, "v_null"), PortValue());
    frame.Set(Key(kNode1, "v_int"), PortValue(int64_t{-100}));
    frame.Set(Key(kNode1, "v_float"), PortValue(2.718));
    frame.Set(Key(kNode1, "v_string"), PortValue(std::string("test")));
    frame.Set(Key(kNode1, "v_bool"), PortValue(true));
    frame.Set(
        Key(kNode1, "v_base"),
        PortValue(BaseHandle{DAS::DasPtr<IDasBase>{}}));
    frame.Set(
        Key(kNode1, "v_comp"),
        PortValue(ComponentHandle{DAS::DasPtr<IDasBase>{}}));
    frame.Set(Key(kNode1, "v_image"), PortValue(ImageData{{0xAA, 0xBB}}));
    frame.Set(Key(kNode1, "v_json"), PortValue(JsonData{yyjson::read("[]")}));
    frame.Set(Key(kNode1, "v_signal"), PortValue::Signal());

    EXPECT_EQ(frame.Size(), 10u);

    EXPECT_TRUE(frame.Find(Key(kNode1, "v_null"))->IsNull());
    EXPECT_EQ(*frame.Find(Key(kNode1, "v_int"))->AsInt(), -100);
    EXPECT_DOUBLE_EQ(*frame.Find(Key(kNode1, "v_float"))->AsFloat(), 2.718);
    EXPECT_EQ(*frame.Find(Key(kNode1, "v_string"))->AsString(), "test");
    EXPECT_TRUE(*frame.Find(Key(kNode1, "v_bool"))->AsBool());
    EXPECT_TRUE(frame.Find(Key(kNode1, "v_base"))->IsBase());
    EXPECT_TRUE(frame.Find(Key(kNode1, "v_comp"))->IsComponent());
    ASSERT_NE(frame.Find(Key(kNode1, "v_image"))->AsImage(), nullptr);
    EXPECT_EQ(frame.Find(Key(kNode1, "v_image"))->AsImage()->bytes.size(), 2u);
    EXPECT_TRUE(frame.Find(Key(kNode1, "v_json"))->IsJson());
    EXPECT_TRUE(frame.Find(Key(kNode1, "v_signal"))->IsSignal());
}

// ===========================================================================
// PortValue — GetStorage variant index matches PortValueType
// ===========================================================================

TEST(PortFrameTest, VariantIndexMatchesEnum)
{
    EXPECT_EQ(
        PortValue().GetStorage().index(),
        static_cast<std::size_t>(PortValueType::Null));
    EXPECT_EQ(
        PortValue(int64_t{0}).GetStorage().index(),
        static_cast<std::size_t>(PortValueType::Int));
    EXPECT_EQ(
        PortValue(0.0).GetStorage().index(),
        static_cast<std::size_t>(PortValueType::Float));
    EXPECT_EQ(
        PortValue(std::string{}).GetStorage().index(),
        static_cast<std::size_t>(PortValueType::String));
    EXPECT_EQ(
        PortValue(false).GetStorage().index(),
        static_cast<std::size_t>(PortValueType::Bool));
    EXPECT_EQ(
        PortValue(BaseHandle{}).GetStorage().index(),
        static_cast<std::size_t>(PortValueType::Base));
    EXPECT_EQ(
        PortValue(ComponentHandle{}).GetStorage().index(),
        static_cast<std::size_t>(PortValueType::Component));
    EXPECT_EQ(
        PortValue(ImageData{}).GetStorage().index(),
        static_cast<std::size_t>(PortValueType::Image));
    EXPECT_EQ(
        PortValue(JsonData{}).GetStorage().index(),
        static_cast<std::size_t>(PortValueType::Json));
    EXPECT_EQ(
        PortValue::Signal().GetStorage().index(),
        static_cast<std::size_t>(PortValueType::Signal));
}
