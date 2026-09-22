#ifndef DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKADAPTER_H
#define DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKADAPTER_H

#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTask.Implements.hpp>

#include <ctime>
#include <functional>
#include <optional>
#include <string>

// Task type GUID: {F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3}
// Must equal DasGraphTask.json tasks.<key> / executionComponent.componentGuid,
// otherwise SchedulerService::Initialize cannot match task_descriptors to fill
// name="das.graph" (SchedulerService.cpp task_descriptors.find(task_guid)).
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Plugins::DasGraphTask,
    DasGraphTaskAdapter,
    0xF4A2C9D1,
    0x7B3E,
    0x4D8A,
    0xA1,
    0x5F,
    0xC2,
    0xE8,
    0x6B,
    0x4D,
    0x91,
    0xA3);

DAS_NS_BEGIN
namespace Plugins::DasGraphTask
{
    // IDasTask 薄适配器（调度器顶层执行入口）。职责是
    // 纯形状适配：把 IDasTask 的 task_settings JSON 组装为 executionInput
    // 输入口，委托同模块直接构造的 DasGraphTaskImpl 组件内核执行
    // （GraphRuntime 驱动代码全插件仅此一份）。host 经构造注入（包默认
    // 存储，加载器保证注入先于本 adapter 的创建）；无 host 时内核组件
    // 自身以结构化错误失败。
    class DasGraphTaskAdapter final
        : public PluginInterface::DasTaskImplBase<DasGraphTaskAdapter>
    {
        DasPtr<PluginInterface::IDasTaskComponentCreator> host_;

    public:
        explicit DasGraphTaskAdapter(
            PluginInterface::IDasTaskComponentCreator* p_host);

        DAS_IMPL Do(
            PluginInterface::IDasStopToken* stop_token,
            ExportInterface::IDasJson*      p_environment_json,
            ExportInterface::IDasJson*      p_task_settings_json) override;

        DAS_IMPL GetNextExecutionTime(
            ExportInterface::IDasJson* p_task_settings_json,
            ExportInterface::DasDate*  p_out_date) override;

        // fake clock seam：注入固定时钟驱动静态通道（次日/月末/超窗）单测，
        // 无需实时等待。生产默认 std::time(nullptr)。照 MaapiTask 的
        // ClockFn/SetClockForTest 范式（101-01 模式沿用）。
        using ClockFn = std::function<std::time_t()>;
        void SetClockForTest(ClockFn fn) { now_ = std::move(fn); }

    private:
        // 本次调度周期的基准时刻（「本次原计划时刻」锚，UTC 字段）。重锚定
        // 即提交（首次计算 / Do 执行后首次计算），此后无 Do 伴随的重算路径
        // （RefreshNextExecutionTimes/ComputeInitialNextExecutionTime）不推
        // 槽——同 base 同返回值（防重算推挤）。Do 入口 reset：执行后首次
        // GetNextExecutionTime 以执行完成时刻重锚定（「执行到 End 后记录」
        // 语义——end 算出的下次时间从本次执行起算）。
        // 进程内自持、不持久化；进程重启后基准回落 now_() 重锚定
        // （RESEARCH Open Questions Q2 RESOLVED 处置）。
        std::optional<ExportInterface::DasDate> last_planned_;
        // Do 成功后从 end「下次时间」口捕获的 ISO 8601 UTC 串（动态通道优
        // 先消费）。每次 Do 入口清空重捕获，陈旧值不跨执行残留；口缺省/
        // 读取失败 = 无动态值（回落静态通道）。
        std::optional<std::string> captured_next_time_;
        ClockFn                    now_ = [] { return std::time(nullptr); };
    };
} // namespace Plugins::DasGraphTask
DAS_NS_END

#endif // DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKADAPTER_H
