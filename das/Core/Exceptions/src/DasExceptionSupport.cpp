#include <DAS/_autogen/idl/abi/IDasErrorLens.h>
#include <boost/dll.hpp>
#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/DasException.hpp>
#include <das/DasExport.h>
#include <das/Utils/CommonUtils.hpp>
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

    class DasExceptionStringImpl final : public IDasExceptionString
    {
    public:
        uint32_t DAS_STD_CALL AddRef() override { return counter_.AddRef(); }
        uint32_t DAS_STD_CALL Release() override { return counter_.Release(); }

        explicit DasExceptionStringImpl(std::string&& msg)
            : error_msg_(std::move(msg))
        {
        }

        DAS_IMPL
        QueryInterface(const DasGuid& iid, void** pp_object) override
        {
            if (!pp_object)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DasIidOf<IDasBase>())
            {
                *pp_object = static_cast<IDasBase*>(this);
            }
            if (iid == DasIidOf<IDasExceptionString>())
            {
                *pp_object = static_cast<IDasExceptionString*>(this);
            }
            return DAS_E_NO_INTERFACE;
        }

        DAS_IMPL GetU8(const char** pp_out_string) override
        {
            *pp_out_string = error_msg_.c_str();
            return DAS_S_OK;
        }

    private:
        Utils::RefCounter<DasExceptionStringImpl> counter_;
        std::string                               error_msg_;
    };
}

// 创建错误消息并返回 opaque handle
DAS_C_API void CreateDasExceptionString(
    DasResult               error_code,
    DasExceptionSourceInfo* p_source_info,
    IDasExceptionString**   pp_out_handle)
{
    if (!pp_out_handle)
    {
        return;
    }

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

    DAS::Utils::SetResult(
        DAS::MakeDasPtr<Das::Exception::Impl::DasExceptionStringImpl>(
            std::move(error_msg)),
        pp_out_handle);
}

// 4参数版本：直接使用自定义消息创建错误字符串
DAS_C_API void CreateDasExceptionStringWithMessage(
    DasResult               error_code,
    DasExceptionSourceInfo* p_source_info,
    const char*             message,
    IDasExceptionString**   pp_out_handle)
{
    if (!pp_out_handle)
    {
        return;
    }

    if (DAS::IsFailed(error_code))
    {
        DAS::DasPtr<IDasReadOnlyString> p_error_message{};
        std::string                     formatted_msg =
            DAS::IsOk(
                DasGetPredefinedErrorMessage(error_code, p_error_message.Put()))
                                    ? std::format(
                      "Exception: Code={}, Message='{}', PredefinedErrorMessage = '{}', File={}, Line={}, Function={}",
                      static_cast<int>(error_code),
                      message,
                      [&p_error_message]
                      {
                          const char* result{""};
                          p_error_message->GetUtf8(&result);
                          return result;
                      }(),
                      p_source_info != nullptr ? p_source_info->file : "null",
                      p_source_info != nullptr ? p_source_info->line : 0,
                      p_source_info != nullptr ? p_source_info->function
                                                                   : "null")
                                    : std::format(
                      "Exception: Code={}, Message='{}', File={}, Line={}, Function={}",
                      static_cast<int>(error_code),
                      message,
                      p_source_info != nullptr ? p_source_info->file : "null",
                      p_source_info != nullptr ? p_source_info->line : 0,
                      p_source_info != nullptr ? p_source_info->function
                                                                   : "null");

        DAS::Utils::SetResult(
            DAS::MakeDasPtr<Das::Exception::Impl::DasExceptionStringImpl>(
                std::move(formatted_msg)),
            pp_out_handle);
    }
    else
    {
        *pp_out_handle = nullptr;
    }
}

DAS_C_API void CreateDasExceptionStringWithTypeInfo(
    DasResult               error_code,
    DasExceptionSourceInfo* p_source_info,
    IDasTypeInfo*           p_type_info,
    IDasExceptionString**   pp_out_handle)
{
    if (!pp_out_handle)
    {
        return;
    }

    DAS::DasPtr<IDasReadOnlyString> p_error_message{};
    DasGetErrorMessage(p_type_info, error_code, p_error_message.Put());

    if (DAS::IsFailed(error_code))
    {
        std::string formatted_msg = std::format(
            "Exception: Code={}, Message='{}', File={}, Line={}, Function={}",
            static_cast<int>(error_code),
            [&p_error_message]
            {
                const char* result{""};
                p_error_message->GetUtf8(&result);
                return result;
            }(),
            p_source_info != nullptr ? p_source_info->file : "null",
            p_source_info != nullptr ? p_source_info->line : 0,
            p_source_info != nullptr ? p_source_info->function : "null");

        DAS::Utils::SetResult(
            DAS::MakeDasPtr<Das::Exception::Impl::DasExceptionStringImpl>(
                std::move(formatted_msg)),
            pp_out_handle);
    }
    else
    {
        *pp_out_handle = nullptr;
    }
}
