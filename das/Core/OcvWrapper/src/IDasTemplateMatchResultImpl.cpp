#include "IDasTemplateMatchResultImpl.h"

DAS_CORE_OCVWRAPPER_NS_BEGIN

IDasTemplateMatchResultImpl::IDasTemplateMatchResultImpl(double score, ExportInterface::DasRect rect)
    : score_(score), match_rect_(rect)
{
}

DasResult IDasTemplateMatchResultImpl::Getscore(double* p_out)
{
    if (!p_out) return DAS_E_INVALID_POINTER;
    *p_out = score_;
    return DAS_S_OK;
}

DasResult IDasTemplateMatchResultImpl::Setscore(double value)
{
    score_ = value;
    return DAS_S_OK;
}

DasResult IDasTemplateMatchResultImpl::Getmatch_rect(ExportInterface::DasRect* p_out)
{
    if (!p_out) return DAS_E_INVALID_POINTER;
    *p_out = match_rect_;
    return DAS_S_OK;
}

DasResult IDasTemplateMatchResultImpl::Setmatch_rect(ExportInterface::DasRect value)
{
    match_rect_ = value;
    return DAS_S_OK;
}

DAS_CORE_OCVWRAPPER_NS_END
