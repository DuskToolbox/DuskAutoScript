#ifndef ASR_CORE_EXCEPTIONS_TYPEERROR_H
#define ASR_CORE_EXCEPTIONS_TYPEERROR_H

#include <AutoStarRail/Core/Exceptions/Config.h>
#include <AutoStarRail/ExportInterface/AsrJson.h>

ASR_CORE_EXCEPTIONS_NS_BEGIN

class TypeError : public std::runtime_error
{
    using Base = std::runtime_error;
public:
    TypeError(const AsrType expected, const AsrType actual);

};

ASR_CORE_EXCEPTIONS_NS_END

#endif // ASR_CORE_EXCEPTIONS_TYPEERROR_H
