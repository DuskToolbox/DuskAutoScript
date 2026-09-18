#ifndef DAS_PLUGINS_DASGRAPHTASK_GRAPHTASKFOCUSLOG_H
#define DAS_PLUGINS_DASGRAPHTASK_GRAPHTASKFOCUSLOG_H

#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/header/IDasLogRecorder.generated.h>

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace Das::Plugins::DasGraphTask
{

    // GraphTask 完成事件 focus 记录（user.jsonl → UI 事件日志面板）：end.值
    // 经插件 i18n 模板渲染（{N} 位置占位符，Translate 按 GlobalLocale
    // 解析），表缺失/解析失败回落 en 字面模板——记录失败只落调试日志，绝不
    // 影响任务返回码。测试注入面：显式 recorder + i18n 目录。
    DasResult RecordGraphTaskFinishedFocusEntry(
        Das::ExportInterface::IDasProcessLogRecorder* recorder,
        const std::filesystem::path&                  i18n_directory,
        std::int64_t                                  end_value);

    // 生产入口：主进程 das.focus-log-recorder 具名服务；resolve 失败落调
    // 试日志（含 end value），不降级 + 插件包内 i18n/ 目录（DLL 自定位，
    // cwd != 插件目录）。
    void PublishGraphTaskFinishedFocusEntry(std::int64_t end_value);

    // GraphTask 失败事件 focus 记录（镜像 Finished 形态）：失败消息经插件
    // i18n 模板渲染，表缺失/解析失败回落 en 字面模板；条目类型 ERROR。
    // 记录失败只落调试日志，绝不影响任务返回码。测试注入面：显式
    // recorder + i18n 目录。
    DasResult RecordGraphTaskFailedFocusEntry(
        Das::ExportInterface::IDasProcessLogRecorder* recorder,
        const std::filesystem::path&                  i18n_directory,
        std::string_view                              failure_message);

    // 生产入口：主进程 das.focus-log-recorder 具名服务；resolve 失败落调
    // 试日志（含失败消息），不降级 + 插件包内 i18n/ 目录（DLL 自定位）。
    void PublishGraphTaskFailedFocusEntry(std::string_view failure_message);

} // namespace Das::Plugins::DasGraphTask

#endif // DAS_PLUGINS_DASGRAPHTASK_GRAPHTASKFOCUSLOG_H
