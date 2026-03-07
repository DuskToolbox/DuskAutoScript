#include "IMatchConfigImpl.h"

DAS_CORE_OCVWRAPPER_NS_BEGIN

IMatchConfigImpl::IMatchConfigImpl(
    ExportInterface::DasDetectorType detector_type,
    ExportInterface::DasMatcherType  matcher_type,
    ExportInterface::DasMatchParams  params)
    : detector_type_(detector_type), matcher_type_(matcher_type),
      params_(params)
{
}

DasResult IMatchConfigImpl::GetDetectorType(
    ExportInterface::DasDetectorType* p_out_type)
{
    if (!p_out_type)
        return DAS_E_INVALID_POINTER;
    *p_out_type = detector_type_;
    return DAS_S_OK;
}

DasResult IMatchConfigImpl::GetMatcherType(
    ExportInterface::DasMatcherType* p_out_type)
{
    if (!p_out_type)
        return DAS_E_INVALID_POINTER;
    *p_out_type = matcher_type_;
    return DAS_S_OK;
}

DasResult IMatchConfigImpl::GetParams(
    ExportInterface::DasMatchParams* p_out_params)
{
    if (!p_out_params)
        return DAS_E_INVALID_POINTER;
    *p_out_params = params_;
    return DAS_S_OK;
}

DAS_CORE_OCVWRAPPER_NS_END
