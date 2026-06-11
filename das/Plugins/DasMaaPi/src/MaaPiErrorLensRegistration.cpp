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
            DasReadOnlyString locale_str{locale};
            DasReadOnlyString msg_str{message};
            const auto        result = lens->RegisterErrorMessage(
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

        return lens;
    }

} // namespace Plugins::DasMaaPi

DAS_NS_END
