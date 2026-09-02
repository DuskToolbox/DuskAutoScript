#include "FlowControlErrorCodes.h"
#include "FlowControlErrorLensRegistration.h"

#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/_autogen/idl/abi/IDasErrorLens.h>
#include <gtest/gtest.h>

#include <string>

namespace
{
    using namespace Das;
    using namespace Das::Plugins::DasFlowControl;
    using namespace Das::PluginInterface;

    DasPtr<IDasBasicErrorLens> MakeRegisteredLens()
    {
        auto lens = CreateRegisteredFlowControlErrorLens();
        EXPECT_TRUE(lens);
        return lens;
    }

    std::string GetUtf8Message(
        IDasBasicErrorLens* lens,
        const char*         locale,
        DasResult           error_code)
    {
        const DasReadOnlyString    locale_str{locale};
        DasPtr<IDasReadOnlyString> out;
        const auto                 hr =
            lens->GetErrorMessage(locale_str.Get(), error_code, out.Put());
        if (IsFailed(hr))
        {
            return {};
        }
        const char* text = nullptr;
        EXPECT_EQ(out->GetUtf8(&text), DAS_S_OK);
        return text != nullptr ? text : "";
    }

    // 码表账本单一来源在 FlowControlErrorCodes.h（ALL_FLOW_ERROR_CODES）：
    // 本测试直接遍历它——新增码漏注册 en/zh-cn 条目在即红。
} // namespace

TEST(FlowControlErrorLensTest, FactoryReturnsValidPointer)
{
    auto lens = CreateRegisteredFlowControlErrorLens();
    ASSERT_TRUE(lens);
}

TEST(FlowControlErrorLensTest, EveryCodeCarriesNonEmptyEnAndZhCnMessage)
{
    auto lens = MakeRegisteredLens();
    ASSERT_TRUE(lens);

    for (const auto error_code : ALL_FLOW_ERROR_CODES)
    {
        EXPECT_FALSE(GetUtf8Message(lens.Get(), "en", error_code).empty())
            << "en message missing for error code " << error_code;
        EXPECT_FALSE(GetUtf8Message(lens.Get(), "zh-cn", error_code).empty())
            << "zh-cn message missing for error code " << error_code;
    }
}

TEST(FlowControlErrorLensTest, ReturnsErrorOnUnknownCode)
{
    auto lens = MakeRegisteredLens();
    ASSERT_TRUE(lens);

    const DasReadOnlyString    locale{"en"};
    DasPtr<IDasReadOnlyString> out;
    const auto hr = lens->GetErrorMessage(locale.Get(), DAS_E_FAIL, out.Put());
    EXPECT_EQ(hr, DAS_E_OUT_OF_RANGE);
}
