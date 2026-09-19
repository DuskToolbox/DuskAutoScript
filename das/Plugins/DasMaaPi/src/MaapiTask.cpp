#include "MaapiTask.h"
#include "MaapiExecutionKernel.h"
#include "MaapiScheduleBrain.h"
#include "PluginUtils.h"

#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Plugins/DasMaaPi/PiParser.h>

#include <ctime>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    namespace
    {
        // 跨平台 gmtime_s(Win32, tm*, const time_t*) / gmtime_r(POSIX)。照 Core
        // 的 TimegmPortable #ifdef _WIN32 范式（SchedulerService.cpp）。
        std::tm* PortableGmtime(const std::time_t* timer, std::tm* buf)
        {
#ifdef _WIN32
            return gmtime_s(buf, timer) == 0 ? buf : nullptr;
#else
            return gmtime_r(timer, buf);
#endif
        }

        std::tm* PortableLocaltime(const std::time_t* timer, std::tm* buf)
        {
#ifdef _WIN32
            return localtime_s(buf, timer) == 0 ? buf : nullptr;
#else
            return localtime_r(timer, buf);
#endif
        }

        // 把 unix 时间戳拆成 UTC calendar 字段填 DasDate。Core 的 DasDateToUnix
        // 用 timegm/_mkgmtime 把 DasDate 当 UTC 解释，故此处必须经 gmtime
        // 反向填 UTC 字段，使 DasDateToUnix(timegm(UTC fields)) round-trip 回原
        // unix。
        void FillDasDateFromUnix(
            std::time_t               unix_time,
            ExportInterface::DasDate* out)
        {
            std::tm utc{};
            if (PortableGmtime(&unix_time, &utc) == nullptr)
            {
                return;
            }
            out->year = static_cast<std::uint16_t>(utc.tm_year + 1900);
            out->month = static_cast<std::uint8_t>(utc.tm_mon + 1);
            out->day = static_cast<std::uint8_t>(utc.tm_mday);
            out->hour = static_cast<std::uint8_t>(utc.tm_hour);
            out->minute = static_cast<std::uint8_t>(utc.tm_min);
            out->second = static_cast<std::uint8_t>(utc.tm_sec);
        }

        // 把 local tm 编码成 year*10000+mon*100+day 的 ymd
        // int（用于跨日去重）。
        int EncodeYmd(const std::tm& local)
        {
            return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100
                   + local.tm_mday;
        }
    } // namespace

    DasResult MaapiTask::Do(
        PluginInterface::IDasStopToken* stop_token,
        ExportInterface::IDasJson*,
        ExportInterface::IDasJson* p_task_settings_json)
    {
        if (!p_task_settings_json)
        {
            return DAS_E_INVALID_POINTER;
        }

        auto request = ReadJson(p_task_settings_json);
        if (!request || !request->is_object())
        {
            return DAS_E_INVALID_JSON;
        }

        // 今日 slot 已决策标记：Do 执行过（无论成败）即视为今日已消费。
        // 不标记则执行后 GetNextExecutionTime 的 daily catch-up 分支看到
        // 「今日未 fired 且 slot 已过」会返回 now（立即到期）再跑一次——
        // 手动立即执行与调度执行都会双跑。catch-up 本义（进程停机错过
        // slot、启动后由过期 next_execution_time 触发补跑）不受影响。
        // schedule 语义留在 Task 侧，不进共享执行内核。
        {
            const std::time_t now = now_();
            std::tm           local_now{};
            if (PortableLocaltime(&now, &local_now) != nullptr)
            {
                last_fired_ymd_ = EncodeYmd(local_now);
            }
        }

        MaaRuntimeResult result;
        const auto       execute_result = ExecuteEnvelope(
            *request,
            MaaApiBoundaryForRuntime(),
            stop_token,
            {},
            {},
            result);
        return execute_result;
    }

    DasResult MaapiTask::GetNextExecutionTime(
        ExportInterface::IDasJson* p_task_settings_json,
        ExportInterface::DasDate*  p_out_date)
    {
        if (!p_out_date)
        {
            return DAS_E_INVALID_POINTER;
        }
        if (!p_task_settings_json)
        {
            return DAS_E_INVALID_POINTER;
        }

        // GetNextExecutionTime 独立于 Do：直接从 task_settings_json 解析
        // envelope 取 schedule（与 Do 同款解析路径）。schedule 不再缓存到实例
        // 成员，schedule 变更实时反映到下次调用。
        auto request = ReadJson(p_task_settings_json);
        if (!request || !request->is_object())
        {
            return DAS_E_INVALID_JSON;
        }
        auto parsed = ParseExecutionEnvelope(*request);
        if (DAS::IsFailed(parsed.result))
        {
            return parsed.result;
        }

        // 共享 schedule brain 助手（101 行为零变化抽取；与
        // MaapiRunTaskComponent::Do 的 nextTime 口同一计算——D-B5 插件自治，
        // 值同源）。catch-up 状态机的 last_fired_ymd 经出入参持久化到本实例。
        const auto next = ComputeNextScheduleRun(
            parsed.envelope.maapi.schedule,
            now_(),
            last_fired_ymd_);
        last_fired_ymd_ = next.last_fired_ymd;
        if (next.status == ScheduleNextStatus::Off)
        {
            // off / 未知 kind → 失败，Core sentinel 兜底（既有契约保持）。
            return DAS_E_NO_IMPLEMENTATION;
        }
        if (next.status == ScheduleNextStatus::Invalid)
        {
            return DAS_E_INVALID_ARGUMENT;
        }
        FillDasDateFromUnix(next.next_unix, p_out_date);
        return DAS_S_OK;
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END
