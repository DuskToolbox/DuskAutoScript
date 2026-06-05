#ifndef DAS_CORE_IMPL_DASORT_H
#define DAS_CORE_IMPL_DASORT_H

#include <onnxruntime_cxx_api.h>

#if !defined(_MSC_VER) && defined(_WIN32)
#undef _stdcall
#endif

#include "Config.h"
#include <das/DasString.hpp>

#include <string>

DAS_CORE_ORTWRAPPER_NS_BEGIN

std::basic_string<ORTCHAR_T> ToOrtPath(DasReadOnlyString string);

std::basic_string<ORTCHAR_T> ToOrtPath(IDasReadOnlyString* p_string);

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
