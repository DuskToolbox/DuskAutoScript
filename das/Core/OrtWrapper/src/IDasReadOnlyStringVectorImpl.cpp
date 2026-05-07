#include "IDasReadOnlyStringVectorImpl.h"

#include <das/Core/Logger/Logger.h>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>

DAS_CORE_ORTWRAPPER_NS_BEGIN

IDasReadOnlyStringVectorImpl::IDasReadOnlyStringVectorImpl(
    std::vector<std::string> strings)
{
    items_.reserve(strings.size());
    for (auto& s : strings)
    {
        Das::DasPtr<IDasReadOnlyString> p_str;
        CreateIDasReadOnlyStringFromChar(s.c_str(), p_str.Put());
        items_.push_back(std::move(p_str));
    }
}

DasResult IDasReadOnlyStringVectorImpl::GetCount(uint32_t* p_count)
{
    DAS_UTILS_CHECK_POINTER(p_count);

    *p_count = static_cast<uint32_t>(items_.size());
    return DAS_S_OK;
}

DasResult IDasReadOnlyStringVectorImpl::GetAt(
    uint32_t             index,
    IDasReadOnlyString** pp_out_value)
{
    DAS_UTILS_CHECK_POINTER(pp_out_value);

    if (index >= items_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    *pp_out_value = items_[index].Get();
    (*pp_out_value)->AddRef();
    return DAS_S_OK;
}

void IDasReadOnlyStringVectorImpl::AddString(const char* str)
{
    Das::DasPtr<IDasReadOnlyString> p_str;
    CreateIDasReadOnlyStringFromChar(str, p_str.Put());
    items_.push_back(std::move(p_str));
}

void IDasReadOnlyStringVectorImpl::Reserve(size_t count)
{
    items_.reserve(count);
}

DAS_CORE_ORTWRAPPER_NS_END
