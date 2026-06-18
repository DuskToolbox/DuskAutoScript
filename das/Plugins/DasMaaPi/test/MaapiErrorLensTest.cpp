#include "../src/MaaPiErrorLensRegistration.h"

#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Plugins/DasMaaPi/MaaPiErrorCodes.h>
#include <das/_autogen/idl/abi/IDasErrorLens.h>
#include <gtest/gtest.h>

#include <string>

namespace
{
    using namespace Das;
    using namespace Das::Plugins::DasMaaPi;
    using namespace Das::PluginInterface;

    DasPtr<IDasBasicErrorLens> MakeRegisteredLens()
    {
        auto lens = CreateRegisteredMaapiErrorLens();
        EXPECT_TRUE(lens);
        return lens;
    }

    std::string GetUtf8Message(
        IDasBasicErrorLens* lens,
        const char*         locale,
        DasResult           error_code)
    {
        DasReadOnlyString          locale_str{locale};
        DasPtr<IDasReadOnlyString> out;
        auto                       hr =
            lens->GetErrorMessage(locale_str.Get(), error_code, out.Put());
        if (IsFailed(hr))
        {
            return {};
        }
        const char* text = nullptr;
        EXPECT_EQ(out->GetUtf8(&text), DAS_S_OK);
        return text ? text : "";
    }

    struct ErrorEntry
    {
        DasResult   code;
        const char* en_keyword;
        const char* zh_cn_text;
    };

    static constexpr ErrorEntry ERROR_ENTRIES[] = {
        {DAS_E_MAAPI_PI_MISSING,
         "PI file not found",
         "PI\xe6\x96\x87\xe4\xbb\xb6\xe7\xbc\xba\xe5\xa4\xb1"},
        {DAS_E_MAAPI_PI_PARSE_FAILED,
         "PI parse/catalog load failed",
         "PI\xe8\xa7\xa3\xe6\x9e\x90/\xe7\x9b\xae\xe5\xbd\x95\xe5\x8a\xa0\xe8\xbd\xbd\xe5\xa4\xb1\xe8\xb4\xa5"},
        {DAS_E_MAAPI_TASK_MISSING,
         "Specified task not found in PI",
         "\xe6\x8c\x87\xe5\xae\x9Atask\xe5\x9c\xa8PI\xe4\xb8\xad\xe4\xb8\x8d\xe5\xad\x98\xe5\x9c\xa8"},
        {DAS_E_MAAPI_OPTION_PARSE_FAILED,
         "Option parse failed during compile",
         "\xe7\xbc\x96\xe8\xaf\x91\xe6\x97\xb6option\xe8\xa7\xa3\xe6\x9e\x90\xe5\xa4\xb1\xe8\xb4\xa5"},
        {DAS_E_MAAPI_EXECUTION_FAILED,
         "MaaFramework execution failed",
         "MaaFramework\xe6\x89\xa7\xe8\xa1\x8c\xe5\xa4\xb1\xe8\xb4\xa5"},
    };

} // namespace

TEST(MaapiErrorLensTest, FactoryReturnsValidPointer)
{
    auto lens = CreateRegisteredMaapiErrorLens();
    ASSERT_TRUE(lens);
}

TEST(MaapiErrorLensTest, EnMessagesCorrect)
{
    auto lens = MakeRegisteredLens();
    ASSERT_TRUE(lens);

    for (const auto& entry : ERROR_ENTRIES)
    {
        auto msg = GetUtf8Message(lens.Get(), "en", entry.code);
        EXPECT_EQ(msg, entry.en_keyword)
            << "en message mismatch for error code " << entry.code;
    }
}

TEST(MaapiErrorLensTest, ZhCnMessagesCorrect)
{
    auto lens = MakeRegisteredLens();
    ASSERT_TRUE(lens);

    for (const auto& entry : ERROR_ENTRIES)
    {
        auto msg = GetUtf8Message(lens.Get(), "zh-cn", entry.code);
        EXPECT_EQ(msg, entry.zh_cn_text)
            << "zh-cn message mismatch for error code " << entry.code;
    }
}

TEST(MaapiErrorLensTest, ReturnsErrorOnUnknownCode)
{
    auto lens = MakeRegisteredLens();
    ASSERT_TRUE(lens);

    DasReadOnlyString          locale{"en"};
    DasPtr<IDasReadOnlyString> out;
    auto hr = lens->GetErrorMessage(locale.Get(), DAS_E_FAIL, out.Put());
    EXPECT_EQ(hr, DAS_E_OUT_OF_RANGE);
}
