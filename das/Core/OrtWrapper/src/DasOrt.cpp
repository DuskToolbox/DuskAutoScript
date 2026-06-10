#include "DasOrt.h"

DAS_CORE_ORTWRAPPER_NS_BEGIN

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
