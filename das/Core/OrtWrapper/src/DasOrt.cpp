#include "DasOrt.h"
#include <das/DasString.hpp>
#include <boost/predef/os.h>

DAS_CORE_ORTWRAPPER_NS_BEGIN

const ORTCHAR_T* ToOrtChar(DasReadOnlyString string)
{
#if (BOOST_OS_WINDOWS)
    return string.GetW();
#else
    return string.GetUtf8();
#endif
}

const ORTCHAR_T* ToOrtChar(IDasReadOnlyString* p_string)
{
    auto string = DasReadOnlyString(p_string);
    return ToOrtChar(string);
}

Ort::MemoryInfo& DasOrt::GetDefaultCpuMemoryInfo()
{
    static auto result = Ort::MemoryInfo::CreateCpu(
        OrtAllocatorType::OrtArenaAllocator,
        OrtMemType::OrtMemTypeCPU);
    return result;
}

DasOrt::DasOrt(const char* model_name)
    : env_{ORT_LOGGING_LEVEL_WARNING, model_name}
{
}

DAS_CORE_ORTWRAPPER_NS_END