#ifndef DAS_CORE_OCVWRAPPER_CONFIG_H
#define DAS_CORE_OCVWRAPPER_CONFIG_H

#include <das/DasConfig.h>
#include <das/_autogen/idl/abi/IDasImage.h>

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

inline auto ToMat(ExportInterface::DasRect rect) -> cv::Rect
{
    return {rect.x, rect.y, rect.width, rect.height};
}

inline auto IsValidClipRect(ExportInterface::DasRect rect, int cols, int rows)
    -> bool
{
    if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0)
    {
        return false;
    }

    const int64_t right =
        static_cast<int64_t>(rect.x) + static_cast<int64_t>(rect.width);
    const int64_t bottom =
        static_cast<int64_t>(rect.y) + static_cast<int64_t>(rect.height);

    return right <= cols && bottom <= rows;
}

DAS_CORE_OCVWRAPPER_NS_END

#endif // DAS_CORE_OCVWRAPPER_CONFIG_H
