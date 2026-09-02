#pragma once

#include <das/_autogen/idl/abi/DasResult.h>

#include <cstddef>
#include <iterator>

namespace Das::Plugins::DasFlowControl
{
    // FlowControl 执行失败错误码（-10100 系列）。定义为自动生成枚举区间
    // [-1073750000, -1073800004] 之外的 constexpr DasResult；段位约定：
    // MaaPi 占 -100xx，FlowControl 占 -101xx，后续插件扩段先核对段位
    // （跨插件撞段会导致 ErrorLens 误路由）。
    //
    // 两层失败语义（执行失败 = 错误码；业务结果 = status 端口）：
    // - launch 全部失败场景是「无法完成启动动作」→ 错误码；
    // - invokeRepository 的快照/子组件创建失败是「无法完成调用动作」→
    //   错误码；子任务自身成败是业务结果 → status 端口（不走这些码）。

    /// 启动组件未配置可执行文件路径（对应 flow.diagnostic.
    /// launch-exe-path-missing）。
    inline constexpr DasResult DAS_E_FLOW_LAUNCH_EXE_PATH_MISSING =
        static_cast<DasResult>(-10101);

    /// 启动配置的可执行文件失败（launch-spawn-failed）。
    inline constexpr DasResult DAS_E_FLOW_LAUNCH_SPAWN_FAILED =
        static_cast<DasResult>(-10102);

    /// 等待目标窗口出现超时（launch-window-timeout）。
    inline constexpr DasResult DAS_E_FLOW_LAUNCH_WINDOW_TIMEOUT =
        static_cast<DasResult>(-10103);

    /// 多个窗口匹配等待配置（launch-window-ambiguous）。
    inline constexpr DasResult DAS_E_FLOW_LAUNCH_WINDOW_AMBIGUOUS =
        static_cast<DasResult>(-10104);

    /// invokeRepository 输入缺 compiledSnapshot 端口。
    inline constexpr DasResult DAS_E_FLOW_INVOKE_SNAPSHOT_MISSING =
        static_cast<DasResult>(-10105);

    /// compiledSnapshot JSON 解析/反序列化失败。
    inline constexpr DasResult DAS_E_FLOW_INVOKE_SNAPSHOT_INVALID =
        static_cast<DasResult>(-10106);

    /// compiledSnapshot 版本不受支持。
    inline constexpr DasResult DAS_E_FLOW_INVOKE_SNAPSHOT_VERSION_UNSUPPORTED =
        static_cast<DasResult>(-10107);

    /// 任务组件宿主不可用（无法创建子组件的宿主环境）。
    inline constexpr DasResult DAS_E_FLOW_INVOKE_HOST_UNAVAILABLE =
        static_cast<DasResult>(-10108);

    /// 编译快照的组件 GUID 非法。
    inline constexpr DasResult DAS_E_FLOW_INVOKE_CHILD_GUID_INVALID =
        static_cast<DasResult>(-10109);

    /// 子任务组件创建失败（宿主未注册该组件或工厂拒绝）。
    inline constexpr DasResult DAS_E_FLOW_INVOKE_CHILD_CREATE_FAILED =
        static_cast<DasResult>(-10110);

    /// 仓库条目解析失败（settings fallback 自解析仓库条目，预留）。
    inline constexpr DasResult DAS_E_FLOW_INVOKE_RESOLVE_FAILED =
        static_cast<DasResult>(-10111);

    // 段位与唯一性在编译期锁定：任一码漂出 -101xx 段或互相撞值即编译错。
    constexpr DasResult ALL_FLOW_ERROR_CODES[] = {
        DAS_E_FLOW_LAUNCH_EXE_PATH_MISSING,
        DAS_E_FLOW_LAUNCH_SPAWN_FAILED,
        DAS_E_FLOW_LAUNCH_WINDOW_TIMEOUT,
        DAS_E_FLOW_LAUNCH_WINDOW_AMBIGUOUS,
        DAS_E_FLOW_INVOKE_SNAPSHOT_MISSING,
        DAS_E_FLOW_INVOKE_SNAPSHOT_INVALID,
        DAS_E_FLOW_INVOKE_SNAPSHOT_VERSION_UNSUPPORTED,
        DAS_E_FLOW_INVOKE_HOST_UNAVAILABLE,
        DAS_E_FLOW_INVOKE_CHILD_GUID_INVALID,
        DAS_E_FLOW_INVOKE_CHILD_CREATE_FAILED,
        DAS_E_FLOW_INVOKE_RESOLVE_FAILED};

    constexpr bool FlowErrorCodeWithinBand(DasResult error_code)
    {
        return error_code <= static_cast<DasResult>(-10100)
               && error_code >= static_cast<DasResult>(-10199);
    }

    constexpr bool AllFlowErrorCodesDistinct()
    {
        for (size_t i = 0; i < std::size(ALL_FLOW_ERROR_CODES); ++i)
        {
            for (size_t j = i + 1; j < std::size(ALL_FLOW_ERROR_CODES); ++j)
            {
                if (ALL_FLOW_ERROR_CODES[i] == ALL_FLOW_ERROR_CODES[j])
                {
                    return false;
                }
            }
        }
        return true;
    }

    constexpr bool AllFlowErrorCodesWithinBand()
    {
        for (const auto error_code : ALL_FLOW_ERROR_CODES)
        {
            if (!FlowErrorCodeWithinBand(error_code))
            {
                return false;
            }
        }
        return true;
    }

    static_assert(
        AllFlowErrorCodesWithinBand(),
        "FlowControl error codes must stay inside the -101xx band");
    static_assert(
        AllFlowErrorCodesDistinct(),
        "FlowControl error codes must be pairwise distinct");

} // namespace Das::Plugins::DasFlowControl
