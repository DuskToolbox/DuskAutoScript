#include "MaapiScheduleBrain.h"

#include <spdlog/spdlog.h>

#include <charconv>
#include <cstdio>
#include <ctime>

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        // 跨平台 gmtime_s(Win32, tm*, const time_t*) / gmtime_r(POSIX)。与
        // MaapiTask.cpp 既有私有副本同款（111-01 公共 PortableTime.h 落 Core
        // Utils，插件侧私有副本的统一清理留后续 phase）。
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

        // 解析 daily_time "HH:MM"。成功返回 true 并写出 hour/minute；失败
        // 返回 false（格式非法由调用方按 Invalid 处理，不继续用未初始化值；
        // 范围校验 hour 0-23 / minute 0-59）。
        bool ParseDailyTime(const std::string& text, int& hour, int& minute)
        {
            const char* const text_end = text.data() + text.size();
            const auto hour_result =
                std::from_chars(text.data(), text_end, hour);
            if (
                hour_result.ec != std::errc{} || hour_result.ptr == text_end
                || *hour_result.ptr != ':')
            {
                return false;
            }
            const auto minute_result =
                std::from_chars(hour_result.ptr + 1, text_end, minute);
            if (minute_result.ec != std::errc{} || minute_result.ptr != text_end)
            {
                return false;
            }
            return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
        }

        // 把 local tm 编码成 year*10000+mon*100+day 的 ymd
        // int（用于跨日去重）。
        int EncodeYmd(const std::tm& local)
        {
            return (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100
                   + local.tm_mday;
        }
    } // namespace

    int EncodeLocalYmd(std::time_t now)
    {
        std::tm local{};
        if (PortableLocaltime(&now, &local) == nullptr)
        {
            return 0;
        }
        return EncodeYmd(local);
    }

    ScheduleNextRun ComputeNextScheduleRun(
        const MaaScheduleConfigDto& schedule,
        std::time_t                 now,
        int                         last_fired_ymd)
    {
        ScheduleNextRun run;
        run.last_fired_ymd = last_fired_ymd;

        // interval：固定 now + intervalMinutes，绝不补跑（错过几档跳到下一
        // 未来档）。intervalMinutes<=0 → Invalid，防 next-run=now 的热循环。
        if (schedule.kind == "interval")
        {
            if (schedule.interval_minutes <= 0)
            {
                // 十进制 DasResult（非十六进制），便于日志阅读。
                spdlog::warn(
                    "[DasMaaPi] ComputeNextScheduleRun: invalid "
                    "interval_minutes {}, result={}",
                    schedule.interval_minutes,
                    DAS_E_INVALID_ARGUMENT);
                run.status = ScheduleNextStatus::Invalid;
                return run;
            }
            run.status = ScheduleNextStatus::Ok;
            run.next_unix =
                now + static_cast<std::time_t>(schedule.interval_minutes) * 60;
            return run;
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
                    "[DasMaaPi] ComputeNextScheduleRun: invalid dailyTime "
                    "\"{}\", result={}",
                    schedule.daily_time,
                    DAS_E_INVALID_ARGUMENT);
                run.status = ScheduleNextStatus::Invalid;
                return run;
            }

            std::tm local_now{};
            if (PortableLocaltime(&now, &local_now) == nullptr)
            {
                run.status = ScheduleNextStatus::Invalid;
                return run;
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
            if (last_fired_ymd == today_ymd)
            {
                // 今日 slot 已决策（排过或补跑过）→ 明日 slot，防同日重复
                // 触发。
                target_unix = tomorrow_slot_unix;
            }
            else if (now >= today_slot_unix)
            {
                // 今日 slot 已过（错过）。catchUp=true 补跑一次返回 now 并
                // 标记今日已补跑（下次同日调用走上一分支返回明日 slot，防
                // 热循环）；catchUp=false 直接跳到明日 slot。
                if (schedule.daily_catch_up)
                {
                    target_unix = now;
                    run.last_fired_ymd = today_ymd;
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
                run.last_fired_ymd = today_ymd;
            }

            if (target_unix == static_cast<std::time_t>(-1))
            {
                run.status = ScheduleNextStatus::Invalid;
                return run;
            }
            run.status = ScheduleNextStatus::Ok;
            run.next_unix = target_unix;
            return run;
        }

        // off / 未知 kind → Off（Core sentinel 兜底既有契约）。
        run.status = ScheduleNextStatus::Off;
        return run;
    }

    std::string FormatNextRunIso8601Utc(std::time_t unix_time)
    {
        std::tm utc{};
        if (PortableGmtime(&unix_time, &utc) == nullptr)
        {
            return {};
        }
        // YYYY-MM-DDTHH:MM:SS：定长 19 字符（D-B5 wire 契约，111-02 解析端
        // 按同格式定长解析）。UTC 字段（PortableGmtime），禁本地字段——
        // PortableLocaltime 产物在该串上会偏移一个 UTC 偏移量。year 超
        // 4 位时 snprintf 写入长度 != 19，按失败拒绝（防变长串穿透定长
        // 契约）。
        char      text[20];
        const int written = std::snprintf(
            text,
            sizeof(text),
            "%04d-%02d-%02dT%02d:%02d:%02d",
            utc.tm_year + 1900,
            utc.tm_mon + 1,
            utc.tm_mday,
            utc.tm_hour,
            utc.tm_min,
            utc.tm_sec);
        if (written != 19)
        {
            return {};
        }
        return std::string(text);
    }
} // namespace Das::Plugins::DasMaaPi
