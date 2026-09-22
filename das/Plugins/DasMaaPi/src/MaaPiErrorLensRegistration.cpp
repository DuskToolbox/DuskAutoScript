#include "MaaPiErrorLensRegistration.h"

#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Plugins/DasMaaPi/MaaPiErrorCodes.h>

#include <spdlog/spdlog.h>

DAS_NS_BEGIN

namespace Plugins::DasMaaPi
{
    namespace
    {

        void RegisterOneMessage(
            PluginInterface::IDasBasicErrorLens* lens,
            const char*                          locale,
            DasResult                            error_code,
            const char*                          message)
        {
            const DasReadOnlyString locale_str{locale};
            const DasReadOnlyString msg_str{message};
            const auto              result = lens->RegisterErrorMessage(
                locale_str.Get(),
                error_code,
                msg_str.Get());
            if (DAS::IsFailed(result))
            {
                // 记录注册失败的错误码，但继续注册剩余消息
                SPDLOG_ERROR(
                    "MaaPi ErrorLens: failed to register error_code={}, "
                    "locale='{}', result={}",
                    error_code,
                    locale,
                    result);
            }
        }

    } // namespace

    DasPtr<PluginInterface::IDasBasicErrorLens> CreateRegisteredMaapiErrorLens()
    {
        DasPtr<PluginInterface::IDasBasicErrorLens> lens;
        const auto hr = CreateIDasBasicErrorLens(lens.Put());
        if (DAS::IsFailed(hr))
        {
            SPDLOG_ERROR(
                "MaaPi ErrorLens: CreateIDasBasicErrorLens failed, result={}",
                hr);
            return nullptr;
        }

        // DAS_E_MAAPI_PI_MISSING (-10001): PI文件缺失
        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_MAAPI_PI_MISSING,
            "PI file not found");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_MAAPI_PI_MISSING,
            "PI\xe6\x96\x87\xe4\xbb\xb6\xe7\xbc\xba\xe5\xa4\xb1");

        // DAS_E_MAAPI_PI_PARSE_FAILED (-10002): PI解析/目录加载失败
        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_MAAPI_PI_PARSE_FAILED,
            "PI parse/catalog load failed");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_MAAPI_PI_PARSE_FAILED,
            "PI\xe8\xa7\xa3\xe6\x9e\x90/\xe7\x9b\xae\xe5\xbd\x95\xe5\x8a\xa0\xe8\xbd\xbd\xe5\xa4\xb1\xe8\xb4\xa5");

        // DAS_E_MAAPI_TASK_MISSING (-10003): 指定task在PI中不存在
        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_MAAPI_TASK_MISSING,
            "Specified task not found in PI");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_MAAPI_TASK_MISSING,
            "\xe6\x8c\x87\xe5\xae\x9Atask\xe5\x9c\xa8PI\xe4\xb8\xad\xe4\xb8\x8d\xe5\xad\x98\xe5\x9c\xa8");

        // DAS_E_MAAPI_OPTION_PARSE_FAILED (-10004): 编译时option解析失败
        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_MAAPI_OPTION_PARSE_FAILED,
            "Option parse failed during compile");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_MAAPI_OPTION_PARSE_FAILED,
            "\xe7\xbc\x96\xe8\xaf\x91\xe6\x97\xb6option\xe8\xa7\xa3\xe6\x9e\x90\xe5\xa4\xb1\xe8\xb4\xa5");

        // DAS_E_MAAPI_EXECUTION_FAILED (-10005): MaaFramework执行失败
        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_MAAPI_EXECUTION_FAILED,
            "MaaFramework execution failed");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_MAAPI_EXECUTION_FAILED,
            "MaaFramework\xe6\x89\xa7\xe8\xa1\x8c\xe5\xa4\xb1\xe8\xb4\xa5");

        // DAS_E_MAAPI_PRETASK_FAILED (-10006): 启动前任务启动失败或非零退出
        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_MAAPI_PRETASK_FAILED,
            "Pretask failed to launch or exited with a nonzero status");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_MAAPI_PRETASK_FAILED,
            "\xe5\x90\xaf\xe5\x8a\xa8\xe5\x89\x8d\xe4\xbb\xbb\xe5\x8a\xa1\xe5\x90\xaf\xe5\x8a\xa8\xe5\xa4\xb1\xe8\xb4\xa5\xe6\x88\x96\xe9\x9d\x9e\xe9\x9b\xb6\xe9\x80\x80\xe5\x87\xba");

        // DAS_E_MAAPI_UPDATE_CHECK_FAILED (-10007): 更新版本检查失败
        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_MAAPI_UPDATE_CHECK_FAILED,
            "Update version check failed");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_MAAPI_UPDATE_CHECK_FAILED,
            "\xe6\x9b\xb4\xe6\x96\xb0\xe7\x89\x88\xe6\x9c\xac\xe6\xa3\x80\xe6\x9f\xa5\xe5\xa4\xb1\xe8\xb4\xa5");

        // DAS_E_MAAPI_UPDATE_APPLY_FAILED (-10008): 更新应用失败
        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_MAAPI_UPDATE_APPLY_FAILED,
            "Update apply failed (download / extract / replace error)");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_MAAPI_UPDATE_APPLY_FAILED,
            "\xe6\x9b\xb4\xe6\x96\xb0\xe5\xba\x94\xe7\x94\xa8\xe5\xa4\xb1\xe8\xb4\xa5\xef\xbc\x88\xe4\xb8\x8b\xe8\xbd\xbd/\xe8\xa7\xa3\xe5\x8e\x8b/\xe6\x9b\xbf\xe6\x8d\xa2\xe9\x94\x99\xe8\xaf\xaf\xef\xbc\x89");

        // DAS_E_MAAPI_PROXY_UNSUPPORTED (-10009): 代理方案不支持
        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_MAAPI_PROXY_UNSUPPORTED,
            "Proxy scheme unsupported (only http:// is supported)");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_MAAPI_PROXY_UNSUPPORTED,
            "\xe4\xbb\xa3\xe7\x90\x86\xe6\x96\xb9\xe6\xa1\x88\xe4\xb8\x8d\xe6\x94\xaf\xe6\x8c\x81\xef\xbc\x88\xe4\xbb\x85\xe6\x94\xaf\xe6\x8c\x81 http://\xef\xbc\x89");

        // DAS_E_MAAPI_EXECUTION_INPUT_MISSING (-10012): executionInput 端口
        // 缺失（envelope 须经编译期注入链喂线）
        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_MAAPI_EXECUTION_INPUT_MISSING,
            "executionInput port missing (envelope must be fed via the "
            "compile-time injection chain)");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_MAAPI_EXECUTION_INPUT_MISSING,
            "executionInput\xe7\xab\xaf\xe5\x8f\xa3\xe7\xbc\xba\xe5\xa4\xb1"
            "\xef\xbc\x88"
            "envelope"
            "\xe9\xa1\xbb\xe7\xbb\x8f\xe7\xbc\x96\xe8\xaf\x91\xe6\x9c\x9f\xe6\xb3\xa8"
            "\xe5\x85\xa5\xe9\x93\xbe\xe5\x96\x82\xe7\xba\xbf\xef\xbc\x89");

        return lens;
    }

} // namespace Plugins::DasMaaPi

DAS_NS_END
