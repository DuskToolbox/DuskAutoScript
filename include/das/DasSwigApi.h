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

DAS_API void DasLogError(DasReadOnlyString das_string);

DAS_API void DasLogWarning(DasReadOnlyString das_string);

DAS_API void DasLogInfo(DasReadOnlyString das_string);

DAS_API DasRetIDasVariantVector CreateDasRetIDasVariantVector();

DAS_API DasRetIDasBase QueryMainProcessInterface(const DasGuid& iid);

DAS_API DasRetIDasBase QueryMainProcessInterfaceByName(const char* name);

DAS_SWIG_NS_END
