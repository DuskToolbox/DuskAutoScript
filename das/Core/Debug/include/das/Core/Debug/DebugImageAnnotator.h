#ifndef DAS_CORE_DEBUG_DEBUGIMAGEANNOTATOR_H
#define DAS_CORE_DEBUG_DEBUGIMAGEANNOTATOR_H

#include <das/Core/Debug/Config.h>
#include <das/_autogen/idl/abi/DasCV.h>
#include <das/_autogen/idl/abi/IDasImage.h>

#include <memory>
#include <string>
#include <vector>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_OPENCV_WARNING
#include <opencv2/core/mat.hpp>
#include <opencv2/core/types.hpp>
DAS_DISABLE_WARNING_END

DAS_CORE_DEBUG_NS_BEGIN

enum class DebugAnnotationColor
{
    Green,
    Yellow,
    Red,
    Blue
};

struct DebugDrawBox
{
    Das::ExportInterface::DasRect rect{};
    DebugAnnotationColor          color{DebugAnnotationColor::Green};
    std::string                   label;
};

struct DebugImageSnapshot
{
    bool                                available{false};
    std::string                         image_status{"not_available"};
    cv::Mat                             bgr_image;
    Das::ExportInterface::DasImagePixelFormat pixel_format{
        Das::ExportInterface::DAS_PIXEL_FORMAT_UNKNOWN};
};

struct DebugImageWriteResult
{
    std::string image_status{"not_available"};
    std::string original_image_filename;
    std::string image_filename;
};

std::shared_ptr<DebugImageSnapshot> CaptureImageSnapshot(
    Das::ExportInterface::IDasImage* p_image);

DebugImageWriteResult SaveOriginalAndAnnotated(
    const std::string&                         step_name,
    std::shared_ptr<const DebugImageSnapshot>  snapshot,
    const std::vector<DebugDrawBox>&           annotations);

std::string BuildImageJson(const DebugImageWriteResult& result);

DasResult DrainImageJobs();
void ShutdownImageWorker();
bool IsImageWorkerRunningForTest();

DAS_CORE_DEBUG_NS_END

#endif // DAS_CORE_DEBUG_DEBUGIMAGEANNOTATOR_H
