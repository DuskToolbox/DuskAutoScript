#include "CvCpuImpl.h"
#include <das/_autogen/idl/abi/DasCV.h>

DAS_CORE_OCVWRAPPER_NS_BEGIN

DasResult TemplateMatchBest(
    Das::ExportInterface::IDasImage*                p_image,
    Das::ExportInterface::IDasImage*                p_template,
    Das::ExportInterface::DasTemplateMatchType      type,
    Das::ExportInterface::IDasTemplateMatchResult** pp_out_result)
{
    CvCpuImpl impl;
    return impl.TemplateMatchBest(p_image, p_template, type, pp_out_result);
}

DasResult CreateMatchConfig(
    Das::ExportInterface::DasDetectorType     detector_type,
    Das::ExportInterface::DasMatcherType      matcher_type,
    Das::ExportInterface::DasMatchParams      params,
    Das::ExportInterface::IDasCvMatchConfig** pp_out_config)
{
    CvCpuImpl impl;
    return impl
        .CreateMatchConfig(detector_type, matcher_type, params, pp_out_config);
}

DasResult MatchFeatures(
    Das::ExportInterface::IDasImage*          p_query,
    Das::ExportInterface::IDasImage*          p_train,
    Das::ExportInterface::IDasCvMatchConfig*  p_config,
    Das::ExportInterface::IDasCvMatchResult** pp_out_result)
{
    CvCpuImpl impl;
    return impl.MatchFeatures(p_query, p_train, p_config, pp_out_result);
}

DAS_CORE_OCVWRAPPER_NS_END
