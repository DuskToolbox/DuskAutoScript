#ifndef ASR_CORE_EXCEPTIONS_ASREXCEPTION_H
#define ASR_CORE_EXCEPTIONS_ASREXCEPTION_H

#include <AutoStarRail/Core/Exceptions/Config.h>
#include <AutoStarRail/IAsrTypeInfo.h>
#include <exception>
#include <variant>

ASR_CORE_EXCEPTIONS_NS_BEGIN

class borrow_t
{
};

class AsrException final : public std::exception
{
    AsrResult                              error_code_;
    std::variant<const char*, std::string> common_string_;

    using Base = std::runtime_error;

    static void ThrowDefault(AsrResult error_code);

    AsrException(AsrResult error_code, std::string&& string);
    AsrException(AsrResult error_code, const char* p_string, borrow_t);

public:
    static void Throw(AsrResult error_code);
    static void Throw(AsrResult error_code, IAsrTypeInfo* p_type_info);
    static void Throw(AsrResult error_code, IAsrSwigTypeInfo* p_type_info);
    static void Throw(AsrResult error_code, const std::string& ex_message);
    // ex message 支持p_type_info

    [[nodiscard]]
    const char* what() const noexcept override;
    auto        GetErrorCode() const noexcept -> AsrResult;
};

ASR_CORE_EXCEPTIONS_NS_END

#endif // ASR_CORE_EXCEPTIONS_ASREXCEPTION_H
