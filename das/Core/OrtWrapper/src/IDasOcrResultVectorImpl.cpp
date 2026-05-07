#include "IDasOcrResultVectorImpl.h"

#include <das/Core/Logger/Logger.h>
#include <das/Utils/CommonUtils.hpp>

DAS_CORE_ORTWRAPPER_NS_BEGIN

DasResult IDasOcrResultVectorImpl::GetCount(uint32_t* p_count)
{
    DAS_UTILS_CHECK_POINTER(p_count);

    *p_count = static_cast<uint32_t>(items_.size());
    return DAS_S_OK;
}

DasResult IDasOcrResultVectorImpl::GetAt(
    uint32_t                              index,
    Das::ExportInterface::IDasOcrResult** pp_out)
{
    DAS_UTILS_CHECK_POINTER(pp_out);

    if (index >= static_cast<uint32_t>(items_.size()))
    {
        return DAS_E_OUT_OF_RANGE;
    }

    items_[index]->AddRef();
    *pp_out = items_[index].Get();
    return DAS_S_OK;
}

void IDasOcrResultVectorImpl::AddResult(
    Das::ExportInterface::IDasOcrResult* p_result)
{
    if (p_result)
    {
        items_.emplace_back(
            Das::DasPtr<Das::ExportInterface::IDasOcrResult>(p_result));
    }
}

void IDasOcrResultVectorImpl::Reserve(size_t count) { items_.reserve(count); }

DAS_CORE_ORTWRAPPER_NS_END
