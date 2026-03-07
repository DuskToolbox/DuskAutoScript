#include "FeatureDetectorFactory.h"

DAS_CORE_OCVWRAPPER_NS_BEGIN

namespace Details
{

    cv::Ptr<cv::Feature2D> CreateDetector(
        ExportInterface::DasDetectorType type,
        uint32_t                         max_keypoints)
    {
        switch (type)
        {
        case ExportInterface::DAS_DETECTOR_ORB:
            return cv::ORB::create(static_cast<int>(max_keypoints));

        case ExportInterface::DAS_DETECTOR_SIFT:
            return cv::SIFT::create();

        case ExportInterface::DAS_DETECTOR_AKAZE:
            return cv::AKAZE::create();

        case ExportInterface::DAS_DETECTOR_BRISK:
            return cv::BRISK::create();

        default:
            return nullptr;
        }
    }

    ExportInterface::DasMatchedPoint ToDasMatchedPoint(
        const cv::KeyPoint& query_kp,
        const cv::KeyPoint& train_kp,
        float               distance)
    {
        ExportInterface::DasMatchedPoint point;
        point.query_x = query_kp.pt.x;
        point.query_y = query_kp.pt.y;
        point.train_x = train_kp.pt.x;
        point.train_y = train_kp.pt.y;
        point.distance = distance;
        return point;
    }

} // namespace Details

DAS_CORE_OCVWRAPPER_NS_END
