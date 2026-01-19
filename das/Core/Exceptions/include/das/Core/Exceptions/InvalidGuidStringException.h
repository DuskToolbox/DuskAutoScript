#ifndef DAS_CORE_EXCEPTIONS_INVALIDGUIDSTRINGEXCEPTION_H
#define DAS_CORE_EXCEPTIONS_INVALIDGUIDSTRINGEXCEPTION_H

#include <das/Core/Exceptions/Config.h>
#include <das/DasException.hpp>
#include <string_view>

DAS_CORE_EXCEPTIONS_NS_BEGIN

struct InvalidGuidStringException : public DasException
{
    explicit InvalidGuidStringException(
        const std::string_view invalid_guid_string);
};

struct InvalidGuidStringSizeException : public DasException
{
    explicit InvalidGuidStringSizeException(const std::size_t string_size);
};

DAS_CORE_EXCEPTIONS_NS_END

#endif // DAS_CORE_EXCEPTIONS_INVALIDGUIDSTRINGEXCEPTION_H
