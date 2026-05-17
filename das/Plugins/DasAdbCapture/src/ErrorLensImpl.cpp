#include "ErrorLensImpl.h"
#include "AdbCaptureImpl.h"
#include "PluginImpl.h"

#include <das/DasApi.h>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/DasLogger.h>

DAS_NS_BEGIN

DAS_NS_ANONYMOUS_DETAILS_BEGIN

template <class T, class Out>
DasResult ReturnPointerInMap(T& it, Out** pp_out_object)
{
    auto p_result = it->second;
    p_result->AddRef();
    if constexpr (std::is_pointer_v<decltype(p_result)>)
    {
        *pp_out_object = p_result;
    }
    else
    {
        *pp_out_object = p_result.Get();
    }
    return DAS_S_OK;
}

DAS_NS_ANONYMOUS_DETAILS_END

AdbCaptureErrorLens::AdbCaptureErrorLens()
{
    DAS::AdbCaptureAddRef();
    AddSupportedIid(DasIidOf<AdbCapture>());

    const auto register_default_message =
        [this](DasResult error_code, const char* message)
    {
        DasPtr<IDasReadOnlyString> message_ptr;
        const auto                 create_result =
            ::CreateIDasReadOnlyStringFromUtf8(message, message_ptr.Put());
        if (IsFailed(create_result))
        {
            DAS_LOG_ERROR("Failed to create AdbCapture ErrorLens message");
            return;
        }
        RegisterErrorCode(error_code, p_default_locale_name, message_ptr);
    };

    register_default_message(
        CAPTURE_DATA_TOO_LESS,
        "ADB capture data is shorter than expected.");
    register_default_message(
        UNSUPPORTED_COLOR_FORMAT,
        "ADB capture color format is not supported.");
}

AdbCaptureErrorLens::~AdbCaptureErrorLens() { DAS::AdbCaptureRelease(); }

DasResult AdbCaptureErrorLens::GetSupportedIids(
    ExportInterface::IDasReadOnlyGuidVector** pp_out_iids)
{
    DasPtr<ExportInterface::IDasGuidVector> p_iids{};
    if (const auto error_code =
            ::CreateIDasGuidVector(iids_.data(), iids_.size(), p_iids.Put());
        IsFailed(error_code))
    {
        const auto error_message = DAS_FMT_NS::format(
            "Create IDasGuidVector failed. Error code = {}.",
            error_code);
        DAS_LOG_ERROR(error_message.c_str());
        return error_code;
    }

    return p_iids->ToConst(pp_out_iids);
}

DasResult AdbCaptureErrorLens::GetErrorMessage(
    IDasReadOnlyString*  locale_name,
    DasResult            error_code,
    IDasReadOnlyString** out_string)
{
    DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(out_string);
    *out_string = nullptr;

    DasPtr locale_name_ptr{locale_name};
    if (const auto locale_it = map_.find(locale_name_ptr);
        locale_it != map_.end())
    {
        const auto& error_code_map = locale_it->second;
        if (const auto it = error_code_map.find(error_code);
            it != error_code_map.end())
        {
            return Details::ReturnPointerInMap(it, out_string);
        }
    }

    if (const auto default_locale_it = map_.find(p_default_locale_name);
        default_locale_it != map_.end())
    {
        const auto& error_code_map = default_locale_it->second;
        if (const auto it = error_code_map.find(error_code);
            it != error_code_map.end())
        {
            return Details::ReturnPointerInMap(it, out_string);
        }
    }

    return DAS_E_OUT_OF_RANGE;
}

DasResult AdbCaptureErrorLens::RegisterErrorCode(
    const DasResult            error_code,
    DasPtr<IDasReadOnlyString> locale_name,
    DasPtr<IDasReadOnlyString> p_explanation)
{
    map_[locale_name][error_code] = p_explanation;
    return DAS_S_OK;
}

DasResult AdbCaptureErrorLens::AddSupportedIid(const DasGuid& iid)
{
    try
    {
        iids_.push_back(iid);
    }
    catch (std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    return DAS_S_OK;
}

DAS_DEFINE_VARIABLE(AdbCaptureErrorLens::p_default_locale_name) = []()
{
    constexpr auto& default_locale_name =
        DAS_UTILS_STRINGUTILS_DEFINE_U8STR("en");
    DasPtr<IDasString> result{};
    ::CreateIDasStringFromUtf8(default_locale_name, result.Put());
    return result;
}();

DAS_NS_END
