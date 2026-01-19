#include <DAS/_autogen/idl/abi/DasJson.h>
#include <das/Utils/fmt.h>
#include <gtest/gtest.h>

DAS_NS_ANONYMOUS_DETAILS_BEGIN

struct Dummy
{
    int     a;
    int64_t b;
};

void ToJson(DasJson& out, const Dummy& in)
{
    out.SetTo(
        DasReadOnlyString::FromUtf8("a", nullptr),
        static_cast<int64_t>(in.a));
    out.SetTo(DasReadOnlyString::FromUtf8("b", nullptr), in.b);
}

const std::string EXPECT_ARRAY_TEST_VALUE = R"({
  "root": [
    {
      "a": 1,
      "b": 3222222222222
    },
    {
      "a": 3,
      "b": 55555555555555555
    }
  ]
})";

DAS_NS_ANONYMOUS_DETAILS_END

TEST(DasJsonTest, ArrayTest)
{
    DasJson root{};
    DasJson array{};

    {
        DasJson        first{};
        Details::Dummy first_dummy{1, 3222222222222};
        ToJson(first, first_dummy);
        array.SetTo(0, first);
    }

    {
        DasJson        second{};
        Details::Dummy second_dummy{3, 55555555555555555};
        ToJson(second, second_dummy);
        array.SetTo(1, second);
    }

    root.SetTo(DasReadOnlyString::FromUtf8("root", nullptr), array);

    DAS::DasPtr<IDasReadOnlyString> p_root_string;
    root.Get()->ToString(2, p_root_string.Put());
    std::cout << DasReadOnlyString{p_root_string}.GetUtf8();
    EXPECT_EQ(
        Details::EXPECT_ARRAY_TEST_VALUE,
        std::string_view{DasReadOnlyString{p_root_string}.GetUtf8()});
}
