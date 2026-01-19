#include "ErrorLensImpl.h"
#include "PluginImpl.h"

#include <DAS/_autogen/idl/abi/DasLogger.h>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>
#include <das/DasApi.h>

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

AdbCaptureErrorLens::AdbCaptureErrorLens() { DAS::AdbCaptureAddRef(); }

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

    const auto& error_code_map = map_.at(p_default_locale_name);
    if (const auto it = error_code_map.find(error_code);
        it != error_code_map.end())
    {
        return Details::ReturnPointerInMap(it, out_string);
    }

    const auto error_message =
        DAS::fmt::format("No explanation for error code {}", error_code);
    DasPtr<IDasReadOnlyString> error_message_ptr{};
    ::CreateIDasReadOnlyStringFromUtf8(
        error_message.c_str(),
        error_message_ptr.Put());
    *out_string = error_message_ptr.Get();
    return DAS_S_OK;
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

DAS_DEFINE_VARIABLE(
    AdbCaptureErrorLens::error_code_not_found_explanation_generator) =
    +[](DasResult error_code, IDasReadOnlyString** pp_out_explanation)
{
    constexpr auto& template_string = DAS_UTILS_STRINGUTILS_DEFINE_U8STR(
        "No explanation found for error code {} .");

    auto result = DAS::fmt::format(template_string, error_code);
    ::CreateIDasReadOnlyStringFromUtf8(result.c_str(), pp_out_explanation);
    return result;
};

DAS_NS_END