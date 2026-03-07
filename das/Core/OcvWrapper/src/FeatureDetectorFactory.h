#ifndef DAS_CORE_OCVWRAPPER_FEATUREDETECTORFACTORY_H
#define DAS_CORE_OCVWRAPPER_FEATUREDETECTORFACTORY_H

#include "Config.h"

#include <das/_autogen/idl/abi/DasCV.h>

DAS_DISABLE_WARNING_BEGIN

DAS_IGNORE_OPENCV_WARNING
#include <opencv2/features2d.hpp>

DAS_DISABLE_WARNING_END

DAS_CORE_OCVWRAPPER_NS_BEGIN

namespace Details
{

    cv::Ptr<cv::Feature2D> CreateDetector(
        ExportInterface::DasDetectorType type,
        uint32_t                         max_keypoints);

    ExportInterface::DasMatchedPoint ToDasMatchedPoint(
        const cv::KeyPoint& query_kp,
        const cv::KeyPoint& train_kp,
        float               distance);

} // namespace Details

DAS_CORE_OCVWRAPPER_NS_END

#endif
