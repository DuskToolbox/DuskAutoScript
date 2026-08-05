#ifndef DAS_DEBUG_CONFIG_H
#define DAS_DEBUG_CONFIG_H

#include <das/DasConfig.h>

// Das::Debug namespace 宏。与 Das::Core 平级（DAS_NS_BEGIN 开 Das）。
#define DAS_DEBUG_NS_BEGIN                                                     \
    DAS_NS_BEGIN namespace Debug                                               \
    {
#define DAS_DEBUG_NS_END                                                       \
    }                                                                          \
    DAS_NS_END

#endif // DAS_DEBUG_CONFIG_H
