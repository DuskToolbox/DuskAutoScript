#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/EnumUtils.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace
{
    enum class TestEnum
    {
        Value1 = 1,
        Value2 = 2
    };

    enum class TestEnum2
    {
        Value1 = 1,
        Value2 = 2
    };
    NLOHMANN_JSON_SERIALIZE_ENUM(
        TestEnum2,
        {{TestEnum2::Value1, "Value1"}, {TestEnum2::Value2, "Value2"}})
}

using namespace nlohmann::literals;

const nlohmann::json j = R"(
{
    "enum_test1": "Value1",
    "enum_test2": "Value2",
    "enum_test3": "Value3",
    "int_enum_test1": 1,
    "int_enum_test2": 2,
    "int_enum_test3": 3
}
)"_json;

TEST(NlohmannJsonTest, DefaultEnumSerialization)
{
    EXPECT_EQ(TestEnum::Value1, j.at("int_enum_test1").get<TestEnum>());
    EXPECT_EQ(TestEnum::Value2, j.at("int_enum_test2").get<TestEnum>());

    EXPECT_THROW(j.at("enum_test1").get<TestEnum>(), nlohmann::json::exception);
}

TEST(NlohmannJsonTest, UserDefinedEnumDeserialization)
{
    EXPECT_EQ(TestEnum2::Value1, j.at("enum_test1").get<TestEnum2>());
    EXPECT_EQ(TestEnum2::Value2, j.at("enum_test2").get<TestEnum2>());

    // 不匹配时会返回第一个枚举值
    EXPECT_EQ(TestEnum2::Value1, j.at("int_enum_test1").get<TestEnum2>());
    EXPECT_EQ(TestEnum2::Value1, j.at("int_enum_test2").get<TestEnum2>());
    EXPECT_EQ(TestEnum2::Value1, j.at("int_enum_test3").get<TestEnum2>());
}

TEST(NlohmannJsonTest, MagicEnumConverter)
{
    EXPECT_EQ(
        TestEnum::Value1,
        DAS::Utils::StringToEnum<TestEnum>(
            j.at("enum_test1").get<std::string>()));
    EXPECT_EQ(
        TestEnum::Value2,
        DAS::Utils::StringToEnum<TestEnum>(
            j.at("enum_test2").get<std::string>()));

    EXPECT_THROW(
        DAS::Utils::StringToEnum<TestEnum>(
            j.at("enum_test3").get<std::string>()),
        DAS::Utils::UnexpectedEnumException);
}