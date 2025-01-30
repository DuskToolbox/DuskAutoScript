#include <boost/dll.hpp>
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

void ThrowDefaultDasException(DasResult error_code)
{
    throw DasException{error_code, FATAL_ERROR_MESSAGE, borrow_t{}};
}

void ThrowDasExceptionEc(
    DasResult               error_code,
    DasExceptionSourceInfo* p_source_info)
{
    DasPtr<IDasReadOnlyString> p_error_message{};
    const auto                 get_predefined_error_message_result =
        ::DasGetPredefinedErrorMessage(error_code, p_error_message.Put());
    if (IsFailed(get_predefined_error_message_result))
    {
        if (p_source_info)
        {
            DAS_CORE_LOG_ERROR(
                "|[{}][{}][{}] DasGetPredefinedErrorMessage failed. Error code = {}.",
                p_source_info->file,
                p_source_info->line,
                p_source_info->function,
                get_predefined_error_message_result);
            ThrowDefaultDasException(get_predefined_error_message_result);
        }
        else
        {
            DAS_CORE_LOG_ERROR(
                "DasGetPredefinedErrorMessage failed. Error code = {}.",
                get_predefined_error_message_result);
            ThrowDefaultDasException(get_predefined_error_message_result);
        }
    }
    if (p_source_info)
    {
        throw DasException{
            error_code,
            DAS::fmt::format(
                "|[{}][{}][{}] Operation failed. Error code = {}. Message = \"{}\".",
                p_source_info->file,
                p_source_info->line,
                p_source_info->function,
                error_code,
                p_error_message)};
    }
    throw DasException{
        error_code,
        DAS::fmt::format(
            "Operation failed. Error code = {}. Message = \"{}\".",
            error_code,
            p_error_message)};
}

void ThrowDasException(
    DasResult               error_code,
    IDasTypeInfo*           p_type_info,
    DasExceptionSourceInfo* p_source_info)
{
    DasPtr<IDasReadOnlyString> p_error_message{};

    const auto get_error_message_result =
        ::DasGetErrorMessage(p_type_info, error_code, p_error_message.Put());
    if (IsFailed(get_error_message_result))
    {
        if (p_source_info)
        {
            DAS_CORE_LOG_ERROR(
                "|[{}][{}][{}] DasGetErrorMessage failed. Error code = {}.",
                p_source_info->file,
                p_source_info->line,
                p_source_info->function,
                get_error_message_result);
            ThrowDefaultDasException(get_error_message_result);
        }
        else
        {
            DAS_CORE_LOG_ERROR(
                "DasGetErrorMessage failed. Error code = {}.",
                get_error_message_result);
            ThrowDefaultDasException(get_error_message_result);
        }
    }

    if (p_source_info)
    {
        throw DasException{
            error_code,
            DAS::fmt::format(
                "|[{}][{}][{}] Operation failed. Error code = {}. Message = \"{}\".",
                p_source_info->file,
                p_source_info->line,
                p_source_info->function,
                p_error_message,
                error_code)};
    }
    throw DasException{
        error_code,
        DAS::fmt::format(
            "Operation failed. Error code = {}. Message = \"{}\".",
            p_error_message,
            error_code)};
}

void ThrowDasException(
    DasResult               error_code,
    IDasSwigTypeInfo*       p_type_info,
    DasExceptionSourceInfo* p_source_info)
{
    const auto internal_error_message =
        ::DasGetErrorMessage(p_type_info, error_code);
    const auto get_error_message_result =
        DAS::GetErrorCodeFrom(internal_error_message);
    if (IsFailed(get_error_message_result))
    {
        if (p_source_info)
        {
            DAS_CORE_LOG_ERROR(
                "|[{}][{}][{}] DasGetErrorMessage failed. Error code = {}.",
                p_source_info->file,
                p_source_info->line,
                p_source_info->function,
                get_error_message_result);
            ThrowDefaultDasException(get_error_message_result);
        }
        else
        {
            DAS_CORE_LOG_ERROR(
                "DasGetErrorMessage failed. Error code = {}.",
                get_error_message_result);
            ThrowDefaultDasException(get_error_message_result);
        }
    }

    if (p_source_info)
    {
        throw DasException{
            error_code,
            DAS::fmt::format(
                "|[{}][{}][{}] Operation failed. Error code = {}. Message = \"{}\".",
                p_source_info->file,
                p_source_info->line,
                p_source_info->function,
                internal_error_message.value,
                error_code)};
    }
    throw DasException{
        error_code,
        DAS::fmt::format(
            "Operation failed. Error code = {}. Message = \"{}\".",
            internal_error_message.value,
            error_code)};
}

void ThrowDasException(
    DasResult               error_code,
    const std::string&      ex_message,
    DasExceptionSourceInfo* p_source_info)
{
    DasPtr<IDasReadOnlyString> p_error_message{};
    const auto                 get_predefined_error_message_result =
        ::DasGetPredefinedErrorMessage(error_code, p_error_message.Put());
    if (IsFailed(get_predefined_error_message_result))
    {
        if (p_source_info)
        {
            DAS_CORE_LOG_ERROR(
                "|[{}][{}][{}] DasGetErrorMessage failed. Error code = {}. ExMessage = \"{}\".",
                p_source_info->file,
                p_source_info->line,
                p_source_info->function,
                get_predefined_error_message_result,
                ex_message);
            ThrowDefaultDasException(get_predefined_error_message_result);
        }
        else
        {
            DAS_CORE_LOG_ERROR(
                "DasGetErrorMessage failed. Error code = {}. ExMessage = \"{}\".",
                get_predefined_error_message_result,
                ex_message);
            ThrowDefaultDasException(get_predefined_error_message_result);
        }
    }

    if (p_source_info)
    {
        throw DasException{
            error_code,
            DAS::fmt::format(
                R"(|[{}][{}][{}] Operation failed. Error code = {}. Message = "{}". ExMessage = "{}".)",
                p_source_info->file,
                p_source_info->line,
                p_source_info->function,
                p_error_message,
                error_code,
                ex_message)};
    }
    throw DasException{
        error_code,
        DAS::fmt::format(
            R"(Operation failed. Error code = {}. Message = "{}". ExMessage = "{}".)",
            p_error_message,
            error_code,
            ex_message)};
}

DAS_CORE_EXCEPTIONS_NS_END

BOOST_DLL_ALIAS(
    DAS::Core::Exceptions::ThrowDasExceptionEc,
    ThrowDasExceptionEc)
