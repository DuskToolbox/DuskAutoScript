#include "MaapiTask.h"
#include "PluginUtils.h"

#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Plugins/DasMaaPi/PiParser.h>

#include <spdlog/spdlog.h>

#include <cstdio>
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

        // 解析 daily_time "HH:MM"。成功返回 true 并写出 hour/minute；失败返回
        // false（格式非法由调用方返回
        // DAS_E_INVALID_ARGUMENT，不继续用未初始化值 填 DasDate；范围校验 hour
        // 0-23 / minute 0-59）。
        bool ParseDailyTime(const std::string& text, int& hour, int& minute)
        {
            if (std::sscanf(text.c_str(), "%d:%d", &hour, &minute) != 2)
            {
                return false;
            }
            return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
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
        auto parsed = ParseExecutionEnvelope(*request);
        if (DAS::IsFailed(parsed.result))
        {
            spdlog::error(
                "[DasMaaPi] MaapiTask::Do: envelope parse failed, "
                "result={}, message={}",
                parsed.result,
                parsed.message);
            return parsed.result;
        }

        // 今日 slot 已决策标记：Do 执行过（无论成败）即视为今日已消费。
        // 不标记则执行后 GetNextExecutionTime 的 daily catch-up 分支看到
        // 「今日未 fired 且 slot 已过」会返回 now（立即到期）再跑一次——
        // 手动立即执行与调度执行都会双跑。catch-up 本义（进程停机错过
        // slot、启动后由过期 next_execution_time 触发补跑）不受影响。
        {
            const std::time_t now = now_();
            std::tm           local_now{};
            if (PortableLocaltime(&now, &local_now) != nullptr)
            {
                last_fired_ymd_ = EncodeYmd(local_now);
            }
        }

        auto result = MaaRuntime::Run(
            parsed.envelope,
            MaaApiBoundaryForRuntime(),
            stop_token);
        // 结果诊断外显（事件日志）：warning 级无论成败投递（sink 注册降级
        // 等在成功运行也会出现），标题与 error 级仅失败时投递。
        LogRunResult(result);
        return result.das_result;
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
        const auto& schedule = parsed.envelope.maapi.schedule;

        // interval：固定 now + intervalMinutes，绝不补跑（错过几档跳到下一未来
        // 档）。intervalMinutes<=0 返回 DAS_E_INVALID_ARGUMENT，防 next-run=now
        // 的热循环。
        if (schedule.kind == "interval")
        {
            if (schedule.interval_minutes <= 0)
            {
                // 十进制 DasResult（非十六进制），便于日志阅读。
                spdlog::warn(
                    "[DasMaaPi] MaapiTask::GetNextExecutionTime: invalid "
                    "interval_minutes {}, result={}",
                    schedule.interval_minutes,
                    DAS_E_INVALID_ARGUMENT);
                return DAS_E_INVALID_ARGUMENT;
            }
            const std::time_t target_unix =
                now_()
                + static_cast<std::time_t>(schedule.interval_minutes) * 60;
            FillDasDateFromUnix(target_unix, p_out_date);
            return DAS_S_OK;
        }

        // daily：宿主机本地下一次 HH:MM，带 catch-up 状态机。
        if (schedule.kind == "daily")
        {
            int hour = 0;
            int minute = 0;
            if (!ParseDailyTime(schedule.daily_time, hour, minute))
            {
                // 十进制 DasResult（非十六进制），便于日志阅读。
                spdlog::warn(
                    "[DasMaaPi] MaapiTask::GetNextExecutionTime: invalid "
                    "dailyTime \"{}\", result={}",
                    schedule.daily_time,
                    DAS_E_INVALID_ARGUMENT);
                return DAS_E_INVALID_ARGUMENT;
            }

            const std::time_t now = now_();
            std::tm           local_now{};
            if (PortableLocaltime(&now, &local_now) == nullptr)
            {
                return DAS_E_INVALID_ARGUMENT;
            }
            const int today_ymd = EncodeYmd(local_now);

            // 今日 slot unix（宿主机本地 HH:MM）。
            std::tm slot_tm = local_now;
            slot_tm.tm_hour = hour;
            slot_tm.tm_min = minute;
            slot_tm.tm_sec = 0;
            slot_tm.tm_isdst = -1;
            const std::time_t today_slot_unix = std::mktime(&slot_tm);
            // 明日 slot unix（tm_mday 进位后 mktime normalize 跨月/年）。
            std::tm tomorrow_tm = slot_tm;
            tomorrow_tm.tm_mday += 1;
            tomorrow_tm.tm_isdst = -1;
            const std::time_t tomorrow_slot_unix = std::mktime(&tomorrow_tm);

            std::time_t target_unix;
            if (last_fired_ymd_ == today_ymd)
            {
                // 今日 slot 已决策（排过或补跑过）→ 明日 slot，防同日重复触发。
                target_unix = tomorrow_slot_unix;
            }
            else if (now >= today_slot_unix)
            {
                // 今日 slot 已过（错过）。catchUp=true 补跑一次返回 now
                // 并标记今日 已补跑（下次同日调用走上一分支返回明日
                // slot，防热循环）； catchUp=false 直接跳到明日 slot。
                if (schedule.daily_catch_up)
                {
                    target_unix = now;
                    last_fired_ymd_ = today_ymd;
                }
                else
                {
                    target_unix = tomorrow_slot_unix;
                }
            }
            else
            {
                // 今日 slot 未到：排今日 slot，并标记今日已决策。
                target_unix = today_slot_unix;
                last_fired_ymd_ = today_ymd;
            }

            if (target_unix == static_cast<std::time_t>(-1))
            {
                return DAS_E_INVALID_ARGUMENT;
            }
            FillDasDateFromUnix(target_unix, p_out_date);
            return DAS_S_OK;
        }

        // off / 未知 kind → 失败，Core sentinel 兜底（既有契约保持）。
        return DAS_E_NO_IMPLEMENTATION;
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END
