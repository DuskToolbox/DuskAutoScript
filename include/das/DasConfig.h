#ifndef DAS_DASCONFIG_H
#define DAS_DASCONFIG_H

#include <das/DasExport.h>

#define DAS ::Das

#define USING_DAS using namespace Das;

#define DAS_NS_BEGIN                                                           \
    namespace Das                                                              \
    {

#define DAS_NS_END }

#define DAS_NS_ANONYMOUS_DETAILS_BEGIN                                         \
    namespace Details                                                          \
    {                                                                          \
        namespace                                                              \
        {

#define DAS_NS_ANONYMOUS_DETAILS_END                                           \
    }                                                                          \
    }

#define DAS_FULL_RANGE_OF(x) std::begin(x), std::end(x)

#define DAS_DV_V(x) decltype(x), x

#define DAS_PRAGMA_IMPL(x) _Pragma(#x)
#define DAS_PRAGMA(x) DAS_PRAGMA_IMPL(x)

#define DAS_STR_IMPL(x) #x
#define DAS_STR(x) DAS_STR_IMPL(x)

#define DAS_CONCAT_IMPL(x, y) x##y
#define DAS_CONCAT(x, y) DAS_CONCAT_IMPL(x, y)

#define DAS_TOKEN_PASTE_IMPL(x, y) x##y
#define DAS_TOKEN_PASTE(x, y) DAS_TOKEN_PASTE_IMPL(x, y)

#define DAS_WSTR(x) DAS_WSTR_IMPL(x)
#define DAS_WSTR_IMPL(x) L##x

#define DAS_USTR(x) DAS_USTR_IMPL(x)
#define DAS_USTR_IMPL(x) u##x

#define DAS_U8STR(x) DAS_U8STR_IMPL(x)
#define DAS_U8STR_IMPL(x) u8##x

#define DAS_USING_BASE_CTOR(base) using base::base

#ifdef _MSC_VER
#define DAS_DISABLE_WARNING_BEGIN DAS_PRAGMA(warning(push))

#define DAS_IGNORE_UNUSED_PARAMETER DAS_PRAGMA(warning(disable : 4100))
#define DAS_IGNORE_UNUSED_FUNCTION

#define DAS_IGNORE_OPENCV_WARNING                                              \
    DAS_IGNORE_UNUSED_PARAMETER                                                \
    DAS_PRAGMA(warning(disable : 4127 4244 4251 4275 4305 5054))

#define DAS_IGNORE_STDEXEC_PARAMETERS                                          \
    DAS_IGNORE_UNUSED_PARAMETER                                                \
    DAS_PRAGMA(warning(disable : 4324 4456))

#elif defined(__clang__)
#define DAS_DISABLE_WARNING_BEGIN DAS_PRAGMA(clang diagnostic push)

#define DAS_IGNORE_UNUSED_PARAMETER                                            \
    DAS_PRAGMA(clang diagnostic ignored "-Wunused-parameter")
#define DAS_IGNORE_UNUSED_FUNCTION                                             \
    DAS_PRAGMA(clang diagnostic ignored "-Wunused-function")

#define DAS_IGNORE_OPENCV_WARNING                                              \
    DAS_PRAGMA(clang diagnostic ignored "-Wc11-extensions")

#define DAS_IGNORE_STDEXEC_PARAMETERS DAS_IGNORE_UNUSED_PARAMETER

#elif defined(__GNUC__)
#define DAS_DISABLE_WARNING_BEGIN DAS_PRAGMA(GCC diagnostic push)

#define DAS_IGNORE_UNUSED_PARAMETER                                            \
    DAS_PRAGMA(GCC diagnostic ignored "-Wunused-parameter")
#define DAS_IGNORE_UNUSED_FUNCTION                                             \
    DAS_PRAGMA(GCC diagnostic ignored "-Wunused-function")

#define DAS_IGNORE_OPENCV_WARNING                                              \
    DAS_PRAGMA(GCC diagnostic ignored "-Wdeprecated-enum-enum-conversion")

#define DAS_IGNORE_STDEXEC_PARAMETERS DAS_IGNORE_UNUSED_PARAMETER
#endif

#ifdef _MSC_VER
#define DAS_DISABLE_WARNING_END DAS_PRAGMA(warning(pop))

#elif defined(__clang__)
#define DAS_DISABLE_WARNING_END DAS_PRAGMA(clang diagnostic pop)

#elif defined(__GNUC__)
#define DAS_DISABLE_WARNING_END DAS_PRAGMA(GCC diagnostic pop)

#endif

#define DAS_DEFINE_VARIABLE(...) decltype(__VA_ARGS__) __VA_ARGS__

#endif // DAS_DASCONFIG_H
