#include <das/DasConfig.h>
#include <das/DasString.hpp>

#include "IDasVariantVector.h"

DAS_DEFINE_RET_POINTER(
    DasRetIDasVariantVector,
    Das::ExportInterface::IDasVariantVector);

// 这里为了保证SWIG兼容先不嵌套命名空间
DAS_SWIG_NS_BEGIN

DAS_API void DasLogError(IDasReadOnlyString* das_string);

DAS_API void DasLogWarning(IDasReadOnlyString* das_string);

DAS_API void DasLogInfo(IDasReadOnlyString* das_string);

DAS_API DasRetIDasVariantVector CreateDasRetIDasVariantVector();

DAS_SWIG_NS_END