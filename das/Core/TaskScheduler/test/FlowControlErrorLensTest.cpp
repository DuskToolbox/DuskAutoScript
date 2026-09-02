#include "FlowControlErrorLensRegistration.h"

#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Plugins/DasFlowControl/FlowControlErrorCodes.h>
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
        const auto                 hr = lens->GetErrorMessage(
            locale_str.Get(), error_code, out.Put());
        if (IsFailed(hr))
        {
            return {};
        }
        const char* text = nullptr;
        EXPECT_EQ(out->GetUtf8(&text), DAS_S_OK);
        return text != nullptr ? text : "";
    }

    struct ErrorEntry
    {
        DasResult   code;
        const char* en_text;
        const char* zh_cn_text;
    };

    // 11 个码全量账本：launch 4 + invokeRepository 6 + resolve 预留 1。
    // launch 四码文案与 i18n 表 flow.diagnostic.*（en.json/zh-cn.json）逐字
    // 一致——lens 与端口 diagnostic 载荷是同一人话的两个出口，文案漂移在此
    // 即红。
    static constexpr ErrorEntry ERROR_ENTRIES[] = {
        {DAS_E_FLOW_LAUNCH_EXE_PATH_MISSING,
         "No executable path is configured for the launch component.",
         "\xe5\x90\xaf\xe5\x8a\xa8\xe7\xbb\x84\xe4\xbb\xb6\xe6\x9c\xaa\xe9"
         "\x85\x8d\xe7\xbd\xae\xe5\x8f\xaf\xe6\x89\xa7\xe8\xa1\x8c\xe6\x96"
         "\x87\xe4\xbb\xb6\xe8\xb7\xaf\xe5\xbe\x84\xe3\x80\x82"},
        {DAS_E_FLOW_LAUNCH_SPAWN_FAILED,
         "Failed to start the configured executable.",
         "\xe5\x90\xaf\xe5\x8a\xa8\xe9\x85\x8d\xe7\xbd\xae\xe7\x9a\x84\xe5"
         "\x8f\xaf\xe6\x89\xa7\xe8\xa1\x8c\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4"
         "\xb1\xe8\xb4\xa5\xe3\x80\x82"},
        {DAS_E_FLOW_LAUNCH_WINDOW_TIMEOUT,
         "Timed out waiting for the target window to appear.",
         "\xe7\xad\x89\xe5\xbe\x85\xe7\x9b\xae\xe6\xa0\x87\xe7\xaa\x97\xe5"
         "\x8f\xa3\xe5\x87\xba\xe7\x8e\xb0\xe8\xb6\x85\xe6\x97\xb6\xe3\x80"
         "\x82"},
        {DAS_E_FLOW_LAUNCH_WINDOW_AMBIGUOUS,
         "Multiple windows matched the wait configuration; refine the match "
         "pattern.",
         "\xe5\xa4\x9a\xe4\xb8\xaa\xe7\xaa\x97\xe5\x8f\xa3\xe5\x8c\xb9\xe9"
         "\x85\x8d\xe7\xad\x89\xe5\xbe\x85\xe9\x85\x8d\xe7\xbd\xae\xef\xbc"
         "\x8c\xe8\xaf\xb7\xe6\x94\xb6\xe7\xaa\x84\xe5\x8c\xb9\xe9\x85\x8d"
         "\xe6\x9d\xa1\xe4\xbb\xb6\xe3\x80\x82"},
        {DAS_E_FLOW_INVOKE_SNAPSHOT_MISSING,
         "Repository invoke requires a compiled child snapshot.",
         "\xe8\xb0\x83\xe7\x94\xa8\xe4\xbb\x93\xe5\xba\x93\xe7\xbb\x84\xe4"
         "\xbb\xb6\xe9\x9c\x80\xe8\xa6\x81\xe7\xbc\x96\xe8\xaf\x91\xe5\xad"
         "\x90\xe5\xbf\xab\xe7\x85\xa7\xe3\x80\x82"},
        {DAS_E_FLOW_INVOKE_SNAPSHOT_INVALID,
         "Compiled child snapshot JSON is not valid.",
         "\xe7\xbc\x96\xe8\xaf\x91\xe5\xad\x90\xe5\xbf\xab\xe7\x85\xa7\x20"
         "JSON\x20\xe6\x97\xa0\xe6\x95\x88\xe3\x80\x82"},
        {DAS_E_FLOW_INVOKE_SNAPSHOT_VERSION_UNSUPPORTED,
         "Compiled child snapshot version is not supported.",
         "\xe7\xbc\x96\xe8\xaf\x91\xe5\xad\x90\xe5\xbf\xab\xe7\x85\xa7\xe7"
         "\x89\x88\xe6\x9c\xac\xe4\xb8\x8d\xe5\x8f\x97\xe6\x94\xaf\xe6\x8c"
         "\x81\xe3\x80\x82"},
        {DAS_E_FLOW_INVOKE_HOST_UNAVAILABLE,
         "Task component host is unavailable.",
         "\xe4\xbb\xbb\xe5\x8a\xa1\xe7\xbb\x84\xe4\xbb\xb6\xe5\xae\xbf\xe4"
         "\xb8\xbb\xe4\xb8\x8d\xe5\x8f\xaf\xe7\x94\xa8\xe3\x80\x82"},
        {DAS_E_FLOW_INVOKE_CHILD_GUID_INVALID,
         "Compiled child snapshot component GUID is invalid.",
         "\xe7\xbc\x96\xe8\xaf\x91\xe5\xad\x90\xe5\xbf\xab\xe7\x85\xa7\xe7"
         "\x9a\x84\xe7\xbb\x84\xe4\xbb\xb6\x20GUID\x20\xe9\x9d\x9e\xe6\xb3"
         "\x95\xe3\x80\x82"},
        {DAS_E_FLOW_INVOKE_CHILD_CREATE_FAILED,
         "Child task component could not be created.",
         "\xe6\x97\xa0\xe6\xb3\x95\xe5\x88\x9b\xe5\xbb\xba\xe5\xad\x90\xe4"
         "\xbb\xbb\xe5\x8a\xa1\xe7\xbb\x84\xe4\xbb\xb6\xe3\x80\x82"},
        {DAS_E_FLOW_INVOKE_RESOLVE_FAILED,
         "Repository entry resolve failed.",
         "\xe4\xbb\x93\xe5\xba\x93\xe6\x9d\xa1\xe7\x9b\xae\xe8\xa7\xa3\xe6"
         "\x9e\x90\xe5\xa4\xb1\xe8\xb4\xa5\xe3\x80\x82"},
    };

} // namespace

TEST(FlowControlErrorLensTest, FactoryReturnsValidPointer)
{
    auto lens = CreateRegisteredFlowControlErrorLens();
    ASSERT_TRUE(lens);
}

TEST(FlowControlErrorLensTest, CoversAllRegisteredCodesInBothLocales)
{
    auto lens = MakeRegisteredLens();
    ASSERT_TRUE(lens);

    for (const auto& entry : ERROR_ENTRIES)
    {
        EXPECT_NE(
            GetUtf8Message(lens.Get(), "en", entry.code),
            std::string{})
            << "en message missing for error code " << entry.code;
        EXPECT_NE(
            GetUtf8Message(lens.Get(), "zh-cn", entry.code),
            std::string{})
            << "zh-cn message missing for error code " << entry.code;
    }
}

TEST(FlowControlErrorLensTest, EnMessagesMatchI18nSourceTexts)
{
    auto lens = MakeRegisteredLens();
    ASSERT_TRUE(lens);

    for (const auto& entry : ERROR_ENTRIES)
    {
        EXPECT_EQ(GetUtf8Message(lens.Get(), "en", entry.code), entry.en_text)
            << "en message mismatch for error code " << entry.code;
    }
}

TEST(FlowControlErrorLensTest, ZhCnMessagesMatchI18nSourceTexts)
{
    auto lens = MakeRegisteredLens();
    ASSERT_TRUE(lens);

    for (const auto& entry : ERROR_ENTRIES)
    {
        EXPECT_EQ(
            GetUtf8Message(lens.Get(), "zh-cn", entry.code),
            entry.zh_cn_text)
            << "zh-cn message mismatch for error code " << entry.code;
    }
}

TEST(FlowControlErrorLensTest, ReturnsErrorOnUnknownCode)
{
    auto lens = MakeRegisteredLens();
    ASSERT_TRUE(lens);

    const DasReadOnlyString    locale{"en"};
    DasPtr<IDasReadOnlyString> out;
    const auto                 hr =
        lens->GetErrorMessage(locale.Get(), DAS_E_FAIL, out.Put());
    EXPECT_EQ(hr, DAS_E_OUT_OF_RANGE);
}
