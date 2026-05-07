#include "IDasTensorVectorImpl.h"

#include <das/Core/Logger/Logger.h>
#include <das/Utils/CommonUtils.hpp>

DAS_CORE_ORTWRAPPER_NS_BEGIN

DasResult IDasTensorVectorImpl::GetCount(uint32_t* p_count)
{
    DAS_UTILS_CHECK_POINTER(p_count);

    *p_count = static_cast<uint32_t>(items_.size());
    return DAS_S_OK;
}

DasResult IDasTensorVectorImpl::GetAt(uint32_t index, IDasTensor** pp_out_value)
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

void IDasTensorVectorImpl::AddTensor(IDasTensor* p_tensor)
{
    items_.emplace_back(p_tensor);
}

void IDasTensorVectorImpl::Reserve(size_t count) { items_.reserve(count); }

DAS_CORE_ORTWRAPPER_NS_END
