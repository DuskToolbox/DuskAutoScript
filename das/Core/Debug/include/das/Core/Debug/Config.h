#ifndef DAS_CORE_DEBUG_CONFIG_H
#define DAS_CORE_DEBUG_CONFIG_H

#include <das/DasConfig.h>

#define DAS_CORE_DEBUG_NS_BEGIN                                                \
    DAS_NS_BEGIN namespace Core                                                \
    {                                                                          \
        namespace Debug                                                        \
        {

#define DAS_CORE_DEBUG_NS_END                                                  \
    }                                                                          \
    }                                                                          \
    DAS_NS_END

#endif // DAS_CORE_DEBUG_CONFIG_H
