#include "IDasMatchResultImpl.h"

DAS_CORE_OCVWRAPPER_NS_BEGIN

DasResult IDasMatchResultImpl::GetMatchCount(uint32_t* p_out_count)
{
    if (!p_out_count)
        return DAS_E_INVALID_POINTER;
    *p_out_count = static_cast<uint32_t>(matches_.size());
    return DAS_S_OK;
}

DasResult IDasMatchResultImpl::GetMatchPair(
    uint32_t                          index,
    ExportInterface::DasMatchedPoint* p_out_pair)
{
    if (!p_out_pair)
        return DAS_E_INVALID_POINTER;

    if (index >= matches_.size())
        return DAS_E_OUT_OF_RANGE;

    *p_out_pair = matches_[index];
    return DAS_S_OK;
}

void IDasMatchResultImpl::AddMatch(
    const ExportInterface::DasMatchedPoint& match)
{
    matches_.push_back(match);
}

void IDasMatchResultImpl::Reserve(size_t count) { matches_.reserve(count); }

DAS_CORE_OCVWRAPPER_NS_END
