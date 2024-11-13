#include <AutoStarRail/Core/Exceptions/AsrException.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/AsrStringImpl.h>
#include <AutoStarRail/Core/Logger/Logger.h>
#include <AutoStarRail/PluginInterface/IAsrErrorLens.h>
#include <AutoStarRail/Utils/CommonUtils.hpp>
#include <AutoStarRail/Utils/StringUtils.h>
#include <AutoStarRail/Utils/fmt.h>

ASR_CORE_EXCEPTIONS_NS_BEGIN

static const auto FATAL_ERROR_MESSAGE = ASR_UTILS_STRINGUTILS_DEFINE_U8STR(
    "Can not get error message from error code. Fatal error happened!");

void AsrException::ThrowDefault(AsrResult error_code)
{
    throw AsrException{error_code, FATAL_ERROR_MESSAGE, borrow_t{}};
}

AsrException::AsrException(AsrResult error_code, std::string&& string)
    : error_code_{error_code}, common_string_{std::move(string)}
{
}

AsrException::AsrException(AsrResult error_code, const char* p_string, borrow_t)
    : error_code_{error_code}, common_string_{p_string}
{
}

void AsrException::Throw(AsrResult error_code)
{
    AsrPtr<IAsrReadOnlyString> p_error_message{};
    const auto                 get_predefined_error_message_result =
        ::AsrGetPredefinedErrorMessage(error_code, p_error_message.Put());
    if (IsFailed(get_predefined_error_message_result))
    {
        ASR_CORE_LOG_ERROR(
            "AsrGetPredefinedErrorMessage failed. Error code = {}.",
            get_predefined_error_message_result);
        ThrowDefault(get_predefined_error_message_result);
    }

    throw AsrException{
        error_code,
        ASR::fmt::format(
            "Operation failed. Error code = {}. message = \"{}\".",
            p_error_message,
            error_code)};
}

void AsrException::Throw(AsrResult error_code, IAsrTypeInfo* p_type_info)
{
    AsrPtr<IAsrReadOnlyString> p_error_message{};

    const auto get_error_message_result =
        ::AsrGetErrorMessage(p_type_info, error_code, p_error_message.Put());
    if (IsFailed(get_error_message_result))
    {
        ASR_CORE_LOG_ERROR(
            "AsrGetErrorMessage failed. Error code = {}.",
            get_error_message_result);
        ThrowDefault(get_error_message_result);
    }

    throw AsrException{
        error_code,
        ASR::fmt::format(
            "Operation failed. Error code = {}. message = \"{}\".",
            p_error_message,
            error_code)};
}

void AsrException::Throw(AsrResult error_code, IAsrSwigTypeInfo* p_type_info)
{
    const auto internal_error_message =
        ::AsrGetErrorMessage(p_type_info, error_code);
    const auto get_error_message_result =
        ASR::GetErrorCodeFrom(internal_error_message);
    if (IsFailed(get_error_message_result))
    {
        ASR_CORE_LOG_ERROR(
            "AsrGetErrorMessage failed. Error code = {}.",
            get_error_message_result);
        ThrowDefault(get_error_message_result);
    }

    throw AsrException{
        error_code,
        ASR::fmt::format(
            "Operation failed. Error code = {}. message = \"{}\".",
            internal_error_message.value,
            error_code)};
}

void AsrException::Throw(AsrResult error_code, const std::string& ex_message)
{
    AsrPtr<IAsrReadOnlyString> p_error_message{};
    const auto                 get_predefined_error_message_result =
        ::AsrGetPredefinedErrorMessage(error_code, p_error_message.Put());
    if (IsFailed(get_predefined_error_message_result))
    {
        ASR_CORE_LOG_ERROR(
            "AsrGetPredefinedErrorMessage failed. Error code = {}. ExMessage = \"{}\".",
            get_predefined_error_message_result,
            ex_message);
        ThrowDefault(get_predefined_error_message_result);
    }

    throw AsrException{
        error_code,
        ASR::fmt::format(
            "Operation failed. Error code = {}. Message = \"{}\". ExMessage = \"{}\".",
            p_error_message,
            error_code,
            ex_message)};
}

const char* AsrException::what() const noexcept
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

auto AsrException::GetErrorCode() const noexcept -> AsrResult
{
    return error_code_;
}

ASR_CORE_EXCEPTIONS_NS_END
