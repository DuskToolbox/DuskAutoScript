#include "IDasTemplateMatchResultsImpl.h"

DAS_CORE_OCVWRAPPER_NS_BEGIN

DasResult IDasTemplateMatchResultsImpl::GetCount(uint32_t* p_out_count)
{
    if (!p_out_count)
    {
        return DAS_E_INVALID_POINTER;
    }
    *p_out_count = static_cast<uint32_t>(results_.size());
    return DAS_S_OK;
}

DasResult IDasTemplateMatchResultsImpl::GetRawMatchCount(
    uint32_t* p_out_raw_count)
{
    if (!p_out_raw_count)
    {
        return DAS_E_INVALID_POINTER;
    }
    *p_out_raw_count = raw_match_count_;
    return DAS_S_OK;
}

DasResult IDasTemplateMatchResultsImpl::GetAt(
    uint32_t                                   index,
    ExportInterface::IDasTemplateMatchResult** pp_out_result)
{
    if (!pp_out_result)
    {
        return DAS_E_INVALID_POINTER;
    }
    if (index >= results_.size())
    {
        return DAS_E_OUT_OF_RANGE;
    }

    *pp_out_result = results_[index].Get();
    (*pp_out_result)->AddRef();
    return DAS_S_OK;
}

void IDasTemplateMatchResultsImpl::AddResult(
    ExportInterface::IDasTemplateMatchResult* p_result)
{
    results_.emplace_back(p_result);
}

void IDasTemplateMatchResultsImpl::SetRawMatchCount(uint32_t count)
{
    raw_match_count_ = count;
}

void IDasTemplateMatchResultsImpl::Reserve(size_t count)
{
    results_.reserve(count);
}

DAS_CORE_OCVWRAPPER_NS_END
