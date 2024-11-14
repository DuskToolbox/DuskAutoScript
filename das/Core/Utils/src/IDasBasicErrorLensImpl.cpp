#include "IDasBasicErrorLensImpl.h"

#include <das/Core/Logger/Logger.h>
#include <das/Utils/QueryInterface.hpp>

DAS_CORE_UTILS_NS_BEGIN

IDasBasicErrorLensImpl::IDasBasicErrorLensImpl(DasBasicErrorLensImpl& impl)
    : impl_{impl}
{
}

int64_t IDasBasicErrorLensImpl::AddRef() { return impl_.AddRef(); }

int64_t IDasBasicErrorLensImpl::Release() { return impl_.Release(); }

DasResult IDasBasicErrorLensImpl::QueryInterface(
    const DasGuid& iid,
    void**         pp_object)
{
    return Utils::QueryInterface<IDasBasicErrorLens>(this, iid, pp_object);
}

DasResult IDasBasicErrorLensImpl::GetSupportedIids(
    IDasReadOnlyGuidVector** pp_out_iids)
{
    return impl_.GetSupportedIids(pp_out_iids);
}

DasResult IDasBasicErrorLensImpl::GetErrorMessage(
    IDasReadOnlyString*  locale_name,
    DasResult            error_code,
    IDasReadOnlyString** pp_out_string)
{
    return impl_.GetErrorMessage(locale_name, error_code, pp_out_string);
}

DasResult IDasBasicErrorLensImpl::RegisterErrorMessage(
    IDasReadOnlyString* locale_name,
    DasResult           error_code,
    IDasReadOnlyString* p_explanation)
{
    return impl_.RegisterErrorMessage(locale_name, error_code, p_explanation);
}

DasResult IDasBasicErrorLensImpl::GetWritableSupportedIids(
    IDasGuidVector** pp_out_iids)
{
    return impl_.GetWritableSupportedIids(pp_out_iids);
}

IDasSwigBasicErrorLensImpl::IDasSwigBasicErrorLensImpl(
    DasBasicErrorLensImpl& impl)
    : impl_{impl}
{
}

int64_t IDasSwigBasicErrorLensImpl::AddRef() { return impl_.AddRef(); }

int64_t IDasSwigBasicErrorLensImpl::Release() { return impl_.Release(); }

DasRetSwigBase IDasSwigBasicErrorLensImpl::QueryInterface(const DasGuid& iid)
{
    return Utils::QueryInterface<IDasSwigBasicErrorLens>(this, iid);
}

DasRetReadOnlyGuidVector IDasSwigBasicErrorLensImpl::GetSupportedIids()
{
    return impl_.GetSupportedIids();
}

DasRetReadOnlyString IDasSwigBasicErrorLensImpl::GetErrorMessage(
    const DasReadOnlyString locale_name,
    DasResult               error_code)
{
    DasRetReadOnlyString       result{};
    DasPtr<IDasReadOnlyString> p_result{};
    result.error_code =
        impl_.GetErrorMessage(locale_name.Get(), error_code, p_result.Put());
    result.value = std::move(p_result);
    return result;
}

DasResult IDasSwigBasicErrorLensImpl::RegisterErrorMessage(
    DasReadOnlyString locale_name,
    DasResult         error_code,
    DasReadOnlyString error_message)
{
    return impl_.RegisterErrorMessage(
        locale_name.Get(),
        error_code,
        error_message.Get());
}

DasRetGuidVector IDasSwigBasicErrorLensImpl::GetWritableSupportedIids()
{
    return impl_.GetWritableSupportedIids();
}

int64_t DasBasicErrorLensImpl::AddRef() { return ref_counter_.AddRef(); }

int64_t DasBasicErrorLensImpl::Release() { return ref_counter_.Release(this); }

DasResult DasBasicErrorLensImpl::GetSupportedIids(
    IDasReadOnlyGuidVector** pp_out_iids)
{
    DAS_UTILS_CHECK_POINTER(pp_out_iids)

    *pp_out_iids = suppored_guid_vector_;

    return DAS_S_OK;
}

DAS_NS_ANONYMOUS_DETAILS_BEGIN

DasResult FindErrorMessage(
    const std::unordered_map<DasResult, DasPtr<IDasReadOnlyString>>&
                         error_message_map,
    DasResult            error_code,
    IDasReadOnlyString*& p_out_string)
{
    if (const auto error_message_it = error_message_map.find(error_code);
        error_message_it != error_message_map.end())
    {
        p_out_string = error_message_it->second.Get();
        p_out_string->AddRef();
        return DAS_S_OK;
    }
    return DAS_E_OUT_OF_RANGE;
}

DAS_NS_ANONYMOUS_DETAILS_END

DasResult DasBasicErrorLensImpl::GetErrorMessage(
    IDasReadOnlyString*  locale_name,
    DasResult            error_code,
    IDasReadOnlyString** out_string)
{
    DAS_UTILS_CHECK_POINTER(out_string)

    DasPtr locale_name_holder{locale_name};

    const auto locale_it = map_.find(locale_name_holder);
    if (locale_it != map_.end())
    {
        return Details::FindErrorMessage(
            locale_it->second,
            error_code,
            *out_string);
    }
    for (const auto& [loacle_name, error_message_map] : map_)
    {
        const auto fem_result = Details::FindErrorMessage(
            error_message_map,
            error_code,
            *out_string);

        if (fem_result == DAS_E_OUT_OF_RANGE)
        {
            continue;
        }

        return fem_result;
    }
    return DAS_S_OK;
}

DasResult DasBasicErrorLensImpl::RegisterErrorMessage(
    IDasReadOnlyString* locale_name,
    DasResult           error_code,
    IDasReadOnlyString* p_error_message)
{
    DasPtr locale_name_holder{locale_name};
    try
    {
        DasPtr error_message_holder = {p_error_message};
        map_[std::move(locale_name_holder)][error_code] = error_message_holder;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    return DAS_S_OK;
}

DasRetReadOnlyGuidVector DasBasicErrorLensImpl::GetSupportedIids()
{
    return {
        DAS_S_OK,
        DAS::DasPtr{
            static_cast<IDasSwigReadOnlyGuidVector*>(suppored_guid_vector_)}};
}

DasResult DasBasicErrorLensImpl::GetWritableSupportedIids(
    IDasGuidVector** pp_out_iids)
{
    DAS_UTILS_CHECK_POINTER(pp_out_iids)

    *pp_out_iids = suppored_guid_vector_;

    return DAS_S_OK;
}

DasRetGuidVector DasBasicErrorLensImpl::GetWritableSupportedIids()
{
    return {
        DAS_S_OK,
        {static_cast<IDasSwigGuidVector*>(suppored_guid_vector_)}};
}

DAS_CORE_UTILS_NS_END

DasResult CreateIDasBasicErrorLens(IDasBasicErrorLens** pp_out_error_lens)
{
    DAS_UTILS_CHECK_POINTER(pp_out_error_lens)

    try
    {
        const auto p_result =
            Das::MakeDasPtr<DAS::Core::Utils::DasBasicErrorLensImpl>();
        *pp_out_error_lens = *p_result;
        p_result->AddRef();
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasRetBasicErrorLens CreateIDasSwigBasicErrorLens()
{
    try
    {
        const auto p_result =
            Das::MakeDasPtr<DAS::Core::Utils::DasBasicErrorLensImpl>();

        return {DAS_S_OK, {static_cast<IDasSwigBasicErrorLens*>(*p_result)}};
    }
    catch (const std::bad_alloc&)
    {
        return {DAS_E_OUT_OF_MEMORY, nullptr};
    }
}
