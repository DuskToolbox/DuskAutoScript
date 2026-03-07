#ifndef DAS_CORE_OCVWRAPPER_IMATCHCONFIGIMPL_H
#define DAS_CORE_OCVWRAPPER_IMATCHCONFIGIMPL_H

#include "Config.h"

#include <das/_autogen/idl/abi/DasCV.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasCvMatchConfig.Implements.hpp>

// {C1D2E3F4-A5B6-4C7D-8E9F-0A1B2C3D4E5F}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::OcvWrapper,
    IMatchConfigImpl,
    0xc1d2e3f4,
    0xa5b6,
    0x4c7d,
    0x8e,
    0x9f,
    0x0a,
    0x1b,
    0x2c,
    0x3d,
    0x4e,
    0x5f);

DAS_CORE_OCVWRAPPER_NS_BEGIN

class IMatchConfigImpl final
    : public ExportInterface::DasCvMatchConfigImplBase<IMatchConfigImpl>
{
    ExportInterface::DasDetectorType detector_type_{
        ExportInterface::DAS_DETECTOR_ORB};
    ExportInterface::DasMatcherType matcher_type_{
        ExportInterface::DAS_MATCHER_BF};
    ExportInterface::DasMatchParams params_{0.75f, false, 500};

public:
    IMatchConfigImpl() = default;

    IMatchConfigImpl(
        ExportInterface::DasDetectorType detector_type,
        ExportInterface::DasMatcherType  matcher_type,
        ExportInterface::DasMatchParams  params);

    DAS_IMPL GetDetectorType(
        ExportInterface::DasDetectorType* p_out_type) override;
    DAS_IMPL GetMatcherType(
        ExportInterface::DasMatcherType* p_out_type) override;
    DAS_IMPL GetParams(ExportInterface::DasMatchParams* p_out_params) override;
};

DAS_CORE_OCVWRAPPER_NS_END

#endif // DAS_CORE_OCVWRAPPER_IMATCHCONFIGIMPL_H
