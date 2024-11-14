#include <gtest/gtest.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/i18n/i18n.hpp>
#include <das/Utils/StringUtils.h>
#include <das/Utils/EnumUtils.hpp>
#include <nlohmann/json.hpp>

const static auto text = DAS_UTILS_STRINGUTILS_DEFINE_U8STR(R"(
{
    "type": "int",
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

auto GetJson()
{
    const static auto result = nlohmann::json::parse(text);
    return result;
}

TEST(DasCoreI18n, DasResultDefaultLocaleTest)
{
    const DAS::Core::i18n::I18n<DasResult> test_instance{GetJson()};
    DAS::DasPtr<IDasReadOnlyString>  en_expected{};
    ::CreateIDasReadOnlyStringFromUtf8(
        "Testing error message.",
        en_expected.Put());
    DAS::DasPtr<IDasReadOnlyString> en_result{};
    test_instance.GetErrorMessage(-1, en_result.Put());
    DAS_CORE_LOG_INFO("{}", en_result);

    EXPECT_EQ(DasReadOnlyString{en_expected}, DasReadOnlyString{en_result});
}

TEST(DasCoreI18n, DasResultUserDefinedLocaleTest)
{
    DAS::Core::i18n::I18n<DasResult> test_instance{GetJson()};
    DAS::DasPtr<IDasReadOnlyString>  zh_cn_expected{};
    ::CreateIDasReadOnlyStringFromUtf8(
        static_cast<const char*>(
            DAS_UTILS_STRINGUTILS_DEFINE_U8STR("测试错误消息")),
        zh_cn_expected.Put());
    DAS::DasPtr<IDasReadOnlyString> zh_cn_result{};
    test_instance.GetErrorMessage(u8"zh-cn", -1, zh_cn_result.Put());
    DAS_CORE_LOG_INFO(zh_cn_result);

    EXPECT_EQ(
        DasReadOnlyString{zh_cn_expected},
        DasReadOnlyString{zh_cn_result});
}
