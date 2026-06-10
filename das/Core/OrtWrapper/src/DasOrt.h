#ifndef DAS_CORE_IMPL_DASORT_H
#define DAS_CORE_IMPL_DASORT_H

#include <onnxruntime_cxx_api.h>

#if !defined(_MSC_VER) && defined(_WIN32)
#undef _stdcall
#endif

#include "Config.h"
#include <boost/predef/os.h>
#include <das/DasString.hpp>

#include <string>

#if BOOST_OS_WINDOWS
static_assert(
    sizeof(wchar_t) == sizeof(char16_t),
    "wchar_t and char16_t must be same size for ORT path zero-copy");
#endif

DAS_CORE_ORTWRAPPER_NS_BEGIN

#if BOOST_OS_WINDOWS
/// Borrow UTF-16 buffer as const ORTCHAR_T* for Ort::Session constructor.
/// The DasReadOnlyString parameter (passed by value) holds a DasPtr that
/// maintains the reference count on the underlying IDasReadOnlyString,
/// keeping the UTF-16 buffer alive through the borrow scope.
inline const ORTCHAR_T* BorrowOrtPath(DasReadOnlyString string)
{
    const char16_t* utf16 = nullptr;
    size_t          utf16_size = 0;
    string.BorrowUtf16(&utf16, &utf16_size);
    // BorrowUtf16 guarantees utf16[utf16_size] == 0 per Phase 78.1 contract
    return reinterpret_cast<const ORTCHAR_T*>(utf16);
}
#else
/// Borrow UTF-8 buffer as const char* for Ort::Session constructor.
/// On non-Windows, ORTCHAR_T is char and GetUtf8 returns cached UTF-8.
inline const char* BorrowOrtPath(DasReadOnlyString string)
{
    return string.GetUtf8();
}
#endif

class DasOrt
{
protected:
    Ort::Env                         env_{};
    Ort::SessionOptions              Session_options_{};
    Ort::AllocatorWithDefaultOptions allocator_{};

    static Ort::MemoryInfo& GetDefaultCpuMemoryInfo();

public:
    DasOrt(const char* model_name);

    const Ort::Env& GetEnv() const { return env_; }
};

DAS_CORE_ORTWRAPPER_NS_END

#endif // DAS_CORE_IMPL_DASORT_H
