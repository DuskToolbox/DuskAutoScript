#ifndef DAS_CORE_IMPL_CONFIG_H
#define DAS_CORE_IMPL_CONFIG_H

#include <das/DasConfig.h>

#define DAS_CORE_ORTWRAPPER_NS_BEGIN                                                 \
    DAS_NS_BEGIN namespace Core                                                \
    {                                                                          \
        namespace OrtWrapper                                                         \
        {

#define DAS_CORE_ORTWRAPPER_NS_END                                                   \
    }                                                                          \
    }                                                                          \
    DAS_NS_END

#endif // DAS_CORE_IMPL_CONFIG_H
