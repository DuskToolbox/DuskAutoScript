#include "IDasBasicErrorLensImpl.h"

#include <das/Core/Logger/Logger.h>
#include <das/Utils/CommonUtils.hpp>

DAS_CORE_UTILS_NS_BEGIN

DasResult DasBasicErrorLensImpl::GetSupportedIids(
    IDasReadOnlyGuidVector** pp_out_iids)
{
    DAS_UTILS_CHECK_POINTER(pp_out_iids)

    *pp_out_iids = &suppored_guid_vector_;

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

DasResult DasBasicErrorLensImpl::GetWritableSupportedIids(
    IDasGuidVector** pp_out_iids)
{
    DAS_UTILS_CHECK_POINTER(pp_out_iids)

    *pp_out_iids = &suppored_guid_vector_;

    return DAS_S_OK;
}

DAS_CORE_UTILS_NS_END

DasResult CreateIDasBasicErrorLens(
    DAS::PluginInterface::IDasBasicErrorLens** pp_out_error_lens)
{
    DAS_UTILS_CHECK_POINTER(pp_out_error_lens)

    try
    {
        const auto p_result = DAS::Core::Utils::DasBasicErrorLensImpl::Make();
        DAS::Utils::SetResult(p_result, pp_out_error_lens);
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}
