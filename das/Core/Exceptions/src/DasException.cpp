#include <das/Core/Exceptions/DasException.h>
#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/PluginInterface/IDasErrorLens.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>

DAS_CORE_EXCEPTIONS_NS_BEGIN

static const auto FATAL_ERROR_MESSAGE = DAS_UTILS_STRINGUTILS_DEFINE_U8STR(
    "Can not get error message from error code. Fatal error happened!");

void DasException::ThrowDefault(DasResult error_code)
{
    throw DasException{error_code, FATAL_ERROR_MESSAGE, borrow_t{}};
}

DasException::DasException(DasResult error_code, std::string&& string)
    : error_code_{error_code}, common_string_{std::move(string)}
{
}

DasException::DasException(DasResult error_code, const char* p_string, borrow_t)
    : error_code_{error_code}, common_string_{p_string}
{
}

void DasException::Throw(DasResult error_code)
{
    DasPtr<IDasReadOnlyString> p_error_message{};
    const auto                 get_predefined_error_message_result =
        ::DasGetPredefinedErrorMessage(error_code, p_error_message.Put());
    if (IsFailed(get_predefined_error_message_result))
    {
        DAS_CORE_LOG_ERROR(
            "DasGetPredefinedErrorMessage failed. Error code = {}.",
            get_predefined_error_message_result);
        ThrowDefault(get_predefined_error_message_result);
    }

    throw DasException{
        error_code,
        DAS::fmt::format(
            "Operation failed. Error code = {}. message = \"{}\".",
            p_error_message,
            error_code)};
}

void DasException::Throw(DasResult error_code, IDasTypeInfo* p_type_info)
{
    DasPtr<IDasReadOnlyString> p_error_message{};

    const auto get_error_message_result =
        ::DasGetErrorMessage(p_type_info, error_code, p_error_message.Put());
    if (IsFailed(get_error_message_result))
    {
        DAS_CORE_LOG_ERROR(
            "DasGetErrorMessage failed. Error code = {}.",
            get_error_message_result);
        ThrowDefault(get_error_message_result);
    }

    throw DasException{
        error_code,
        DAS::fmt::format(
            "Operation failed. Error code = {}. message = \"{}\".",
            p_error_message,
            error_code)};
}

void DasException::Throw(DasResult error_code, IDasSwigTypeInfo* p_type_info)
{
    const auto internal_error_message =
        ::DasGetErrorMessage(p_type_info, error_code);
    const auto get_error_message_result =
        DAS::GetErrorCodeFrom(internal_error_message);
    if (IsFailed(get_error_message_result))
    {
        DAS_CORE_LOG_ERROR(
            "DasGetErrorMessage failed. Error code = {}.",
            get_error_message_result);
        ThrowDefault(get_error_message_result);
    }

    throw DasException{
        error_code,
        DAS::fmt::format(
            "Operation failed. Error code = {}. message = \"{}\".",
            internal_error_message.value,
            error_code)};
}

void DasException::Throw(DasResult error_code, const std::string& ex_message)
{
    DasPtr<IDasReadOnlyString> p_error_message{};
    const auto                 get_predefined_error_message_result =
        ::DasGetPredefinedErrorMessage(error_code, p_error_message.Put());
    if (IsFailed(get_predefined_error_message_result))
    {
        DAS_CORE_LOG_ERROR(
            "DasGetPredefinedErrorMessage failed. Error code = {}. ExMessage = \"{}\".",
            get_predefined_error_message_result,
            ex_message);
        ThrowDefault(get_predefined_error_message_result);
    }

    throw DasException{
        error_code,
        DAS::fmt::format(
            "Operation failed. Error code = {}. Message = \"{}\". ExMessage = \"{}\".",
            p_error_message,
            error_code,
            ex_message)};
}

const char* DasException::what() const noexcept
{
    try
    {
        return std::visit(
            Utils::overload_set{
                [](const char* result) { return result; },
                [](const std::string& result) { return result.c_str(); }},
            common_string_);
    }
    catch (const std::exception& ex)
    {
        return ex.what();
    }
}

auto DasException::GetErrorCode() const noexcept -> DasResult
{
    return error_code_;
}

DAS_CORE_EXCEPTIONS_NS_END
