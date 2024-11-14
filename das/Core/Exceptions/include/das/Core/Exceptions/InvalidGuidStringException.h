#ifndef DAS_CORE_EXCEPTIONS_INVALIDGUIDSTRINGEXCEPTION_H
#define DAS_CORE_EXCEPTIONS_INVALIDGUIDSTRINGEXCEPTION_H

#include <das/Core/Exceptions/Config.h>
#include <string_view>

DAS_CORE_EXCEPTIONS_NS_BEGIN

struct InvalidGuidStringException : public std::runtime_error
{
    using Base = std::runtime_error;
    explicit InvalidGuidStringException(const std::string_view invalid_guid_string);
};

struct InvalidGuidStringSizeException : public std::runtime_error
{
    using Base = std::runtime_error;
    explicit InvalidGuidStringSizeException(const std::size_t string_size);
};

DAS_CORE_EXCEPTIONS_NS_END

#endif // DAS_CORE_EXCEPTIONS_INVALIDGUIDSTRINGEXCEPTION_H
