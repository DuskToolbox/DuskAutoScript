#include <DAS/_autogen/idl/abi/IDasErrorLens.h>
#include <boost/dll.hpp>
#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/DasException.hpp>
#include <das/DasExport.h>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>

// Win32 风格 opaque handle 实现（在全局命名空间）
namespace Das::Exception::Impl
{
    // 无用类型别名，用于避免 std::launder 警告
    typedef struct DasExceptionStringHandle_* DasExceptionStringHandleUnused;

    struct DasExceptionStringHandleImpl
    {
        std::unique_ptr<std::string> string_;
    };
}

// Global functions for exception string handle management
// These functions are NOT in DAS::Core::Exceptions namespace

// Global functions for exception string handle management
// These functions are NOT in DAS::Core::Exceptions namespace

// 创建错误消息并返回 opaque handle
DAS_C_API void CreateDasExceptionString(
    DasResult                  error_code,
    DasExceptionSourceInfo*    p_source_info,
    DasExceptionStringHandle** pp_out_handle)
{
    if (!pp_out_handle)
        return;

    // 生成错误消息
    DAS::DasPtr<IDasReadOnlyString> p_error_message{};
    const auto                      result =
        ::DasGetPredefinedErrorMessage(error_code, p_error_message.Put());
    DasReadOnlyString das_error_message{std::move(p_error_message)};

    std::string error_msg;
    if (DAS::IsFailed(result))
    {
        error_msg = "Unknown error";
    }
    else
    {
        error_msg = das_error_message.GetUtf8();
    }

    // 添加源位置信息
    if (p_source_info)
    {
        DAS_CORE_LOG_ERROR(
            "| [{}][{}:{}] DasException thrown. Error code = {}.",
            p_source_info->function,
            p_source_info->file,
            p_source_info->line,
            error_code);
        error_msg = DAS::fmt::format(
            "| [{}][{}:{}] DasException thrown. Error code = {}. Message = \"{}\".",
            p_source_info->function,
            p_source_info->file,
            p_source_info->line,
            error_code,
            error_msg);
    }

    // 创建 opaque handle（Win32 风格）
    auto* p_impl = new Das::Exception::Impl::DasExceptionStringHandleImpl{
        std::make_unique<std::string>(std::move(error_msg))};
    *pp_out_handle =
        std::launder(reinterpret_cast<DasExceptionStringHandle*>(p_impl));
}

// 删除 opaque handle（Win32 风格）
// Debug 模式下对 double delete assert 失败
DAS_C_API void DeleteDasExceptionString(DasExceptionStringHandle* p_handle)
{
    // 安全删除，无需类型转换
    auto* p_impl =
        reinterpret_cast<Das::Exception::Impl::DasExceptionStringHandleImpl*>(
            p_handle);
    delete p_impl;
}

// 获取 opaque handle 中的字符串（Win32 风格）
DAS_C_API const char* GetDasExceptionStringCStr(
    DasExceptionStringHandle* p_handle)
{
    if (!p_handle)
        return nullptr;

    // 安全访问，无需类型转换
    auto* p_impl =
        reinterpret_cast<Das::Exception::Impl::DasExceptionStringHandleImpl*>(
            p_handle);
    return p_impl->string_->c_str();
}

// 辅助函数：抛出默认异常
void ThrowDefaultDasException(DasResult error_code)
{
    throw DasException{
        error_code,
        "Can not get error message from error code. Fatal error happened!",
        das_borrow_t{}};
}

// 辅助函数：从错误码和源位置抛出异常（全局命名空间）
void ThrowDasExceptionEc(
    DasResult               error_code,
    DasExceptionSourceInfo* p_source_info)
{
    DAS::DasPtr<IDasReadOnlyString> p_error_message{};
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

// 辅助函数：从错误码、类型信息和源位置抛出异常（全局命名空间）
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

// 辅助函数：从错误消息和源位置抛出异常（全局命名空间）
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
                "| [{}][{}:{}] DasGetPredefinedErrorMessage failed. Error code = {}. ExMessage = \"{}\".",
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
                "DasGetPredefinedErrorMessage failed. Error code = {}. ExMessage = \"{}\".",
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

// 函数指针导出（向后兼容）
extern "C" DAS_DLL_EXPORT void* ThrowDasExceptionEc;

DAS_DEFINE_VARIABLE(ThrowDasExceptionEc){
    reinterpret_cast<void*>(&ThrowDasExceptionEc)};
