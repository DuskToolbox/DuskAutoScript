#ifndef DAS_CORE_OCVWRAPPER_DESCRIPTORMATCHERFACTORY_H
#define DAS_CORE_OCVWRAPPER_DESCRIPTORMATCHERFACTORY_H

#include "Config.h"

#include <das/_autogen/idl/abi/DasCV.h>

DAS_DISABLE_WARNING_BEGIN

DAS_IGNORE_OPENCV_WARNING
#include <opencv2/features2d.hpp>

DAS_DISABLE_WARNING_END

DAS_CORE_OCVWRAPPER_NS_BEGIN

namespace Details
{

    cv::Ptr<cv::DescriptorMatcher> CreateMatcher(
        ExportInterface::DasMatcherType  type,
        ExportInterface::DasDetectorType detector_type);

    bool IsBinaryDescriptor(ExportInterface::DasDetectorType detector_type);

} // namespace Details

DAS_CORE_OCVWRAPPER_NS_END

#endif
