#include <das/Core/GraphRuntime/PortFrame.h>

#include <das/_autogen/idl/abi/IDasImage.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasImage.Implements.hpp>
#include <gtest/gtest.h>

#include <concepts>
#include <cstdint>
#include <string>

namespace
{
    using namespace Das::Core::GraphRuntime;

    // 节点 ID 采用 authoring 契约层 AllocateStableNodeId 分配的 "n_N"
    // 短字符串生产形态——帧键是字符串，不是 GUID。
    const std::string NODE1 = "n_1";
    const std::string NODE2 = "n_2";
    const std::string NODE3 = "n_3";

    PortKey Key(const std::string& node, const std::string& port)
    {
        return PortKey{node, port};
    }

    using Das::ExportInterface::IDasImage;

    // 计数型 fake IDasImage：引用语义用例的测量仪——AddRef/Release 计数与
    // 析构次数静态记账，据此断言帧槽位的引用配对（不泄漏、不双计）。
    class CountingFakeImage final
        : public Das::ExportInterface::DasImageImplBase<CountingFakeImage>
    {
    public:
        static int add_ref_count;
        static int release_count;
        static int destroyed_count;

        static void ResetCounters()
        {
            add_ref_count = 0;
            release_count = 0;
            destroyed_count = 0;
        }

        ~CountingFakeImage() { ++destroyed_count; }

        uint32_t DAS_STD_CALL AddRef() override
        {
            ++add_ref_count;
            return DasImageImplBase<CountingFakeImage>::AddRef();
        }

        uint32_t DAS_STD_CALL Release() override
        {
            ++release_count;
            return DasImageImplBase<CountingFakeImage>::Release();
        }

        DAS_IMPL GetSize(Das::ExportInterface::DasSize* p_out_size) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DAS_IMPL GetChannelCount(int32_t* p_out_channel_count) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DAS_IMPL Clip(
            const Das::ExportInterface::DasRect* p_rect,
            Das::ExportInterface::IDasImage**    p_out_image) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DAS_IMPL GetDataSize(uint64_t* p_out_size) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DAS_IMPL GetBinaryBuffer(
            Das::ExportInterface::IDasBinaryBuffer** pp_out_buffer) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DAS_IMPL GetPixelFormat(
            Das::ExportInterface::DasImagePixelFormat* p_out_format) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }
    };

    int CountingFakeImage::add_ref_count = 0;
    int CountingFakeImage::release_count = 0;
    int CountingFakeImage::destroyed_count = 0;

    template <typename SlotType>
    constexpr bool HoldsImageReference()
    {
        return requires(const SlotType& data) {
            {
                data.ptr
            } -> std::same_as<
                const DAS::DasPtr<Das::ExportInterface::IDasImage>&>;
        };
    }

    template <typename SlotType>
    constexpr bool CarriesNoBytesMember()
    {
        return !requires(const SlotType& data) { data.bytes; };
    }
} // namespace

// ===========================================================================
// PortValue — type queries
// ===========================================================================

TEST(PortFrameTest, DefaultIsNull)
{
    const PortValue v;
    EXPECT_TRUE(v.IsNull());
    EXPECT_EQ(v.GetType(), PortValueType::Null);
    EXPECT_EQ(v.AsInt(), nullptr);
    EXPECT_EQ(v.AsFloat(), nullptr);
}

TEST(PortFrameTest, IntValue)
{
    const PortValue v(int64_t{42});
    EXPECT_TRUE(v.IsInt());
    EXPECT_FALSE(v.IsNull());
    EXPECT_EQ(v.GetType(), PortValueType::Int);
    ASSERT_NE(v.AsInt(), nullptr);
    EXPECT_EQ(*v.AsInt(), 42);
}

TEST(PortFrameTest, FloatValue)
{
    const PortValue v(3.14);
    EXPECT_TRUE(v.IsFloat());
    EXPECT_EQ(v.GetType(), PortValueType::Float);
    ASSERT_NE(v.AsFloat(), nullptr);
    EXPECT_DOUBLE_EQ(*v.AsFloat(), 3.14);
}

TEST(PortFrameTest, StringValueFromStd)
{
    const PortValue v(std::string("hello"));
    EXPECT_TRUE(v.IsString());
    EXPECT_EQ(v.GetType(), PortValueType::String);
    ASSERT_NE(v.AsString(), nullptr);
    EXPECT_EQ(*v.AsString(), "hello");
}

TEST(PortFrameTest, StringValueFromLiteral)
{
    const PortValue v("world");
    EXPECT_TRUE(v.IsString());
    ASSERT_NE(v.AsString(), nullptr);
    EXPECT_EQ(*v.AsString(), "world");
}

TEST(PortFrameTest, BoolValue)
{
    const PortValue v_true(true);
    const PortValue v_false(false);
    EXPECT_TRUE(v_true.IsBool());
    EXPECT_EQ(v_true.GetType(), PortValueType::Bool);
    ASSERT_NE(v_true.AsBool(), nullptr);
    EXPECT_TRUE(*v_true.AsBool());
    ASSERT_NE(v_false.AsBool(), nullptr);
    EXPECT_FALSE(*v_false.AsBool());
}

TEST(PortFrameTest, BaseValueNullptr)
{
    const PortValue v(BaseHandle{DAS::DasPtr<IDasBase>{}});
    EXPECT_TRUE(v.IsBase());
    EXPECT_EQ(v.GetType(), PortValueType::Base);
    ASSERT_NE(v.AsBase(), nullptr);
    EXPECT_EQ(v.AsBase()->ptr.Get(), nullptr);
}

TEST(PortFrameTest, ComponentValueNullptr)
{
    const PortValue v(ComponentHandle{DAS::DasPtr<IDasBase>{}});
    EXPECT_TRUE(v.IsComponent());
    EXPECT_EQ(v.GetType(), PortValueType::Component);
    ASSERT_NE(v.AsComponent(), nullptr);
    EXPECT_EQ(v.AsComponent()->ptr.Get(), nullptr);
}

TEST(PortFrameTest, ImageValue)
{
    const PortValue v(ImageData{DAS::DasPtr<IDasImage>{}});
    EXPECT_TRUE(v.IsImage());
    EXPECT_EQ(v.GetType(), PortValueType::Image);
    ASSERT_NE(v.AsImage(), nullptr);
    EXPECT_EQ(v.AsImage()->ptr.Get(), nullptr);
}

// ===========================================================================
// PortValue — image slot reference form
// ===========================================================================

TEST(PortFrameTest, ImageSlot_HoldsReferenceNotBytes)
{
    // 引用形态约束：帧 image 槽位以 DasPtr<IDasImage> 持有图像对象引用
    // （引用计数共享、零拷贝），不再以字节缓冲承载像素。探针经模板实例
    // 化进入依赖语境——槽位仍是 bytes 时成员约束不满足、探针取值 false，
    // 断言在此失败；引用形态就位后两个探针均为 true。
    EXPECT_TRUE((HoldsImageReference<ImageData>()));
    EXPECT_TRUE((CarriesNoBytesMember<ImageData>()));
}

TEST(PortFrameTest, ImageSlot_SurvivesOriginalHolderRelease)
{
    CountingFakeImage::ResetCounters();
    IDasImage* raw = nullptr;
    {
        PortValue v;
        {
            Das::DasPtr<IDasImage> owner = CountingFakeImage::Make();
            raw = owner.Get();
            v = PortValue(ImageData{owner});
        }
        // 原持有方已析构，帧值槽位的引用仍钉住对象——身份不变、未悬垂
        EXPECT_EQ(CountingFakeImage::destroyed_count, 0);
        ASSERT_NE(v.AsImage(), nullptr);
        EXPECT_EQ(v.AsImage()->ptr.Get(), raw);
    }
    // 槽位随 PortValue 析构释放最后一个引用：恰好一次析构、计数配对
    EXPECT_EQ(CountingFakeImage::destroyed_count, 1);
    EXPECT_EQ(
        CountingFakeImage::add_ref_count,
        CountingFakeImage::release_count);
}

TEST(PortFrameTest, ImageSlot_CopySharesReference)
{
    CountingFakeImage::ResetCounters();
    {
        Das::DasPtr<IDasImage> owner = CountingFakeImage::Make();
        const PortValue        original(ImageData{owner});
        const PortValue        copied(original);

        ASSERT_NE(original.AsImage(), nullptr);
        ASSERT_NE(copied.AsImage(), nullptr);
        // variant 拷贝共享同一对象引用，不复制图像数据
        EXPECT_EQ(original.AsImage()->ptr.Get(), copied.AsImage()->ptr.Get());
        EXPECT_EQ(original.AsImage()->ptr.Get(), owner.Get());
        // 原持有方 + 两份 PortValue = 3 个活跃引用，不多不少
        EXPECT_EQ(
            CountingFakeImage::add_ref_count - CountingFakeImage::release_count,
            3);
    }
    EXPECT_EQ(CountingFakeImage::destroyed_count, 1);
    EXPECT_EQ(
        CountingFakeImage::add_ref_count,
        CountingFakeImage::release_count);
}

TEST(PortFrameTest, PortValueTypeEnumValuesPinned)
{
    // 槽位形态改造不触碰判别面：十类枚举值序钉住，Image 仍为 7
    EXPECT_EQ(static_cast<int>(PortValueType::Null), 0);
    EXPECT_EQ(static_cast<int>(PortValueType::Int), 1);
    EXPECT_EQ(static_cast<int>(PortValueType::Float), 2);
    EXPECT_EQ(static_cast<int>(PortValueType::String), 3);
    EXPECT_EQ(static_cast<int>(PortValueType::Bool), 4);
    EXPECT_EQ(static_cast<int>(PortValueType::Base), 5);
    EXPECT_EQ(static_cast<int>(PortValueType::Component), 6);
    EXPECT_EQ(static_cast<int>(PortValueType::Image), 7);
    EXPECT_EQ(static_cast<int>(PortValueType::Json), 8);
    EXPECT_EQ(static_cast<int>(PortValueType::Signal), 9);
}

TEST(PortFrameTest, JsonValue)
{
    auto            doc = yyjson::read(R"({"key": 42})");
    const PortValue v(JsonData{doc});
    EXPECT_TRUE(v.IsJson());
    EXPECT_EQ(v.GetType(), PortValueType::Json);
    ASSERT_NE(v.AsJson(), nullptr);
}

TEST(PortFrameTest, SignalValue)
{
    const PortValue v = PortValue::Signal();
    EXPECT_TRUE(v.IsSignal());
    EXPECT_EQ(v.GetType(), PortValueType::Signal);
    EXPECT_FALSE(v.IsNull());
}

// ===========================================================================
// PortValue — type mismatch returns nullptr
// ===========================================================================

TEST(PortFrameTest, TypeMismatchAccessors)
{
    const PortValue v(int64_t{99});
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
    auto a = Key(NODE1, "out");
    auto b = Key(NODE1, "out");
    EXPECT_EQ(a, b);
    EXPECT_FALSE(a != b);
}

TEST(PortFrameTest, PortKeyDiffNode)
{
    auto a = Key(NODE1, "out");
    auto b = Key(NODE2, "out");
    EXPECT_NE(a, b);
}

TEST(PortFrameTest, PortKeyDiffPort)
{
    auto a = Key(NODE1, "out_a");
    auto b = Key(NODE1, "out_b");
    EXPECT_NE(a, b);
}

TEST(PortFrameTest, PortKeyHashEqual)
{
    const PortKeyHash hasher;
    auto              a = Key(NODE1, "port");
    auto              b = Key(NODE1, "port");
    EXPECT_EQ(hasher(a), hasher(b));
}

TEST(PortFrameTest, PortKeyHashDiffKey)
{
    const PortKeyHash hasher;
    auto              a = Key(NODE1, "port");
    auto              b = Key(NODE2, "port");
    // Hash *should* differ for different nodes (not a strict requirement,
    // but we verify it does in practice).
    EXPECT_NE(hasher(a), hasher(b));
}

// ===========================================================================
// PortFrame — basic CRUD
// ===========================================================================

TEST(PortFrameTest, EmptyFrame)
{
    const PortFrame frame;
    EXPECT_TRUE(frame.Empty());
    EXPECT_EQ(frame.Size(), 0u);
    EXPECT_EQ(frame.Find(Key(NODE1, "x")), nullptr);
    EXPECT_FALSE(frame.Contains(Key(NODE1, "x")));
}

TEST(PortFrameTest, SetAndFind)
{
    PortFrame frame;
    frame.Set(Key(NODE1, "output"), PortValue(int64_t{7}));

    EXPECT_FALSE(frame.Empty());
    EXPECT_EQ(frame.Size(), 1u);
    EXPECT_TRUE(frame.Contains(Key(NODE1, "output")));

    auto* pv = frame.Find(Key(NODE1, "output"));
    ASSERT_NE(pv, nullptr);
    EXPECT_TRUE(pv->IsInt());
    EXPECT_EQ(*pv->AsInt(), 7);
}

TEST(PortFrameTest, SetWithSeparateArgs)
{
    PortFrame frame;
    frame.Set(NODE1, std::string("result"), PortValue(2.5));

    auto* pv = frame.Find(Key(NODE1, "result"));
    ASSERT_NE(pv, nullptr);
    EXPECT_TRUE(pv->IsFloat());
    EXPECT_DOUBLE_EQ(*pv->AsFloat(), 2.5);
}

TEST(PortFrameTest, OverwriteExisting)
{
    PortFrame frame;
    frame.Set(Key(NODE1, "out"), PortValue(int64_t{1}));
    frame.Set(Key(NODE1, "out"), PortValue(int64_t{2}));

    EXPECT_EQ(frame.Size(), 1u);
    auto* pv = frame.Find(Key(NODE1, "out"));
    ASSERT_NE(pv, nullptr);
    EXPECT_EQ(*pv->AsInt(), 2);
}

TEST(PortFrameTest, RemoveExisting)
{
    PortFrame frame;
    frame.Set(Key(NODE1, "out"), PortValue(int64_t{1}));
    EXPECT_TRUE(frame.Remove(Key(NODE1, "out")));
    EXPECT_TRUE(frame.Empty());
    EXPECT_EQ(frame.Find(Key(NODE1, "out")), nullptr);
}

TEST(PortFrameTest, RemoveNonexistent)
{
    PortFrame frame;
    EXPECT_FALSE(frame.Remove(Key(NODE1, "out")));
}

TEST(PortFrameTest, Clear)
{
    PortFrame frame;
    frame.Set(Key(NODE1, "a"), PortValue(int64_t{1}));
    frame.Set(Key(NODE2, "b"), PortValue(3.14));
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
    frame.Set(Key(NODE1, "out_int"), PortValue(int64_t{42}));
    frame.Set(Key(NODE1, "out_str"), PortValue(std::string("hello")));
    frame.Set(Key(NODE2, "out_flt"), PortValue(1.5));
    frame.Set(Key(NODE3, "out_sig"), PortValue::Signal());

    EXPECT_EQ(frame.Size(), 4u);

    EXPECT_TRUE(frame.Find(Key(NODE1, "out_int"))->IsInt());
    EXPECT_TRUE(frame.Find(Key(NODE1, "out_str"))->IsString());
    EXPECT_TRUE(frame.Find(Key(NODE2, "out_flt"))->IsFloat());
    EXPECT_TRUE(frame.Find(Key(NODE3, "out_sig"))->IsSignal());
}

// ===========================================================================
// PortFrame — signal clearing at loop iteration boundaries
// ===========================================================================

TEST(PortFrameTest, ClearSignalsByNodeOnlyDropsSignalsForNode)
{
    PortFrame frame;
    frame.Set(NODE1, "sig", PortValue::Signal());
    frame.Set(NODE1, "data", PortValue(int64_t{5}));
    frame.Set(NODE2, "sig", PortValue::Signal());

    EXPECT_EQ(frame.ClearSignalsByNode(NODE1), 1u);
    EXPECT_EQ(frame.Find(Key(NODE1, "sig")), nullptr);
    EXPECT_NE(frame.Find(Key(NODE1, "data")), nullptr);
    EXPECT_NE(frame.Find(Key(NODE2, "sig")), nullptr);
}

// ===========================================================================
// PortFrame — iteration
// ===========================================================================

TEST(PortFrameTest, IterateEmpty)
{
    const PortFrame frame;
    std::size_t     count = 0;
    for (auto it = frame.begin(); it != frame.end(); ++it)
    {
        ++count;
    }
    EXPECT_EQ(count, 0u);
}

TEST(PortFrameTest, IterateNonEmpty)
{
    PortFrame frame;
    frame.Set(Key(NODE1, "a"), PortValue(int64_t{1}));
    frame.Set(Key(NODE2, "b"), PortValue(int64_t{2}));

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

    frame.Set(Key(NODE1, "v_null"), PortValue());
    frame.Set(Key(NODE1, "v_int"), PortValue(int64_t{-100}));
    frame.Set(Key(NODE1, "v_float"), PortValue(2.718));
    frame.Set(Key(NODE1, "v_string"), PortValue(std::string("test")));
    frame.Set(Key(NODE1, "v_bool"), PortValue(true));
    frame.Set(
        Key(NODE1, "v_base"),
        PortValue(BaseHandle{DAS::DasPtr<IDasBase>{}}));
    frame.Set(
        Key(NODE1, "v_comp"),
        PortValue(ComponentHandle{DAS::DasPtr<IDasBase>{}}));
    frame.Set(
        Key(NODE1, "v_image"),
        PortValue(ImageData{DAS::DasPtr<IDasImage>{}}));
    frame.Set(Key(NODE1, "v_json"), PortValue(JsonData{yyjson::read("[]")}));
    frame.Set(Key(NODE1, "v_signal"), PortValue::Signal());

    EXPECT_EQ(frame.Size(), 10u);

    EXPECT_TRUE(frame.Find(Key(NODE1, "v_null"))->IsNull());
    EXPECT_EQ(*frame.Find(Key(NODE1, "v_int"))->AsInt(), -100);
    EXPECT_DOUBLE_EQ(*frame.Find(Key(NODE1, "v_float"))->AsFloat(), 2.718);
    EXPECT_EQ(*frame.Find(Key(NODE1, "v_string"))->AsString(), "test");
    EXPECT_TRUE(*frame.Find(Key(NODE1, "v_bool"))->AsBool());
    EXPECT_TRUE(frame.Find(Key(NODE1, "v_base"))->IsBase());
    EXPECT_TRUE(frame.Find(Key(NODE1, "v_comp"))->IsComponent());
    ASSERT_NE(frame.Find(Key(NODE1, "v_image"))->AsImage(), nullptr);
    EXPECT_TRUE(frame.Find(Key(NODE1, "v_json"))->IsJson());
    EXPECT_TRUE(frame.Find(Key(NODE1, "v_signal"))->IsSignal());
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
