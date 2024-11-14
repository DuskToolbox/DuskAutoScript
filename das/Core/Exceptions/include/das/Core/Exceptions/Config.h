/**
 * @file Config.h
 * @author Dusk_NM02
 * @brief Notice: Every exception class should return an UTF-8 string.
 *
 * @copyright Copyright (c) 2023 Dusk.
 *
 */

#ifndef DAS_CORE_EXCEPTION_CONFIG_H
#define DAS_CORE_EXCEPTION_CONFIG_H

#include <das/DasConfig.h>
#include <stdexcept>
#include <cstdint>

#define DAS_CORE_EXCEPTIONS_NS_BEGIN                                           \
    DAS_NS_BEGIN                                                               \
    namespace Core                                                             \
    {                                                                          \
        namespace Exceptions                                                   \
        {

#define DAS_CORE_EXCEPTIONS_NS_END                                             \
    }                                                                          \
    using namespace DAS::Core::Exceptions;                                     \
    }                                                                          \
    DAS_NS_END

#endif // DAS_CORE_EXCEPTION_CONFIG_H
