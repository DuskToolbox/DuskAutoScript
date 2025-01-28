#ifndef DAS_GATEWAY_CONFIG_H
#define DAS_GATEWAY_CONFIG_H

#include <das/DasConfig.h>
#include <das/DasString.hpp>

#define DAS_GATEWAY_NS_BEGIN                                                   \
    DAS_NS_BEGIN namespace Gateway                                             \
    {

#define DAS_GATEWAY_NS_END                                                     \
    }                                                                          \
    }

DAS_GATEWAY_NS_BEGIN

extern decltype(&::CreateIDasReadOnlyStringFromUtf8)
    g_pfnCreateIDasReadOnlyStringFromUtf8;

DAS_GATEWAY_NS_END

#endif // DAS_GATEWAY_CONFIG_H
