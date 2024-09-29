#ifndef ASR_ASRCONFIG_H
#define ASR_ASRCONFIG_H

#include <AutoStarRail/AsrExport.h>

#define ASR ::Asr

#define USING_ASR using namespace Asr;

#define ASR_NS_BEGIN                                                           \
    namespace Asr                                                              \
    {

#define ASR_NS_END }

#define ASR_NS_ANONYMOUS_DETAILS_BEGIN                                         \
    namespace Details                                                          \
    {                                                                          \
        namespace                                                              \
        {

#define ASR_NS_ANONYMOUS_DETAILS_END                                           \
    }                                                                          \
    }

#define ASR_FULL_RANGE_OF(x) std::begin(x), std::end(x)

#define ASR_DV_V(x) decltype(x), x

#define ASR_PRAGMA_IMPL(x) _Pragma(#x)
#define ASR_PRAGMA(x) ASR_PRAGMA_IMPL(x)

#define ASR_STR_IMPL(x) #x
#define ASR_STR(x) ASR_STR_IMPL(x)

#define ASR_CONCAT_IMPL(x, y) x##y
#define ASR_CONCAT(x, y) ASR_CONCAT_IMPL(x, y)

#define ASR_TOKEN_PASTE_IMPL(x, y) x##y
#define ASR_TOKEN_PASTE(x, y) ASR_TOKEN_PASTE_IMPL(x, y)

#define ASR_WSTR(x) ASR_WSTR_IMPL(x)
#define ASR_WSTR_IMPL(x) L##x

#define ASR_USTR(x) ASR_USTR_IMPL(x)
#define ASR_USTR_IMPL(x) u##x

#define ASR_U8STR(x) ASR_U8STR_IMPL(x)
#define ASR_U8STR_IMPL(x) u8##x

#define ASR_USING_BASE_CTOR(base) using base::base

#ifdef _MSC_VER
#define ASR_DISABLE_WARNING_BEGIN ASR_PRAGMA(warning(push))

#define ASR_IGNORE_UNUSED_PARAMETER ASR_PRAGMA(warning(disable : 4100))
#define ASR_IGNORE_UNUSED_FUNCTION

#define ASR_IGNORE_OPENCV_WARNING                                              \
    ASR_IGNORE_UNUSED_PARAMETER                                                \
    ASR_PRAGMA(warning(disable : 4127 4244 4251 4275 4305 5054))

#define ASR_IGNORE_STDEXEC_PARAMETERS                                          \
    ASR_IGNORE_UNUSED_PARAMETER                                                \
    ASR_PRAGMA(warning(disable : 4324 4456))

#elif defined(__clang__)
#define ASR_DISABLE_WARNING_BEGIN ASR_PRAGMA(clang diagnostic push)

#define ASR_IGNORE_UNUSED_PARAMETER                                            \
    ASR_PRAGMA(clang diagnostic ignored "-Wunused-parameter")
#define ASR_IGNORE_UNUSED_FUNCTION                                             \
    ASR_PRAGMA(clang diagnostic ignored "-Wunused-function")

#define ASR_IGNORE_OPENCV_WARNING                                              \
    ASR_PRAGMA(clang diagnostic ignored "-Wc11-extensions")

#define ASR_IGNORE_STDEXEC_PARAMETERS ASR_IGNORE_UNUSED_PARAMETER

#elif defined(__GNUC__)
#define ASR_DISABLE_WARNING_BEGIN ASR_PRAGMA(GCC diagnostic push)

#define ASR_IGNORE_UNUSED_PARAMETER                                            \
    ASR_PRAGMA(GCC diagnostic ignored "-Wunused-parameter")
#define ASR_IGNORE_UNUSED_FUNCTION                                             \
    ASR_PRAGMA(GCC diagnostic ignored "-Wunused-function")

#define ASR_IGNORE_OPENCV_WARNING                                              \
    ASR_PRAGMA(GCC diagnostic ignored "-Wdeprecated-enum-enum-conversion")

#define ASR_IGNORE_STDEXEC_PARAMETERS ASR_IGNORE_UNUSED_PARAMETER
#endif

#ifdef _MSC_VER
#define ASR_DISABLE_WARNING_END ASR_PRAGMA(warning(pop))

#elif defined(__clang__)
#define ASR_DISABLE_WARNING_END ASR_PRAGMA(clang diagnostic pop)

#elif defined(__GNUC__)
#define ASR_DISABLE_WARNING_END ASR_PRAGMA(GCC diagnostic pop)

#endif

#define ASR_DEFINE_VARIABLE(...) decltype(__VA_ARGS__) __VA_ARGS__

#endif // ASR_ASRCONFIG_H
