#ifndef DAS_CORE_IMPL_DASORT_H
#define DAS_CORE_IMPL_DASORT_H

/**
 * @brief hack onnxruntime header in order to compile on mingw.
 *
 */
#if !defined(_MSC_VER) && defined(_WIN32)
#define _In_
#define _In_z_
#define _In_opt_
#define _In_opt_z_
#define _Out_
#define _Outptr_
#define _Out_opt_
#define _Inout_
#define _Inout_opt_
#define _Frees_ptr_opt_
#define _Ret_maybenull_
#define _Ret_notnull_
#define _Check_return_
#define _Outptr_result_maybenull_
#define _In_reads_(X)
#define _Inout_updates_all_(X)
#define _Out_writes_bytes_all_(X)
#define _Out_writes_all_(X)
#define _Success_(X)
#define _Outptr_result_buffer_maybenull_(X)
#define ORT_ALL_ARGS_NONNULL __attribute__((nonnull))
#define _stdcall __attribute__((__stdcall__))
#endif

#include <onnxruntime_cxx_api.h>

#if !defined(_MSC_VER) && defined(_WIN32)
#undef _stdcall
#endif

#include "Config.h"
#include <das/DasString.hpp>

DAS_CORE_ORTWRAPPER_NS_BEGIN

const ORTCHAR_T* ToOrtChar(DasReadOnlyString string);

const ORTCHAR_T* ToOrtChar(IDasReadOnlyString* p_string);

class DasOrt
{
protected:
    Ort::Env env_{};
    Ort::SessionOptions Session_options_{};
    Ort::AllocatorWithDefaultOptions allocator_{};

    static Ort::MemoryInfo& GetDefaultCpuMemoryInfo();

public:
    DasOrt(const char* model_name);
};

DAS_CORE_ORTWRAPPER_NS_END

#endif // DAS_CORE_IMPL_DASORT_H
