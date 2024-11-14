#ifndef DAS_CORE_EXCEPTIONS_DASEXCEPTION_H
#define DAS_CORE_EXCEPTIONS_DASEXCEPTION_H

#include <das/Core/Exceptions/Config.h>
#include <das/IDasTypeInfo.h>
#include <exception>
#include <variant>

DAS_CORE_EXCEPTIONS_NS_BEGIN

class borrow_t
{
};

class DasException final : public std::exception
{
    DasResult                              error_code_;
    std::variant<const char*, std::string> common_string_;

    using Base = std::runtime_error;

    static void ThrowDefault(DasResult error_code);

    DasException(DasResult error_code, std::string&& string);
    DasException(DasResult error_code, const char* p_string, borrow_t);

public:
    static void Throw(DasResult error_code);
    static void Throw(DasResult error_code, IDasTypeInfo* p_type_info);
    static void Throw(DasResult error_code, IDasSwigTypeInfo* p_type_info);
    static void Throw(DasResult error_code, const std::string& ex_message);
    // ex message 支持p_type_info

    [[nodiscard]]
    const char* what() const noexcept override;
    auto        GetErrorCode() const noexcept -> DasResult;
};

DAS_CORE_EXCEPTIONS_NS_END

#endif // DAS_CORE_EXCEPTIONS_DASEXCEPTION_H
