#include <das/Core/ForeignInterfaceHost/ErrorLensManager.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Expected.h>
#include <das/_autogen/idl/iids/OfficialIids.h>
#include <unordered_set>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_NS_ANONYMOUS_DETAILS_BEGIN

auto GetIidVectorSize(
    Das::ExportInterface::IDasReadOnlyGuidVector* p_iid_vector)
    -> DAS::Utils::Expected<size_t>
{
    size_t     iid_size{};
    const auto get_iid_size_result = p_iid_vector->Size(&iid_size);
    if (!IsOk(get_iid_size_result))
    {
        DasPtr<IDasReadOnlyString> p_error_message{};
        ::DasGetPredefinedErrorMessage(
            get_iid_size_result,
            p_error_message.Put());
        auto final_message = DAS_FMT_NS::format(
            "Error happened in class IDasGuidVector. Pointer = {}. "
            "Error code = {}. Error message = \"{}\".",
            static_cast<void*>(p_iid_vector),
            get_iid_size_result,
            *p_error_message);
        DAS_LOG_ERROR(final_message.c_str());
        return tl::make_unexpected(get_iid_size_result);
    }
    return iid_size;
}

auto GetIidFromIidVector(
    Das::ExportInterface::IDasReadOnlyGuidVector* p_iid_vector,
    size_t iid_index) -> DAS::Utils::Expected<DasGuid>
{
    DasGuid    iid{DasIidOf<IDasBase>()};
    const auto get_iid_result = p_iid_vector->At(iid_index, &iid);
    if (!IsOk(get_iid_result))
    {
        DasPtr<IDasReadOnlyString> p_error_message{};
        ::DasGetPredefinedErrorMessage(get_iid_result, p_error_message.Put());
        DAS_CORE_LOG_ERROR(
            "Error happened in class IDasGuidVector. Pointer = {}. "
            "Error code = {}. Error message = \"{}\".",
            static_cast<void*>(p_iid_vector),
            get_iid_result,
            *p_error_message);
        return tl::make_unexpected(get_iid_result);
    }
    return iid;
}

DAS_NS_ANONYMOUS_DETAILS_END

DasResult ErrorLensManager::Register(
    Das::ExportInterface::IDasReadOnlyGuidVector* p_iid_vector,
    PluginInterface::IDasErrorLens*               p_error_lens)
{
    const auto get_iid_size_result = Details::GetIidVectorSize(p_iid_vector);
    if (!get_iid_size_result)
    {
        return get_iid_size_result.error();
    }
    const auto iid_size = get_iid_size_result.value();
    // try to use all iids to register IDasErrorLens instance.
    for (size_t i = 0; i < iid_size; ++i)
    {
        const auto get_iid_from_iid_vector_result =
            Details::GetIidFromIidVector(p_iid_vector, i);
        if (!get_iid_from_iid_vector_result)
        {
            if (get_iid_size_result.error() == DAS_E_OUT_OF_RANGE)
            {
                DAS_CORE_LOG_WARN(
                    "Received DAS_E_OUT_OF_RANGE when calling IDasIidVector::At()."
                    "Pointer = {}. Size = {}. Index = {}.",
                    static_cast<void*>(p_iid_vector),
                    iid_size,
                    i);
                break;
            }
            return get_iid_from_iid_vector_result.error();
        }
        const auto& g_official_iids = Das::_autogen::g_official_iids;
        const auto& iid = get_iid_from_iid_vector_result.value();
        if (g_official_iids.find(iid) != g_official_iids.end())
        {
            if (map_.count(iid) == 1)
            {
                DAS_CORE_LOG_WARN(
                    "Trying to register duplicate IDasErrorLens "
                    "instance. Operation ignored."
                    "Pointer = {}. Iid = {}.",
                    static_cast<void*>(p_error_lens),
                    iid);
            }
            // register IDasErrorLens instance.
            map_[iid] = {p_error_lens};
        }
    }
    return DAS_S_OK;
}

DasResult ErrorLensManager::FindInterface(
    const DasGuid&                   iid,
    PluginInterface::IDasErrorLens** pp_out_lens)
{
    DAS_UTILS_CHECK_POINTER(pp_out_lens)
    if (const auto it = map_.find(iid); it != map_.end())
    {
        const auto& p_factory = it->second;
        *pp_out_lens = p_factory.Get();
        p_factory->AddRef();
        return DAS_S_OK;
    }
    return DAS_E_NO_INTERFACE;
}

auto ErrorLensManager::GetErrorMessage(
    const DasGuid&      iid,
    IDasReadOnlyString* locale_name,
    DasResult           error_code) const
    -> DAS::Utils::Expected<DasPtr<IDasReadOnlyString>>
{
    if (const auto it = map_.find(iid); it != map_.end())
    {
        DasPtr<IDasReadOnlyString> p_result{};
        const auto get_error_message_result = it->second->GetErrorMessage(
            locale_name,
            error_code,
            p_result.Put());
        if (IsOk(get_error_message_result))
        {
            return p_result;
        }

        return tl::make_unexpected(get_error_message_result);
    }
    return tl::make_unexpected(DAS_E_OUT_OF_RANGE);
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END