#include "FeatureDetectorFactory.h"

DAS_DISABLE_WARNING_BEGIN

DAS_IGNORE_OPENCV_WARNING
#if CV_VERSION_MAJOR >= 5
#include <opencv2/xfeatures2d.hpp>
#endif

DAS_DISABLE_WARNING_END

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
#if CV_VERSION_MAJOR >= 5
            return cv::xfeatures2d::AKAZE::create();
#else
            return cv::AKAZE::create();
#endif

        case ExportInterface::DAS_DETECTOR_BRISK:
#if CV_VERSION_MAJOR >= 5
            return cv::xfeatures2d::BRISK::create();
#else
            return cv::BRISK::create();
#endif

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
