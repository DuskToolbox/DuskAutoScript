#include "DasOrt.h"
#include <boost/predef/os.h>
#include <das/DasString.hpp>

DAS_CORE_ORTWRAPPER_NS_BEGIN

std::basic_string<ORTCHAR_T> ToOrtPath(DasReadOnlyString string)
{
#if (BOOST_OS_WINDOWS)
    const char16_t* utf16 = nullptr;
    size_t          utf16_size = 0;
    string.BorrowUtf16(&utf16, &utf16_size);

    std::basic_string<ORTCHAR_T> result;
    if (utf16 == nullptr || utf16_size == 0)
    {
        return result;
    }

    result.reserve(utf16_size);
    for (size_t index = 0; index < utf16_size; ++index)
    {
        result.push_back(static_cast<ORTCHAR_T>(utf16[index]));
    }
    return result;
#else
    return string.GetUtf8();
#endif
}

std::basic_string<ORTCHAR_T> ToOrtPath(IDasReadOnlyString* p_string)
{
    auto string = DasReadOnlyString(p_string);
    return ToOrtPath(string);
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
