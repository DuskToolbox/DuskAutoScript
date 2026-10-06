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
        const DasReadOnlyString    locale_str{locale};
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
        {DAS_E_MAAPI_PI_MISSING, "PI file not found", "PI文件缺失"},
        {DAS_E_MAAPI_PI_PARSE_FAILED,
         "PI parse/catalog load failed",
         "PI解析/目录加载失败"},
        {DAS_E_MAAPI_TASK_MISSING,
         "Specified task not found in PI",
         "指定task在PI中不存在"},
        {DAS_E_MAAPI_OPTION_PARSE_FAILED,
         "Option parse failed during compile",
         "编译时option解析失败"},
        {DAS_E_MAAPI_EXECUTION_FAILED,
         "MaaFramework execution failed",
         "MaaFramework执行失败"},
        {DAS_E_MAAPI_PRETASK_FAILED,
         "Pretask failed to launch or exited with a nonzero status",
         "启动前任务启动失败或非零退出"},
        {DAS_E_MAAPI_UPDATE_CHECK_FAILED,
         "Update version check failed",
         "更新版本检查失败"},
        {DAS_E_MAAPI_UPDATE_APPLY_FAILED,
         "Update apply failed (download / extract / replace error)",
         "更新应用失败（下载/解压/替换错误）"},
        {DAS_E_MAAPI_PROXY_UNSUPPORTED,
         "Proxy scheme unsupported (only http:// is supported)",
         "代理方案不支持（仅支持 http://）"},
        {DAS_E_MAAPI_ADB_DEVICE_MISSING,
         "Adb controller has no usable device (no selection, or pinned "
         "address absent from the runtime Toolkit scan)",
         "Adb 控制器无可用设备（未选择设备，或固定地址不在运行时 Toolkit 扫"
         "描结果中）"},
        {DAS_E_MAAPI_EXECUTION_INPUT_MISSING,
         "executionInput port missing (envelope must be fed via the "
         "execution-time compile chain)",
         "executionInput端口缺失（envelope须经执行时编译链喂线）"},
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

    const DasReadOnlyString    locale{"en"};
    DasPtr<IDasReadOnlyString> out;
    auto hr = lens->GetErrorMessage(locale.Get(), DAS_E_FAIL, out.Put());
    EXPECT_EQ(hr, DAS_E_OUT_OF_RANGE);
}
