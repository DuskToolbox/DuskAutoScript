#ifndef DAS_CORE_UTILS_INTERNALUTILS_H
#define DAS_CORE_UTILS_INTERNALUTILS_H

#include <das/Core/Exceptions/DasException.h>
#include <das/Core/ForeignInterfaceHost/CppSwigInterop.h>
#include <das/Core/Utils/Config.h>
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
    if constexpr (DAS::Core::ForeignInterfaceHost::is_das_swig_interface<T>)
    {
        const auto ret_guid = p_object->GetGuid();
        if (IsFailed(ret_guid.error_code))
        {
            DAS_THROW_EC_EX(ret_guid.error_code, p_object);
        }
        return ret_guid.value;
    }
    else if constexpr (DAS::Core::ForeignInterfaceHost::is_das_interface<T>)
    {
        DasGuid guid;
        if (const auto gg_result = p_object->GetGuid(&guid);
            IsFailed(gg_result))
        {
            DAS_THROW_EC_EX(gg_result, p_object);
        }
        return guid;
    }
    else
    {
        static_assert(DAS::Utils::value<false, T>, "没有匹配的类型！");
    }
}

template <class T>
auto GetRuntimeClassNameFrom(T* p_object) -> DasPtr<IDasReadOnlyString>
{
    if constexpr (DAS::Core::ForeignInterfaceHost::is_das_swig_interface<T>)
    {
        DasPtr<IDasReadOnlyString> result{};
        const auto                 ret_name = p_object->GetRuntimeClassName();
        if (IsFailed(ret_name.error_code))
        {
            DAS_THROW_EC_EX(ret_name.error_code, p_object);
        }
        ret_name.value.GetImpl(result.Put());
        return result;
    }
    else if constexpr (DAS::Core::ForeignInterfaceHost::is_das_interface<T>)
    {
        DasPtr<IDasReadOnlyString> result{};
        if (const auto error_code = p_object->GetRuntimeClassName(result.Put());
            IsFailed(error_code)) [[unlikely]]
        {
            DAS_THROW_EC_EX(error_code, p_object);
        }
        return result;
    }
    else
    {
        static_assert(DAS::Utils::value<false, T>, "没有匹配的类型！");
    }
}

DAS_CORE_UTILS_NS_END

#endif // DAS_CORE_UTILS_INTERNALUTILS_H
