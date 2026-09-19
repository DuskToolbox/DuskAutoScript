#ifndef DAS_CORE_UTILS_PORTABLE_TIME_H
#define DAS_CORE_UTILS_PORTABLE_TIME_H

#include <ctime>
#include <das/DasConfig.h>

DAS_NS_BEGIN
namespace Core
{
    namespace Utils
    {
        // 跨平台 gmtime_s(Win32, tm*, const time_t*) / gmtime_r(POSIX)。
        // 返回 nullptr 表示转换失败（调用方按非法输入处理）。
        inline std::tm* PortableGmtime(const std::time_t* timer, std::tm* buf)
        {
#ifdef _WIN32
            return gmtime_s(buf, timer) == 0 ? buf : nullptr;
#else
            return gmtime_r(timer, buf);
#endif
        }

        // 跨平台 localtime_s(Win32, tm*, const time_t*) / localtime_r(POSIX)。
        inline std::tm* PortableLocaltime(
            const std::time_t* timer,
            std::tm*           buf)
        {
#ifdef _WIN32
            return localtime_s(buf, timer) == 0 ? buf : nullptr;
#else
            return localtime_r(timer, buf);
#endif
        }

        // 跨平台 _mkgmtime(Win32) / timegm(POSIX)：把 UTC 字段 tm 编码成 unix
        // 时间戳。DasDate wire 按 UTC 解释（SchedulerService DasDateToUnix 同
        // 链），禁止用本地时区的 mktime 解释 DasDate 字段。
        inline std::time_t TimegmPortable(std::tm* tm)
        {
#ifdef _WIN32
            return _mkgmtime(tm);
#else
            return timegm(tm);
#endif
        }
    } // namespace Utils
} // namespace Core
DAS_NS_END

#endif // DAS_CORE_UTILS_PORTABLE_TIME_H
