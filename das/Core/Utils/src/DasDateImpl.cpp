#include <das/Core/Logger/Logger.h>
#include <das/Core/Utils/PortableTime.h>
#include <das/DasApi.h>
#include <das/DasConfig.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/abi/DasBasicTypes.h>

#include <ctime>

DAS_NS_ANONYMOUS_DETAILS_BEGIN

// 日历字段谓词（基线 / Compare 两侧入参）：字段处于名义日历域——month 1-12、
// day 1-31（day 31 落在短月属归一化职责，不是非法输入）、hour 0-23、
// minute 0-59、second 0-59。不夹紧、拒绝非法（ParseHHMM 纪律同款）。
bool IsCalendarDateFieldsValid(const Das::ExportInterface::DasDate& date)
{
    if (date.month < 1 || date.month > 12)
    {
        return false;
    }
    if (date.day < 1 || date.day > 31)
    {
        return false;
    }
    if (date.hour > 23 || date.minute > 59 || date.second > 59)
    {
        return false;
    }
    return true;
}

// 间隔字段谓词（DasDateAdd 的 delta
// 入参）：间隔装字段（D-B6「+1天={day:1}」）， 各字段允许 0（零偏移——{year:1}
// 隐含 month=0/day=0 合法）；上界沿用同字段 名义域（month ≤ 12、day ≤
// 31），越界拒绝。
bool IsIntervalFieldsValid(const Das::ExportInterface::DasDate& delta)
{
    if (delta.month > 12)
    {
        return false;
    }
    if (delta.day > 31)
    {
        return false;
    }
    if (delta.hour > 23 || delta.minute > 59 || delta.second > 59)
    {
        return false;
    }
    return true;
}

// DasDate（UTC 字段）→ unix。字段先经谓词校验；timegm 链失败（如 year 0 落在
// time_t 可表示域外）按非法输入拒绝。
bool DasDateToUnixTime(
    const Das::ExportInterface::DasDate& date,
    std::time_t&                         out_unix)
{
    std::tm tm{};
    tm.tm_year = date.year - 1900;
    tm.tm_mon = date.month - 1;
    tm.tm_mday = date.day;
    tm.tm_hour = date.hour;
    tm.tm_min = date.minute;
    tm.tm_sec = date.second;
    tm.tm_isdst = 0;
    const std::time_t unix_time = DAS::Core::Utils::TimegmPortable(&tm);
    if (unix_time == static_cast<std::time_t>(-1))
    {
        return false;
    }
    out_unix = unix_time;
    return true;
}

// unix → DasDate（UTC 字段，gmtime 反填——与 DasDateToUnixTime 的 timegm 链
// 精确互逆）。归一化结果 year 超出 wire uint16 可表示域时拒绝（防静默截断）。
bool UnixTimeToDasDate(
    std::time_t                    unix_time,
    Das::ExportInterface::DasDate& out_date)
{
    std::tm utc{};
    if (DAS::Core::Utils::PortableGmtime(&unix_time, &utc) == nullptr)
    {
        return false;
    }
    const int normalized_year = utc.tm_year + 1900;
    if (normalized_year < 0 || normalized_year > 65535)
    {
        return false;
    }
    out_date.year = static_cast<std::uint16_t>(normalized_year);
    out_date.month = static_cast<std::uint8_t>(utc.tm_mon + 1);
    out_date.day = static_cast<std::uint8_t>(utc.tm_mday);
    out_date.hour = static_cast<std::uint8_t>(utc.tm_hour);
    out_date.minute = static_cast<std::uint8_t>(utc.tm_min);
    out_date.second = static_cast<std::uint8_t>(utc.tm_sec);
    return true;
}

DAS_NS_ANONYMOUS_DETAILS_END

// ========================================================================
// C API — DasDateAdd / DasDateCompare（D-B6：tm 风格字段相加 + timegm 链
// 归一化进位，DasDate wire 按 UTC 字段解释——111-PATTERNS 共享模式 2）
// ========================================================================

DAS_C_API DasResult DasDateAdd(
    const Das::ExportInterface::DasDate* p_base,
    const Das::ExportInterface::DasDate* p_delta,
    Das::ExportInterface::DasDate*       p_out_result)
{
    DAS_UTILS_CHECK_POINTER(p_base)
    DAS_UTILS_CHECK_POINTER(p_delta)
    DAS_UTILS_CHECK_POINTER(p_out_result)

    try
    {
        if (!Details::IsCalendarDateFieldsValid(*p_base))
        {
            return DAS_E_INVALID_ARGUMENT;
        }
        if (!Details::IsIntervalFieldsValid(*p_delta))
        {
            return DAS_E_INVALID_ARGUMENT;
        }

        // 六字段直加（复合偏移可并存），随后经 timegm 链归一化进位：
        // {2026,1,31}+{month:1} → 2 月 31 日 → {2026,3,3}（tm/mktime 语义，
        // 不发明 clamp——D-B3）。
        std::tm tm{};
        tm.tm_year = p_base->year - 1900 + static_cast<int>(p_delta->year);
        tm.tm_mon = p_base->month - 1 + static_cast<int>(p_delta->month);
        tm.tm_mday = p_base->day + static_cast<int>(p_delta->day);
        tm.tm_hour = p_base->hour + static_cast<int>(p_delta->hour);
        tm.tm_min = p_base->minute + static_cast<int>(p_delta->minute);
        tm.tm_sec = p_base->second + static_cast<int>(p_delta->second);
        tm.tm_isdst = 0;

        const std::time_t unix_time = DAS::Core::Utils::TimegmPortable(&tm);
        if (unix_time == static_cast<std::time_t>(-1))
        {
            return DAS_E_INVALID_ARGUMENT;
        }
        if (!Details::UnixTimeToDasDate(unix_time, *p_out_result))
        {
            return DAS_E_INVALID_ARGUMENT;
        }
        return DAS_S_OK;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

DAS_C_API DasResult DasDateCompare(
    const Das::ExportInterface::DasDate* p_left,
    const Das::ExportInterface::DasDate* p_right,
    int32_t*                             p_out_result)
{
    DAS_UTILS_CHECK_POINTER(p_left)
    DAS_UTILS_CHECK_POINTER(p_right)
    DAS_UTILS_CHECK_POINTER(p_out_result)

    try
    {
        if (!Details::IsCalendarDateFieldsValid(*p_left)
            || !Details::IsCalendarDateFieldsValid(*p_right))
        {
            return DAS_E_INVALID_ARGUMENT;
        }

        std::time_t left_unix = 0;
        std::time_t right_unix = 0;
        if (!Details::DasDateToUnixTime(*p_left, left_unix)
            || !Details::DasDateToUnixTime(*p_right, right_unix))
        {
            return DAS_E_INVALID_ARGUMENT;
        }

        // 三态：左<右 → -1，相等 → 0，左>右 → 1（超窗判定消费）。
        if (left_unix < right_unix)
        {
            *p_out_result = -1;
        }
        else if (left_unix > right_unix)
        {
            *p_out_result = 1;
        }
        else
        {
            *p_out_result = 0;
        }
        return DAS_S_OK;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}
