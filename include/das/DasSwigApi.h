#include <das/DasConfig.h>
#include <das/DasString.hpp>
#include <das/IDasBase.h>

#include "IDasVariantVector.h"

DAS_DEFINE_RET_POINTER(
    DasRetIDasVariantVector,
    Das::ExportInterface::IDasVariantVector);

DAS_DEFINE_RET_POINTER(DasRetIDasBase, IDasBase);

// 这里为了保证SWIG兼容先不嵌套命名空间
DAS_SWIG_NS_BEGIN

#include "DasCoreApi.generated.h"

DAS_SWIG_NS_END
