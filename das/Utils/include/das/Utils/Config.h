#ifndef DAS_UTILS_CONFIG_H
#define DAS_UTILS_CONFIG_H

#include <das/DasConfig.h>

#define DAS_UTILS_NS_BEGIN                                                     \
    DAS_NS_BEGIN namespace Utils                                               \
    {

#define DAS_UTILS_NS_END                                                       \
    }                                                                          \
    namespace Core                                                             \
    {                                                                          \
        namespace Utils                                                        \
        {                                                                      \
            using namespace DAS::Utils;                                        \
        }                                                                      \
    }                                                                          \
    DAS_NS_END

#endif // DAS_UTILS_CONFIG_H
