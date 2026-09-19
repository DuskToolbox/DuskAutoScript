#ifndef DAS_PLUGINS_DASMAAPI_MAAPI_SCHEDULE_BRAIN_H
#define DAS_PLUGINS_DASMAAPI_MAAPI_SCHEDULE_BRAIN_H

#include <das/Plugins/DasMaaPi/ExecutionEnvelope.h>

#include <ctime>
#include <string>

namespace Das::Plugins::DasMaaPi
{
    // 共享 schedule brain 的计算状态分类。
    enum class ScheduleNextStatus
    {
        // next_unix 有效。
        Ok,
        // schedule 描述非法（interval<=0 / dailyTime 格式非法 / local
        // calendar 转换失败）——MaapiTask::GetNextExecutionTime 按
        // DAS_E_INVALID_ARGUMENT 既有契约；组件侧不发 nextTime 键。
        Invalid,
        // off / 未知 kind——MaapiTask::GetNextExecutionTime 按
        // DAS_E_NO_IMPLEMENTATION（Core sentinel 兜底既有契约）；组件侧不
        // 发 nextTime 键（engaged-only 纪律）。
        Off
    };

    // 纯函数出入参（助手不带实例态）：catch-up 状态机的 last_fired_ymd 经
    // 入参/出参传递，由调用方持久化。
    struct ScheduleNextRun
    {
        ScheduleNextStatus status = ScheduleNextStatus::Off;
        std::time_t        next_unix = 0;
        // daily 状态机输出：今日 slot 已决策时更新为今日 ymd，否则原样回传
        // 入参值。MaapiTask 持久化到成员；组件侧按「本次 Do 刚执行」语义
        // 直接传今日 ymd（daily 恒推明日 slot）。
        int last_fired_ymd = 0;
    };

    // 共享 schedule 计算助手（101 brain 抽出，两消费方：
    // MaapiTask::GetNextExecutionTime 与 MaapiRunTaskComponent::Do 的
    // nextTime 口）。纯函数：schedule 描述 + now unix → 下次执行 unix。
    // 界线铁律：不进 MaapiExecutionKernel（schedule 语义留在 Task 侧），
    // 不带实例态。
    // - interval：now + intervalMinutes（<=0 → Invalid，防 next-run=now 的
    //   热循环）；绝不补跑（错过几档跳到下一未来档）。
    // - daily：宿主机本地下一次 HH:MM slot + catch-up 状态机（mktime 归一化
    //   跨月/年；last_fired_ymd == 今日 → 明日 slot，防同日双跑）。
    // - off / 未知 kind → Off。
    ScheduleNextRun ComputeNextScheduleRun(
        const MaaScheduleConfigDto& schedule,
        std::time_t                 now,
        int                         last_fired_ymd);

    // 本地 calendar 的 year*10000+mon*100+day ymd（daily 跨日去重键）。
    // localtime 失败返回 0。
    int EncodeLocalYmd(std::time_t now);

    // unix → ISO 8601 UTC YYYY-MM-DDTHH:MM:SS（定长 19 字符，D-B5 wire
    // 契约——111-01/111-02/111-03 三端同约定）。UTC 字段经 PortableGmtime
    // 反填（DasDate/DasDateToUnix wire 按 UTC 解释；本地字段串会被 111-02
    // 直填后偏移一个 UTC 偏移量，禁用）。失败返回空串。
    std::string FormatNextRunIso8601Utc(std::time_t unix_time);
} // namespace Das::Plugins::DasMaaPi

#endif // DAS_PLUGINS_DASMAAPI_MAAPI_SCHEDULE_BRAIN_H
