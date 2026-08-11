#ifndef DAS_PLUGINS_DASMAAPI_MAAPITASK_H
#define DAS_PLUGINS_DASMAAPI_MAAPITASK_H

#include <das/Plugins/DasMaaPi/ExecutionEnvelope.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTask.Implements.hpp>

#include <ctime>
#include <functional>

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Plugins::DasMaaPi,
    MaapiTask,
    0x69f20001,
    0x0000,
    0x4000,
    0x80,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x01);

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    class MaapiTask final : public PluginInterface::DasTaskImplBase<MaapiTask>
    {
    public:
        DAS_IMPL Do(
            PluginInterface::IDasStopToken* stop_token,
            ExportInterface::IDasJson*      p_environment_json,
            ExportInterface::IDasJson*      p_task_settings_json) override;

        DAS_IMPL GetNextExecutionTime(
            ExportInterface::IDasJson* p_task_settings_json,
            ExportInterface::DasDate*  p_out_date) override;

        // fake clock seam：注入固定时钟驱动 daily 跨午夜 / off 单测，无需实时
        // 等待。生产默认 std::time(nullptr)。照既有 DasMaaPi fake/seam 范式
        // （DefaultAdbDeviceEnumerator::SetForTest）。
        using ClockFn = std::function<std::time_t()>;
        void SetClockForTest(ClockFn fn) { now_ = std::move(fn); }

    private:
        // daily 跨日去重：标记今日 slot
        // 已决策（排过或补跑过），GetNextExecutionTime 同日再次调用直接返回明日
        // slot，防 catch-up 热循环与调度后同日双跑。
        // 跨自然日自动失效（today_ymd != last_fired_ymd_）。
        int     last_fired_ymd_ = 0;
        ClockFn now_ = [] { return std::time(nullptr); };
    };
} // namespace Plugins::DasMaaPi
DAS_NS_END

#endif // DAS_PLUGINS_DASMAAPI_MAAPITASK_H
