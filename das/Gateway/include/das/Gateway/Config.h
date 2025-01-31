#ifndef DAS_GATEWAY_CONFIG_H
#define DAS_GATEWAY_CONFIG_H

#include <das/Core/Exceptions/DasException.h>
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

extern decltype(&DAS::Core::Exceptions::ThrowDasExceptionEc)
    g_pfnThrowDasExceptionEc;

DAS_GATEWAY_NS_END

#define DAS_GATEWAY_THROW_IF_FAILED(...)                                       \
    if (const auto __result = __VA_ARGS__; ::Das::IsFailed(__result))          \
    {                                                                          \
        ::Das::Core::Exceptions::DasExceptionSourceInfo                        \
            __das_internal_source_location{__FILE__, __LINE__, DAS_FUNCTION};  \
        ::Das::Gateway::g_pfnThrowDasExceptionEc(                              \
            __result,                                                          \
            &__das_internal_source_location);                                  \
    }

#endif // DAS_GATEWAY_CONFIG_H
