#include <algorithm>
#include <atomic>
#include <das/Core/ForeignInterfaceHost/ErrorLensManager.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Expected.h>
#include <mutex>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_NS_ANONYMOUS_DETAILS_BEGIN

std::atomic<ErrorLensManager*> g_active_error_lens_manager = nullptr;

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

auto ReadIidsFromVector(
    Das::ExportInterface::IDasReadOnlyGuidVector* p_iid_vector)
    -> DAS::Utils::Expected<std::vector<DasGuid>>
{
    if (p_iid_vector == nullptr)
    {
        return tl::make_unexpected(DAS_E_INVALID_POINTER);
    }

    const auto get_iid_size_result = GetIidVectorSize(p_iid_vector);
    if (!get_iid_size_result)
    {
        return tl::make_unexpected(get_iid_size_result.error());
    }

    const auto           iid_size = get_iid_size_result.value();
    std::vector<DasGuid> result;
    result.reserve(iid_size);
    for (size_t i = 0; i < iid_size; ++i)
    {
        const auto get_iid_result = GetIidFromIidVector(p_iid_vector, i);
        if (!get_iid_result)
        {
            if (get_iid_result.error() == DAS_E_OUT_OF_RANGE)
            {
                DAS_CORE_LOG_WARN(
                    "Received DAS_E_OUT_OF_RANGE when calling IDasGuidVector::At(). "
                    "Pointer = {}. Size = {}. Index = {}.",
                    static_cast<void*>(p_iid_vector),
                    iid_size,
                    i);
                break;
            }
            return tl::make_unexpected(get_iid_result.error());
        }
        result.push_back(get_iid_result.value());
    }
    return result;
}

DAS_NS_ANONYMOUS_DETAILS_END

DasResult ErrorLensManager::OnPluginLoaded(
    const DasGuid&                plugin_guid,
    std::span<FeatureInfo* const> error_lens_features)
{
    if (error_lens_features.empty())
    {
        return DAS_S_OK;
    }

    std::vector<
        std::pair<DasPtr<PluginInterface::IDasErrorLens>, std::vector<DasGuid>>>
        registrations;

    for (auto* feat : error_lens_features)
    {
        if (feat == nullptr || !feat->interface_ptr)
        {
            continue;
        }

        DasPtr<PluginInterface::IDasErrorLens> lens;
        const auto query_result = feat->interface_ptr->QueryInterface(
            DasIidOf<PluginInterface::IDasErrorLens>(),
            reinterpret_cast<void**>(lens.Put()));
        if (DAS::IsFailed(query_result) || !lens)
        {
            DAS_CORE_LOG_WARN("Failed to QI IDasErrorLens from plugin feature");
            continue;
        }

        DasPtr<Das::ExportInterface::IDasReadOnlyGuidVector> supported_iids;
        const auto get_supported_result =
            lens->GetSupportedIids(supported_iids.Put());
        if (DAS::IsFailed(get_supported_result) || !supported_iids)
        {
            DAS_CORE_LOG_WARN(
                "Failed to get supported ErrorLens GUIDs from plugin feature");
            continue;
        }

        auto iids = Details::ReadIidsFromVector(supported_iids.Get());
        if (!iids)
        {
            return iids.error();
        }

        registrations.emplace_back(std::move(lens), std::move(iids.value()));
    }

    OnPluginUnloading(plugin_guid);

    for (auto& [lens, iids] : registrations)
    {
        const auto register_result = RegisterRoutes(
            &plugin_guid,
            {iids.data(), iids.size()},
            lens.Get());
        if (DAS::IsFailed(register_result))
        {
            return register_result;
        }
    }

    return DAS_S_OK;
}

DasResult ErrorLensManager::OnPluginUnloading(const DasGuid& plugin_guid)
{
    std::unique_lock lock{mutex_};

    const auto owner_it = plugin_routes_.find(plugin_guid);
    if (owner_it == plugin_routes_.end())
    {
        return DAS_S_OK;
    }

    for (const auto& guid : owner_it->second)
    {
        const auto route_it = routes_.find(guid);
        if (route_it != routes_.end()
            && route_it->second.plugin_guid == plugin_guid)
        {
            routes_.erase(route_it);
        }
    }

    plugin_routes_.erase(owner_it);
    DAS_CORE_LOG_INFO("Removed ErrorLens routes for plugin");
    return DAS_S_OK;
}

DasResult ErrorLensManager::Register(
    Das::ExportInterface::IDasReadOnlyGuidVector* p_iid_vector,
    PluginInterface::IDasErrorLens*               p_error_lens)
{
    DAS_UTILS_CHECK_POINTER(p_iid_vector)
    DAS_UTILS_CHECK_POINTER(p_error_lens)

    auto iids = Details::ReadIidsFromVector(p_iid_vector);
    if (!iids)
    {
        return iids.error();
    }

    return RegisterRoutes(
        nullptr,
        {iids.value().data(), iids.value().size()},
        p_error_lens);
}

DasResult ErrorLensManager::RegisterRoutes(
    const DasGuid*                  p_plugin_guid,
    std::span<const DasGuid>        guids,
    PluginInterface::IDasErrorLens* p_error_lens)
{
    DAS_UTILS_CHECK_POINTER(p_error_lens)

    std::unique_lock lock{mutex_};

    for (const auto& iid : guids)
    {
        const auto existing_route = routes_.find(iid);
        if (existing_route != routes_.end())
        {
            DAS_CORE_LOG_WARN(
                "Trying to register duplicate IDasErrorLens route. "
                "Operation will replace the previous route. Pointer = {}. Iid = {}.",
                static_cast<void*>(p_error_lens),
                iid);

            if (p_plugin_guid != nullptr)
            {
                auto previous_owner_it =
                    plugin_routes_.find(existing_route->second.plugin_guid);
                if (previous_owner_it != plugin_routes_.end())
                {
                    auto& previous_routes = previous_owner_it->second;
                    previous_routes.erase(
                        std::remove(
                            previous_routes.begin(),
                            previous_routes.end(),
                            iid),
                        previous_routes.end());
                }
            }
        }

        const auto owner =
            p_plugin_guid != nullptr ? *p_plugin_guid : DasGuid{};
        routes_[iid] =
            Route{owner, DasPtr<PluginInterface::IDasErrorLens>{p_error_lens}};

        if (p_plugin_guid != nullptr)
        {
            auto& owner_routes = plugin_routes_[*p_plugin_guid];
            if (std::find(owner_routes.begin(), owner_routes.end(), iid)
                == owner_routes.end())
            {
                owner_routes.push_back(iid);
            }
        }
    }

    return DAS_S_OK;
}

DasResult ErrorLensManager::FindInterface(
    const DasGuid&                   iid,
    PluginInterface::IDasErrorLens** pp_out_lens)
{
    DAS_UTILS_CHECK_POINTER(pp_out_lens)

    DasPtr<PluginInterface::IDasErrorLens> lens;
    {
        std::shared_lock lock{mutex_};
        if (const auto it = routes_.find(iid); it != routes_.end())
        {
            lens = it->second.lens;
        }
    }

    if (!lens)
    {
        return DAS_E_NO_INTERFACE;
    }

    *pp_out_lens = lens.Get();
    lens->AddRef();
    return DAS_S_OK;
}

auto ErrorLensManager::GetErrorMessage(
    const DasGuid&      iid,
    IDasReadOnlyString* locale_name,
    DasResult           error_code) const
    -> DAS::Utils::Expected<DasPtr<IDasReadOnlyString>>
{
    DasPtr<PluginInterface::IDasErrorLens> lens;
    {
        std::shared_lock lock{mutex_};
        if (const auto it = routes_.find(iid); it != routes_.end())
        {
            lens = it->second.lens;
        }
    }

    if (!lens)
    {
        return tl::make_unexpected(DAS_E_OUT_OF_RANGE);
    }

    DasPtr<IDasReadOnlyString> p_result{};
    const auto                 get_error_message_result =
        lens->GetErrorMessage(locale_name, error_code, p_result.Put());
    if (IsOk(get_error_message_result))
    {
        return p_result;
    }

    return tl::make_unexpected(get_error_message_result);
}

void SetActiveErrorLensManager(ErrorLensManager* p_manager)
{
    Details::g_active_error_lens_manager.store(
        p_manager,
        std::memory_order_release);
}

void ClearActiveErrorLensManager(ErrorLensManager* p_manager)
{
    auto* expected = p_manager;
    Details::g_active_error_lens_manager.compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_acq_rel);
}

ErrorLensManager* GetActiveErrorLensManager()
{
    return Details::g_active_error_lens_manager.load(std::memory_order_acquire);
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
