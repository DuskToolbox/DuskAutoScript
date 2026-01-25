#ifndef DAS_GATEWAY_CONFIG_H
#define DAS_GATEWAY_CONFIG_H

#include <das/DasApi.h>
#include <das/DasConfig.h>

#define DAS_GATEWAY_NS_BEGIN                                                   \
    DAS_NS_BEGIN namespace Gateway                                             \
    {

#define DAS_GATEWAY_NS_END                                                     \
    }                                                                          \
    }

DAS_GATEWAY_NS_BEGIN

[[nodiscard]]
decltype(&::ParseDasJsonFromString) GetParseDasJsonFromStringFunction();

DAS_GATEWAY_NS_END

#endif // DAS_GATEWAY_CONFIG_H
