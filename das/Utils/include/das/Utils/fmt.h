#ifndef DAS_UTILS_FMT_H
#define DAS_UTILS_FMT_H

#include <das/DasConfig.h>

#ifndef DAS_USE_STD_FMT
#define DAS_USE_STD_FMT 1
#endif

#if DAS_USE_STD_FMT
#include <format>
DAS_NS_BEGIN
namespace fmt
{
    using namespace ::std;
}
namespace FmtCommon
{
    template <class T>
    auto to_string(T&& value)
    {
        return DAS::fmt::format("{}", std::forward<T>(value));
    }
}
DAS_NS_END
#define DAS_FMT_NS std
#else
#include <fmt/core.h>
#include <fmt/format.h>
DAS_NS_BEGIN
namespace fmt
{
    using namespace ::fmt;
}
namespace FmtCommon
{
    template <class T>
    auto to_string(T&& value)
    {
        return DAS::fmt::to_string(std::forward<T>(value));
    }
}
DAS_NS_END
#define DAS_FMT_NS fmt
#endif

#endif // DAS_UTILS_FMT_H
