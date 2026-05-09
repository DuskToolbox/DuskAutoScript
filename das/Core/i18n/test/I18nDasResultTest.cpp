#include <das/Core/Logger/Logger.h>
#include <das/Core/i18n/i18n.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/EnumUtils.hpp>
#include <das/Utils/StringUtils.h>
#include <gtest/gtest.h>

namespace
{
    const static auto valid_numeric_schema_text =
        DAS_UTILS_STRINGUTILS_DEFINE_U8STR(R"(
{
    "type": 0,
    "resource":
        {
            "en" :
            {
                "-1": "Testing error message."
            },
            "zh-cn" :
            {
                "-1": "测试错误消息"
            }
        }
}
)");

    yyjson::value ParseJson(const char* const text)
    {
        auto result = Das::Utils::ParseYyjsonFromString(text);
        EXPECT_TRUE(result.has_value());
        return result ? std::move(*result) : yyjson::value{};
    }

    auto GetValidJson() { return ParseJson(valid_numeric_schema_text); }

    void ExpectLookupFailsSafely(const char* const text)
    {
        DAS::Core::i18n::I18n<DasResult> test_instance{ParseJson(text)};
        DAS::DasPtr<IDasReadOnlyString>  result{};

        EXPECT_NE(test_instance.GetErrorMessage(-1, result.Put()), DAS_S_OK);
        EXPECT_EQ(result.Get(), nullptr);
    }
} // namespace

TEST(DasCoreI18n, DasResultDefaultLocaleTest)
{
    const DAS::Core::i18n::I18n<DasResult> test_instance{GetValidJson()};
    DAS::DasPtr<IDasReadOnlyString>        en_expected{};
    ::CreateIDasReadOnlyStringFromUtf8(
        "Testing error message.",
        en_expected.Put());
    DAS::DasPtr<IDasReadOnlyString> en_result{};
    ASSERT_EQ(test_instance.GetErrorMessage(-1, en_result.Put()), DAS_S_OK);
    ASSERT_NE(en_result.Get(), nullptr);
    DAS_CORE_LOG_INFO("{}", en_result);

    EXPECT_EQ(DasReadOnlyString{en_expected}, DasReadOnlyString{en_result});
}

TEST(DasCoreI18n, DasResultUserDefinedLocaleTest)
{
    DAS::Core::i18n::I18n<DasResult> test_instance{GetValidJson()};
    DAS::DasPtr<IDasReadOnlyString>  zh_cn_expected{};
    ::CreateIDasReadOnlyStringFromUtf8(
        static_cast<const char*>(
            DAS_UTILS_STRINGUTILS_DEFINE_U8STR("测试错误消息")),
        zh_cn_expected.Put());
    DAS::DasPtr<IDasReadOnlyString> zh_cn_result{};
    ASSERT_EQ(
        test_instance.GetErrorMessage(u8"zh-cn", -1, zh_cn_result.Put()),
        DAS_S_OK);
    ASSERT_NE(zh_cn_result.Get(), nullptr);
    DAS_CORE_LOG_INFO(zh_cn_result);

    EXPECT_EQ(
        DasReadOnlyString{zh_cn_expected},
        DasReadOnlyString{zh_cn_result});
}

TEST(DasCoreI18n, ParseFailureInvalidJsonLeavesLookupSafe)
{
    ExpectLookupFailsSafely(R"([])");
    ExpectLookupFailsSafely(R"({"resource": {"en": {"-1": "x"}}})");
    ExpectLookupFailsSafely(R"({"type": 0, "resource": []})");
}

TEST(DasCoreI18n, ParseFailureClearsPreviousLookupState)
{
    DAS::Core::i18n::I18n<DasResult> test_instance{GetValidJson()};
    DAS::DasPtr<IDasReadOnlyString>  valid_result{};
    ASSERT_EQ(test_instance.GetErrorMessage(-1, valid_result.Put()), DAS_S_OK);
    ASSERT_NE(valid_result.Get(), nullptr);

    test_instance.ParseFromYyjsonValue(
        ParseJson(R"({"type": 0, "resource": []})"));

    DAS::DasPtr<IDasReadOnlyString> failed_result{};
    EXPECT_NE(test_instance.GetErrorMessage(-1, failed_result.Put()), DAS_S_OK);
    EXPECT_EQ(failed_result.Get(), nullptr);
}
