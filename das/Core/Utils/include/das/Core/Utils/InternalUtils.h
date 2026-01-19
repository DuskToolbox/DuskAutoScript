#ifndef DAS_CORE_UTILS_INTERNALUTILS_H
#define DAS_CORE_UTILS_INTERNALUTILS_H

#include <das/Core/Utils/Config.h>
#include <das/DasException.hpp>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Expected.h>
#include <string_view>

DAS_CORE_UTILS_NS_BEGIN

inline void* VoidP(void* pointer) { return pointer; }

[[nodiscard]]
auto MakeDasReadOnlyStringFromUtf8(std::string_view u8_string)
    -> DAS::Utils::Expected<DasPtr<IDasReadOnlyString>>;

template <class T>
auto GetGuidFrom(T* p_object) -> DasGuid
{
    DasGuid guid;
    if (const auto gg_result = p_object->GetGuid(&guid); IsFailed(gg_result))
    {
        DAS_THROW_EC_EX(gg_result, p_object);
    }
    return guid;
}

template <class T>
auto GetRuntimeClassNameFrom(T* p_object) -> DasPtr<IDasReadOnlyString>
{
    DasPtr<IDasReadOnlyString> result{};
    if (const auto error_code = p_object->GetRuntimeClassName(result.Put());
        IsFailed(error_code)) [[unlikely]]
    {
        DAS_THROW_EC_EX(error_code, p_object);
    }
    return result;
}

DAS_CORE_UTILS_NS_END

#endif // DAS_CORE_UTILS_INTERNALUTILS_H
