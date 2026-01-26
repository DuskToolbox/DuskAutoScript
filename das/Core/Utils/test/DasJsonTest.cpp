#include <das/Utils/fmt.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.DasJson.hpp>
#include <gtest/gtest.h>

using Das::ExportInterface::DasJson;

DAS_NS_ANONYMOUS_DETAILS_BEGIN

struct Dummy
{
    int     a;
    int64_t b;
};

void ToJson(DasJson& out, const Dummy& in)
{
    out.SetIntByName(
        DasReadOnlyString::FromUtf8("a", nullptr),
        static_cast<int64_t>(in.a));
    out.SetIntByName(DasReadOnlyString::FromUtf8("b", nullptr), in.b);
}

const std::string EXPECT_ARRAY_TEST_VALUE = R"({
  "root": [
    {
      "a": 1,
      "b": 3222222222222
    },
    {
      "a": 3,
      "b": 55555555555555555555
    }
  ]
})";

DAS_NS_ANONYMOUS_DETAILS_END

TEST(DasJsonTest, ArrayTest)
{
    DasJson root;
    DasJson array;

    {
        DasJson        first;
        Details::Dummy first_dummy{1, 3222222222222};
        ToJson(first, first_dummy);
        array.SetObjectByIndex(0, first);
    }

    {
        DasJson        second;
        Details::Dummy second_dummy{3, 9223372036854775807};
        ToJson(second, second_dummy);
        array.SetObjectByIndex(1, second);
    }

    root.SetObjectByName(DasReadOnlyString::FromUtf8("root", nullptr), array);

    DAS::DasPtr<IDasReadOnlyString> p_root_string;
    root.Get()->ToString(2, p_root_string.Put());
    std::cout << DasReadOnlyString{p_root_string}.GetUtf8();
    EXPECT_EQ(
        Details::EXPECT_ARRAY_TEST_VALUE,
        std::string_view{DasReadOnlyString{p_root_string}.GetUtf8()});
}
