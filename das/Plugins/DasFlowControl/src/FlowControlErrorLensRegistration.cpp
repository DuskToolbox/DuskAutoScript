#include "FlowControlErrorLensRegistration.h"

#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Plugins/DasFlowControl/FlowControlErrorCodes.h>

#include <spdlog/spdlog.h>

DAS_NS_BEGIN

namespace Plugins::DasFlowControl
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
                    "FlowControl ErrorLens: failed to register error_code={}, "
                    "locale='{}', result={}",
                    error_code,
                    locale,
                    result);
            }
        }

    } // namespace

    DasPtr<PluginInterface::IDasBasicErrorLens>
    CreateRegisteredFlowControlErrorLens()
    {
        DasPtr<PluginInterface::IDasBasicErrorLens> lens;
        const auto hr = CreateIDasBasicErrorLens(lens.Put());
        if (DAS::IsFailed(hr))
        {
            SPDLOG_ERROR(
                "FlowControl ErrorLens: CreateIDasBasicErrorLens failed, "
                "result={}",
                hr);
            return nullptr;
        }

        // launch 四码文案与 i18n 表 flow.diagnostic.*（en.json/zh-cn.json）
        // 逐字一致：lens 说明文本与端口 diagnostic 载荷是同一文本的两个出口。

        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_FLOW_LAUNCH_EXE_PATH_MISSING,
            "No executable path is configured for the launch component.");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_FLOW_LAUNCH_EXE_PATH_MISSING,
            "启动组件未配置可执行文件路径。");

        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_FLOW_LAUNCH_SPAWN_FAILED,
            "Failed to start the configured executable.");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_FLOW_LAUNCH_SPAWN_FAILED,
            "启动配置的可执行文件失败。");

        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_FLOW_LAUNCH_WINDOW_TIMEOUT,
            "Timed out waiting for the target window to appear.");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_FLOW_LAUNCH_WINDOW_TIMEOUT,
            "等待目标窗口出现超时。");

        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_FLOW_LAUNCH_WINDOW_AMBIGUOUS,
            "Multiple windows matched the wait configuration; refine the "
            "match pattern.");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_FLOW_LAUNCH_WINDOW_AMBIGUOUS,
            "多个窗口匹配等待配置，请收窄匹配条件。");

        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_FLOW_INVOKE_SNAPSHOT_MISSING,
            "Repository invoke requires a compiled child snapshot.");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_FLOW_INVOKE_SNAPSHOT_MISSING,
            "调用仓库组件需要编译子快照。");

        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_FLOW_INVOKE_SNAPSHOT_INVALID,
            "Compiled child snapshot JSON is not valid.");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_FLOW_INVOKE_SNAPSHOT_INVALID,
            "编译子快照 JSON 无效。");

        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_FLOW_INVOKE_SNAPSHOT_VERSION_UNSUPPORTED,
            "Compiled child snapshot version is not supported.");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_FLOW_INVOKE_SNAPSHOT_VERSION_UNSUPPORTED,
            "编译子快照版本不受支持。");

        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_FLOW_INVOKE_HOST_UNAVAILABLE,
            "Task component host is unavailable.");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_FLOW_INVOKE_HOST_UNAVAILABLE,
            "任务组件宿主不可用。");

        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_FLOW_INVOKE_CHILD_GUID_INVALID,
            "Compiled child snapshot component GUID is invalid.");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_FLOW_INVOKE_CHILD_GUID_INVALID,
            "编译子快照的组件 GUID 非法。");

        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_FLOW_INVOKE_CHILD_CREATE_FAILED,
            "Child task component could not be created.");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_FLOW_INVOKE_CHILD_CREATE_FAILED,
            "无法创建子任务组件。");

        RegisterOneMessage(
            lens.Get(),
            "en",
            DAS_E_FLOW_INVOKE_RESOLVE_FAILED,
            "Repository entry resolve failed.");
        RegisterOneMessage(
            lens.Get(),
            "zh-cn",
            DAS_E_FLOW_INVOKE_RESOLVE_FAILED,
            "仓库条目解析失败。");

        return lens;
    }

} // namespace Plugins::DasFlowControl

DAS_NS_END
