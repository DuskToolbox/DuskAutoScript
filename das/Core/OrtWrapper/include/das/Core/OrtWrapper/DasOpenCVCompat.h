#ifndef DAS_CORE_ORTWRAPPER_DASOPENCVCOMPAT_H
#define DAS_CORE_ORTWRAPPER_DASOPENCVCOMPAT_H

#include <das/DasConfig.h>

DAS_DISABLE_WARNING_BEGIN

DAS_IGNORE_OPENCV_WARNING
#include <opencv2/core/version.hpp>
#if CV_VERSION_MAJOR >= 5
// opencv5 把计算几何函数（minAreaRect/boundingRect/contourArea/arcLength/
// getPerspectiveTransform/convexHull）从 imgproc 迁到 geometry 模块；
// 4.x 无此头，条件编译让旧版本走原路径零影响
#include <opencv2/geometry.hpp>
#endif

DAS_DISABLE_WARNING_END

#endif // DAS_CORE_ORTWRAPPER_DASOPENCVCOMPAT_H
