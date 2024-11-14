#ifndef DAS_CORE_OCVWRAPPER_CONFIG_H
#define DAS_CORE_OCVWRAPPER_CONFIG_H

#include <das/DasConfig.h>
#include <das/ExportInterface/IDasImage.h>

DAS_DISABLE_WARNING_BEGIN

DAS_IGNORE_OPENCV_WARNING
#include <opencv2/core/types.hpp>

DAS_DISABLE_WARNING_END

#define DAS_CORE_OCVWRAPPER_NS_BEGIN                                           \
    DAS_NS_BEGIN namespace Core                                                \
    {                                                                          \
        namespace OcvWrapper                                                   \
        {

#define DAS_CORE_OCVWRAPPER_NS_END                                             \
    }                                                                          \
    }                                                                          \
    DAS_NS_END

DAS_CORE_OCVWRAPPER_NS_BEGIN

inline auto ToMat(DasRect rect) -> cv::Rect
{
    return {rect.x, rect.y, rect.width, rect.height};
}

DAS_CORE_OCVWRAPPER_NS_END

#endif // DAS_CORE_OCVWRAPPER_CONFIG_H
