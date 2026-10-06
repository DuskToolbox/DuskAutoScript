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
            "PI文件缺失");

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
            "PI解析/目录加载失败");

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
            "指定task在PI中不存在");

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
            "编译时option解析失败");

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
            "MaaFramework执行失败");

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
            "启动前任务启动失败或非零退出");

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
            "更新版本检查失败");

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
            "更新应用失败（下载/解压/替换错误）");

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
            "代理方案不支持（仅支持 http://）");

        // DAS_E_MAAPI_ADB_DEVICE_MISSING (-10010): Adb 控制器无可用设备
        // （未选择设备，或固定地址不在运行时 Toolkit 扫描结果中——编译
        // 装配段与运行期校验段共用该语义）
        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_MAAPI_ADB_DEVICE_MISSING,
            "Adb controller has no usable device (no selection, or pinned "
            "address absent from the runtime Toolkit scan)");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_MAAPI_ADB_DEVICE_MISSING,
            "Adb 控制器无可用设备（未选择设备，或固定地址不在运行时 Toolkit "
            "扫描结果中）");

        // DAS_E_MAAPI_EXECUTION_INPUT_MISSING (-10012): executionInput 端口
        // 缺失（envelope 须经执行时编译链喂线）
        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_MAAPI_EXECUTION_INPUT_MISSING,
            "executionInput port missing (envelope must be fed via the "
            "execution-time compile chain)");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_MAAPI_EXECUTION_INPUT_MISSING,
            "executionInput端口缺失（envelope须经执行时编译链喂线）");

        return lens;
    }

} // namespace Plugins::DasMaaPi

DAS_NS_END
