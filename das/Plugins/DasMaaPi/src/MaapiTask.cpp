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

        // 算"宿主机本地下一次 HH:MM"的 unix 时间戳。今日 slot 已过则推进到明日
        // （mktime normalize tm_mday 进位跨月/年）。mktime 读本机
        // TZ+DST，故结果 是真正的本地 HH:MM 对应的 unix。
        std::time_t NextDailySlotUnix(std::time_t now, int hour, int minute)
        {
            std::tm local{};
            if (PortableLocaltime(&now, &local) == nullptr)
            {
                return static_cast<std::time_t>(-1);
            }
            local.tm_hour = hour;
            local.tm_min = minute;
            local.tm_sec = 0;
            local.tm_isdst = -1;
            std::time_t slot = std::mktime(&local);
            if (slot <= now)
            {
                local.tm_mday += 1;
                local.tm_isdst = -1;
                slot = std::mktime(&local);
            }
            return slot;
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
            return parsed.result;
        }
        // Do-cache seam：GetNextExecutionTime 签名无 settings 参数，唯一接缝是
        // Do 解析 envelope 后把 schedule 缓存到实例成员（Core 在 Do 后立刻调
        // GetNextExecutionTime，缓存必然先热）。
        schedule_ = parsed.envelope.maapi.schedule;

        auto result = MaaRuntime::Run(
            parsed.envelope,
            MaaApiBoundaryForRuntime(),
            stop_token);
        return result.das_result;
    }

    DasResult MaapiTask::GetNextExecutionTime(
        ExportInterface::DasDate* p_out_date)
    {
        if (!p_out_date)
        {
            return DAS_E_INVALID_POINTER;
        }

        // off / 未知 kind → 失败，Core sentinel 兜底（既有契约保持）。
        if (schedule_.kind != "daily")
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        int hour = 0;
        int minute = 0;
        if (!ParseDailyTime(schedule_.daily_time, hour, minute))
        {
            // 十进制 DasResult（非十六进制），便于日志阅读。
            spdlog::warn(
                "[DasMaaPi] MaapiTask::GetNextExecutionTime: invalid dailyTime "
                "\"{}\", result={}",
                schedule_.daily_time,
                DAS_E_INVALID_ARGUMENT);
            return DAS_E_INVALID_ARGUMENT;
        }

        const std::time_t now = now_();
        const std::time_t target_unix = NextDailySlotUnix(now, hour, minute);
        if (target_unix == static_cast<std::time_t>(-1))
        {
            return DAS_E_INVALID_ARGUMENT;
        }

        FillDasDateFromUnix(target_unix, p_out_date);
        return DAS_S_OK;
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END
