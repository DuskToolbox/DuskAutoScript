#ifndef DAS_CORE_EXCEPTIONS_TYPEERROR_H
#define DAS_CORE_EXCEPTIONS_TYPEERROR_H

#include <das/Core/Exceptions/Config.h>
#include <das/ExportInterface/DasJson.h>

DAS_CORE_EXCEPTIONS_NS_BEGIN

class TypeError : public std::runtime_error
{
    using Base = std::runtime_error;
public:
    TypeError(const DasType expected, const DasType actual);

};

DAS_CORE_EXCEPTIONS_NS_END

#endif // DAS_CORE_EXCEPTIONS_TYPEERROR_H
