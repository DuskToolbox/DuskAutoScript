#ifndef DAS_CORE_OCVWRAPPER_IDASTEMPLATEMATCHRESULTIMPL_H
#define DAS_CORE_OCVWRAPPER_IDASTEMPLATEMATCHRESULTIMPL_H

#include "Config.h"
#include <das/_autogen/idl/abi/DasBasicTypes.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasTemplateMatchResult.Implements.hpp>

// {A7B8C9D0-E1F2-4A3B-8C7D-9E0F1A2B3C4D}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::OcvWrapper,
    IDasTemplateMatchResultImpl,
    0xa7b8c9d0,
    0xe1f2,
    0x4a3b,
    0x8c,
    0x7d,
    0x9e,
    0x0f,
    0x1a,
    0x2b,
    0x3c,
    0x4d);

DAS_CORE_OCVWRAPPER_NS_BEGIN

class IDasTemplateMatchResultImpl final
    : public ExportInterface::DasTemplateMatchResultImplBase<
          IDasTemplateMatchResultImpl>
{
    double                   score_{};
    ExportInterface::DasRect match_rect_{};

public:
    IDasTemplateMatchResultImpl() = default;
    IDasTemplateMatchResultImpl(double score, ExportInterface::DasRect rect);

    DAS_IMPL Getscore(double* p_out) override;
    DAS_IMPL Setscore(double value) override;
    DAS_IMPL Getmatch_rect(ExportInterface::DasRect* p_out) override;
    DAS_IMPL Setmatch_rect(ExportInterface::DasRect value) override;
};

DAS_CORE_OCVWRAPPER_NS_END

#endif // DAS_CORE_OCVWRAPPER_IDASTEMPLATEMATCHRESULTIMPL_H
