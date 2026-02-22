#ifndef DAS_CORE_EXCEPTIONS_TYPEERROR_H
#define DAS_CORE_EXCEPTIONS_TYPEERROR_H

#include <das/_autogen/idl/abi/DasJson.h>
#include <das/Core/Exceptions/Config.h>

DAS_CORE_EXCEPTIONS_NS_BEGIN

class TypeError : public std::runtime_error
{
    using Base = std::runtime_error;

public:
    TypeError(
        ExportInterface::DasType expected,
        ExportInterface::DasType actual);
};

DAS_CORE_EXCEPTIONS_NS_END

#endif // DAS_CORE_EXCEPTIONS_TYPEERROR_H
