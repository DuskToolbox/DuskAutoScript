#include "DescriptorMatcherFactory.h"

DAS_CORE_OCVWRAPPER_NS_BEGIN

namespace Details
{

    bool IsBinaryDescriptor(ExportInterface::DasDetectorType detector_type)
    {
        return detector_type == ExportInterface::DAS_DETECTOR_ORB
               || detector_type == ExportInterface::DAS_DETECTOR_BRISK;
    }

    cv::Ptr<cv::DescriptorMatcher> CreateMatcher(
        ExportInterface::DasMatcherType  type,
        ExportInterface::DasDetectorType detector_type)
    {
        const bool is_binary = IsBinaryDescriptor(detector_type);

        switch (type)
        {
        case ExportInterface::DAS_MATCHER_BF:
        {
            const int norm_type = is_binary ? cv::NORM_HAMMING : cv::NORM_L2;
            return cv::BFMatcher::create(norm_type, false);
        }

        case ExportInterface::DAS_MATCHER_FLANN:
            return cv::FlannBasedMatcher::create();

        default:
            return nullptr;
        }
    }

} // namespace Details

DAS_CORE_OCVWRAPPER_NS_END
