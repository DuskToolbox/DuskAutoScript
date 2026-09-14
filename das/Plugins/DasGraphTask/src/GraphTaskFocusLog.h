#ifndef DAS_PLUGINS_DASGRAPHTASK_GRAPHTASKFOCUSLOG_H
#define DAS_PLUGINS_DASGRAPHTASK_GRAPHTASKFOCUSLOG_H

#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/header/IDasLogRecorder.generated.h>

#include <cstdint>
#include <filesystem>

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

    // 生产入口：主进程 das.focus-log-recorder 具名服务优先（失败降级本地
    // 工厂）+ 插件包内 i18n/ 目录（DLL 自定位，cwd != 插件目录）。
    void PublishGraphTaskFinishedFocusEntry(std::int64_t end_value);

} // namespace Das::Plugins::DasGraphTask

#endif // DAS_PLUGINS_DASGRAPHTASK_GRAPHTASKFOCUSLOG_H
