#ifndef DAS_GATEWAY_CONFIG_H
#define DAS_GATEWAY_CONFIG_H

#include <das/DasApi.h>
#include <das/DasConfig.h>
#include <das/DasException.hpp>
#include <das/DasString.hpp>

#define DAS_GATEWAY_NS_BEGIN                                                   \
    DAS_NS_BEGIN namespace Gateway                                             \
    {

#define DAS_GATEWAY_NS_END                                                     \
    }                                                                          \
    }

DAS_GATEWAY_NS_BEGIN

// Type aliases for function pointers
using DasExceptionSourceInfo_t = DasExceptionSourceInfo;
using CreateDasExceptionString_t =
    void (*)(DasResult, DasExceptionSourceInfo_t*, DasExceptionStringHandle**);
using ParseDasJsonFromString_t =
    void (*)(const char*, DasExceptionStringHandle*);

[[nodiscard]]
CreateDasExceptionString_t GetCreateDasExceptionStringFunction();

[[nodiscard]]
ParseDasJsonFromString_t GetParseDasJsonFromStringFunction();

DAS_GATEWAY_NS_END

#endif // DAS_GATEWAY_CONFIG_H
