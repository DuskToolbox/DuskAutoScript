#include <DAS/_autogen/idl/abi/IDasErrorLens.h>
#include <boost/dll.hpp>
#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasException.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>
DAS_CORE_EXCEPTIONS_NS_BEGIN

static const auto FATAL_ERROR_MESSAGE = DAS_UTILS_STRINGUTILS_DEFINE_U8STR(
    "Can not get error message from error code. Fatal error happened!");

void ThrowDefaultDasException(DasResult error_code)
{
    throw DasException{error_code, FATAL_ERROR_MESSAGE, das_borrow_t{}};
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
                "| [{}][{}:{}] DasGetPredefinedErrorMessage failed. Error code = {}.",
                p_source_info->function,
                p_source_info->file,
                p_source_info->line,

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
                "| [{}][{}:{}] Operation failed. Error code = {}. Message = \"{}\".",
                p_source_info->function,
                p_source_info->file,
                p_source_info->line,
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
                "| [{}][{}:{}] DasGetErrorMessage failed. Error code = {}.",
                p_source_info->function,
                p_source_info->file,
                p_source_info->line,
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
                "| [{}][{}:{}] Operation failed. Error code = {}. Message = \"{}\".",
                p_source_info->function,
                p_source_info->file,
                p_source_info->line,
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
                "| [{}][{}:{}] DasGetErrorMessage failed. Error code = {}. ExMessage = \"{}\".",
                p_source_info->function,
                p_source_info->file,
                p_source_info->line,
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
                R"(|[{}][{}:{}] Operation failed. Error code = {}. Message = "{}". ExMessage = "{}".)",
                p_source_info->function,
                p_source_info->file,
                p_source_info->line,
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

// 新增：抛出 IDasException* 的函数
// 这个函数抛出 C++ 异常，但异常类型是 IDasException* 而不是 DasException
// 错误消息会在内部通过 DasGetPredefinedErrorMessage 查询
void ThrowDasExceptionPtr(
    DasResult               error_code,
    DasExceptionSourceInfo* p_source_info)
{
    DasPtr<IDasReadOnlyString> p_error_message{};
    const auto                 get_predefined_error_message_result =
        ::DasGetPredefinedErrorMessage(error_code, p_error_message.Put());

    std::string full_message;
    if (IsFailed(get_predefined_error_message_result))
    {
        if (p_source_info)
        {
            DAS_CORE_LOG_ERROR(
                "| [{}][{}:{}] DasGetPredefinedErrorMessage failed. Error code = {}.",
                p_source_info->function,
                p_source_info->file,
                p_source_info->line,
                get_predefined_error_message_result);
            full_message = DAS::fmt::format(
                "| [{}][{}:{}] Operation failed with error code = {}. (Failed to get error message)",
                p_source_info->function,
                p_source_info->file,
                p_source_info->line,
                error_code);
        }
        else
        {
            DAS_CORE_LOG_ERROR(
                "DasGetPredefinedErrorMessage failed. Error code = {}.",
                get_predefined_error_message_result);
            full_message = DAS::fmt::format(
                "Operation failed with error code = {}. (Failed to get error message)",
                error_code);
        }
    }
    else
    {
        if (p_source_info)
        {
            full_message = DAS::fmt::format(
                R"(|[{}][{}:{}] Operation failed. Error code = {}. Message = "{}".)",
                p_source_info->function,
                p_source_info->file,
                p_source_info->line,
                p_error_message,
                error_code);
        }
        else
        {
            full_message = DAS::fmt::format(
                R"(Operation failed. Error code = {}. Message = "{}".)",
                p_error_message,
                error_code);
        }
    }

    // 创建 IDasException* 并抛出
    IDasException* p_exception =
        DasCreateException(error_code, full_message.c_str());
    throw p_exception;
}

DAS_CORE_EXCEPTIONS_NS_END

// Global functions for exception string handle management
// These functions are NOT in DAS::Core::Exceptions namespace

DAS_C_API void CreateDasExceptionString(
    DasResult                  error_code,
    DasExceptionSourceInfo*    p_source_info,
    DasExceptionStringHandle** pp_out_handle)
{
    if (!pp_out_handle)
        return;

    // Query error message from IDasErrorLens
    DasPtr<IDasReadOnlyString> p_error_message{};
    const auto                 get_predefined_error_message_result =
        ::DasGetPredefinedErrorMessage(error_code, p_error_message.Put());

    // Get error message (use type info if available)
    std::string error_msg;
    if (p_type_info)
    {
        DasPtr<IDasReadOnlyString> p_error_message{};
        const auto                 result = ::DasGetErrorMessage(
            p_type_info,
            error_code,
            p_error_message.Put());
        error_msg = result == DAS_S_OK ? p_error_message->GetUtf8String()
                                       : "Unknown error";
    }
    else
    {
        error_msg = "Unknown error (no type info)";
    }

    // Log exception
    if (p_source_info)
    {
        DAS_CORE_LOG_ERROR(
            "| [{}][{}:{}] DasException thrown. Error code = {}",
            p_source_info->function,
            p_source_info->file,
            p_source_info->line,
            error_code);
    }

    // Allocate std::string and convert to opaque handle using std::launder
    std::string* p_string = new std::string{std::move(error_msg)};
    *pp_out_handle =
        std::launder(reinterpret_cast<DasExceptionStringHandle*>(p_string));
}

DAS_C_API void DeleteDasExceptionString(DasExceptionStringHandle* p_handle)
{
    if (!p_handle)
        return;

    // CRITICAL: Convert handle to std::string* BEFORE delete
    // The handle is a laundered std::string*, so we must launder it back to
    // std::string*
    std::string* p_string =
        std::launder(reinterpret_cast<std::string*>(p_handle));
    delete p_string;
}

extern "C" DAS_DLL_EXPORT void* ThrowDasExceptionEc;

DAS_C_API const char* GetDasExceptionStringCStr(
    DasExceptionStringHandle* p_handle)
{
    if (!p_handle)
        return nullptr;

    // CRITICAL: Convert handle to std::string* using std::launder
    // The handle is a laundered std::string*, so we must launder it back to
    // std::string*
    std::string* p_string =
        std::launder(reinterpret_cast<std::string*>(p_handle));
    return p_string->c_str();
}

DAS_C_API void DeleteDasExceptionString(DasExceptionStringHandle* p_handle)

    static_assert(
        sizeof(void*) == sizeof(&DAS::Core::Exceptions::ThrowDasExceptionEc),
        "Size not matched!");

DAS_DEFINE_VARIABLE(ThrowDasExceptionEc){
    reinterpret_cast<void*>(&DAS::Core::Exceptions::ThrowDasExceptionEc)};
