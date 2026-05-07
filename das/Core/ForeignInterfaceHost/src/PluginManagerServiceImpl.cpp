#include <algorithm>
#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/Core/Debug/DebugDecorators.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/IDasCaptureManagerImpl.h>
#include <das/Core/ForeignInterfaceHost/IDasStringVectorImpl.h>
#include <das/Core/ForeignInterfaceHost/PluginManagerServiceImpl.h>
#include <das/Core/ForeignInterfaceHost/PluginScanner.h>
#include <das/Core/ForeignInterfaceHost/PluginZipExtractor.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/DasExport.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasCapture.h>
#include <das/_autogen/idl/abi/IDasComponent.h>
#include <das/_autogen/idl/wrapper/IDasTypeInfo.hpp>
#include <new>

// DasCore-internal: IDasJsonImpl creation from yyjson value
namespace Das::Core::Utils
{
    DasResult CreateDasJsonFromYyjson(
        const yyjson::value&             value,
        Das::ExportInterface::IDasJson** pp_out);
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace
{
    /**
     * @brief RAII inflight guard for per-plugin operation serialization.
     *
     * Acquires BOTH keys (guid and name) in deterministic sorted order
     * to prevent deadlock. Blocks in constructor until all keys are
     * available, removes keys and notifies waiters in destructor.
     * Exception-safe: destructor always releases acquired keys.
     *
     * @architecture HTTP config domain (Phase 52). Used by
     * InstallPluginPackageData and MarkPluginPackageForDeletion to
     * serialize same-GUID or same-name operations. Different GUID AND
     * different name operations proceed independently.
     */
    class InflightGuard
    {
    public:
        InflightGuard(
            std::mutex&                      mtx,
            std::condition_variable&         cv,
            std::unordered_set<std::string>& set,
            const std::string&               guid_key,
            const std::string&               name_key)
            : mutex_(mtx), cv_(cv), set_(set)
        {
            if (!guid_key.empty())
            {
                keys_.push_back("guid:" + guid_key);
            }
            if (!name_key.empty())
            {
                keys_.push_back("name:" + name_key);
            }
            // Sort to prevent deadlock: always acquire in same order
            std::sort(keys_.begin(), keys_.end());

            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(
                lock,
                [&]
                {
                    for (const auto& k : keys_)
                    {
                        if (set_.find(k) != set_.end())
                        {
                            return false;
                        }
                    }
                    return true;
                });
            for (const auto& k : keys_)
            {
                set_.insert(k);
            }
        }

        ~InflightGuard()
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& k : keys_)
                {
                    set_.erase(k);
                }
            }
            cv_.notify_all();
        }

        InflightGuard(const InflightGuard&) = delete;
        InflightGuard& operator=(const InflightGuard&) = delete;

    private:
        std::mutex&                      mutex_;
        std::condition_variable&         cv_;
        std::unordered_set<std::string>& set_;
        std::vector<std::string>         keys_;
    };

} // anonymous namespace

PluginManagerServiceImpl::PluginManagerServiceImpl(
    PluginManager&        mgr,
    std::filesystem::path plugin_dir)
    : mgr_(mgr), plugin_dir_{std::move(plugin_dir)}
{
}

uint32_t DAS_STD_CALL PluginManagerServiceImpl::AddRef()
{
    return ++ref_count_;
}

uint32_t DAS_STD_CALL PluginManagerServiceImpl::Release()
{
    auto count = --ref_count_;
    if (count == 0)
    {
        delete this;
    }
    return count;
}

DasResult DAS_STD_CALL
PluginManagerServiceImpl::QueryInterface(const DasGuid& iid, void** pp_out)
{
    if (pp_out == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    if (iid == DasIidOf<IDasBase>())
    {
        *pp_out = static_cast<IDasBase*>(this);
        AddRef();
        return DAS_S_OK;
    }

    if (iid == DasIidOf<IDasPluginManagerService>())
    {
        *pp_out = static_cast<IDasPluginManagerService*>(this);
        AddRef();
        return DAS_S_OK;
    }

    *pp_out = nullptr;
    return DAS_E_NO_INTERFACE;
}

DasResult PluginManagerServiceImpl::CreateComponent(
    const DasGuid* p_component_iid,
    IDasBase**     pp_out_component)
{
    if (pp_out_component == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    DAS_UTILS_CHECK_POINTER(p_component_iid)

    DAS::DasOutPtr<IDasBase> out_component(pp_out_component);

    DAS::DasPtr<Das::PluginInterface::IDasComponent> component;
    auto result = mgr_.GetComponentFactoryManager().CreateComponent(
        *p_component_iid,
        component.Put());
    if (DAS::IsFailed(result))
    {
        return result;
    }
    if (!component)
    {
        return DAS_E_INVALID_POINTER;
    }

    *out_component.Put() = static_cast<IDasBase*>(component.Get());
    out_component->AddRef();
    out_component.Keep();
    return DAS_S_OK;
}

DasResult PluginManagerServiceImpl::GetPluginSettingsFieldNames(
    const DasGuid*                           p_plugin_guid,
    Das::ExportInterface::IDasStringVector** pp_out) const
{
    DAS_UTILS_CHECK_POINTER(pp_out)
    DAS_UTILS_CHECK_POINTER(p_plugin_guid)

    DAS::DasOutPtr<Das::ExportInterface::IDasStringVector> result(pp_out);
    auto create_result = CreateIDasStringVector(result.Put());
    if (DAS::IsFailed(create_result))
    {
        return create_result;
    }

    auto* desc = mgr_.FindPluginPackageByGuid(*p_plugin_guid);
    if (!desc)
    {
        result.Keep();
        return DAS_S_OK;
    }

    for (const auto& setting : desc->settings_desc)
    {
        DAS::DasPtr<IDasReadOnlyString> field_name;
        auto                            cr = CreateIDasReadOnlyStringFromUtf8(
            setting.name.c_str(),
            field_name.Put());
        if (DAS::IsFailed(cr))
        {
            DAS_CORE_LOG_WARN(
                "Failed to create IDasReadOnlyString for field name: {}",
                setting.name);
            return cr;
        }
        result->PushBack(field_name.Get());
    }

    result.Keep();
    return DAS_S_OK;
}

DasResult PluginManagerServiceImpl::ScanInstalledPlugins(
    Das::ExportInterface::IDasJson** pp_out_plugins)
{
    DAS_UTILS_CHECK_POINTER(pp_out_plugins)

    auto descs = ScanPlugins(plugin_dir_);

    auto arr = Das::Utils::MakeYyjsonArray();
    auto arr_ref = arr.as_array();
    for (const auto& desc : descs)
    {
        if (arr_ref)
        {
            arr_ref->emplace_back(PluginPackageDescToJson(desc));
        }
    }

    using Das::Core::Utils::CreateDasJsonFromYyjson;
    return CreateDasJsonFromYyjson(arr, pp_out_plugins);
}

DasResult PluginManagerServiceImpl::InstallPluginPackage(
    IDasReadOnlyString* p_package_path)
{
    DAS_UTILS_CHECK_POINTER(p_package_path)
    const char* u8_path = nullptr;
    auto        result = p_package_path->GetUtf8(&u8_path);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    // TODO: InstallPlugin currently takes zip_data; the HTTP layer will
    // provide the package content. This method signature accepts a path
    // for future file-based installation support.
    (void)u8_path;
    return DAS_E_NO_IMPLEMENTATION;
}

DasResult PluginManagerServiceImpl::InstallPluginPackageData(
    const uint8_t* p_package_data,
    uint64_t       package_size)
{
    DAS_UTILS_CHECK_POINTER(p_package_data)
    if (package_size == 0)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    // Phase 1: Read identity from in-memory ZIP manifest BEFORE any
    // filesystem mutation. Reuses existing ZIP parsing infrastructure
    // (EnumerateZipEntries, local header, decompression).
    std::string plugin_guid;
    std::string plugin_name;
    auto        meta_result = ReadPluginManifestMetadataFromZip(
        std::string_view{
            reinterpret_cast<const char*>(p_package_data),
            static_cast<size_t>(package_size)},
        plugin_guid,
        plugin_name);
    if (DAS::IsFailed(meta_result))
    {
        DAS_CORE_LOG_ERROR(
            "InstallPluginPackageData: failed to read identity "
            "from zip manifest");
        return meta_result;
    }

    if (plugin_guid.empty() && plugin_name.empty())
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    // Phase 2: RAII inflight guard — acquires both guid and name keys
    // in sorted order, blocks until all are available.
    // Same GUID or same name operations serialize.
    InflightGuard guard(
        inflight_mutex_,
        inflight_cv_,
        inflight_plugin_ops_,
        plugin_guid,
        plugin_name);

    // Phase 3: Install (filesystem mutation under inflight guard scope).
    // InflightGuard destructor releases both keys and notifies waiters
    // on any exit path (including exceptions).
    return InstallPlugin(
        plugin_dir_,
        std::string_view{
            reinterpret_cast<const char*>(p_package_data),
            static_cast<size_t>(package_size)});
}

DasResult PluginManagerServiceImpl::MarkPluginPackageForDeletion(
    const DasGuid* p_package_guid)
{
    DAS_UTILS_CHECK_POINTER(p_package_guid)

    // Convert DasGuid to string for inflight key
    std::string guid_str = DasGuidToStdString(*p_package_guid);

    // Resolve plugin name from scanning installed plugins
    std::string plugin_name;
    auto        installed = ScanPlugins(plugin_dir_);
    for (const auto& info : installed)
    {
        if (info.guid == *p_package_guid)
        {
            plugin_name = info.name;
            break;
        }
    }

    // RAII inflight guard — acquires both guid and name keys
    // in sorted order, blocks until all are available.
    InflightGuard guard(
        inflight_mutex_,
        inflight_cv_,
        inflight_plugin_ops_,
        guid_str,
        plugin_name);

    return MarkForDeletion(plugin_dir_, *p_package_guid);
}

DasResult PluginManagerServiceImpl::SetHostExePath(
    IDasReadOnlyString* p_host_exe_path)
{
    DAS_UTILS_CHECK_POINTER(p_host_exe_path)
    const char* u8_path = nullptr;
    auto        result = p_host_exe_path->GetUtf8(&u8_path);
    if (DAS::IsFailed(result))
    {
        return result;
    }
    mgr_.SetHostExePath(std::string{u8_path});
    return DAS_S_OK;
}

DasResult PluginManagerServiceImpl::CreateCaptureManager(
    IDasReadOnlyString*                        p_environment_config,
    Das::ExportInterface::IDasCaptureManager** pp_out)
{
    if (pp_out == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    auto capture_features = mgr_.GetFeaturesByType(
        Das::PluginInterface::DAS_PLUGIN_FEATURE_CAPTURE_FACTORY);

    if (capture_features.empty())
    {
        DAS_CORE_LOG_WARN("No CAPTURE_FACTORY features found");
        return DAS_E_NOT_FOUND;
    }

    auto* capture_mgr = new CaptureManagerImpl();
    capture_mgr->ReserveInstanceContainer(capture_features.size());

    DasResult overall_result = DAS_S_OK;

    for (auto* feat : capture_features)
    {
        if (!feat->interface_ptr)
        {
            DAS_CORE_LOG_WARN("CAPTURE_FACTORY feature has null interface_ptr");
            continue;
        }

        DAS::DasPtr<Das::PluginInterface::IDasCaptureFactory> factory;
        auto qi_result = feat->interface_ptr->QueryInterface(
            DasIidOf<Das::PluginInterface::IDasCaptureFactory>(),
            reinterpret_cast<void**>(factory.Put()));
        if (DAS::IsFailed(qi_result) || !factory)
        {
            DAS_CORE_LOG_WARN(
                "Failed to QI IDasCaptureFactory from feature, result={}",
                qi_result);
            continue;
        }

        auto guid_str = DasGuidToStdString(feat->plugin_guid);

        // D-11: Obtain settings internally via PluginManager -> SettingsManager
        auto& settings_mgr = mgr_.GetSettingsManager();
        auto  settings_json = settings_mgr.GetPluginSettingsJson("0", guid_str);

        DAS::DasPtr<IDasReadOnlyString> plugin_config;
        if (!settings_json.is_null())
        {
            auto serialized = Das::Utils::SerializeYyjsonValue(settings_json);
            if (serialized)
            {
                const auto create_result = CreateIDasReadOnlyStringFromUtf8(
                    serialized->c_str(),
                    plugin_config.Put());
                if (DAS::IsFailed(create_result))
                {
                    DAS_CORE_LOG_WARN(
                        "Failed to create IDasReadOnlyString for plugin config, guid={}",
                        guid_str);
                }
            }
        }

        Das::PluginInterface::IDasCapture* p_raw_capture = nullptr;
        const auto create_result = factory->CreateInstance(
            p_environment_config,
            plugin_config.Get(),
            &p_raw_capture);

        if (DAS::IsFailed(create_result) || !p_raw_capture)
        {
            if (p_raw_capture)
            {
                p_raw_capture->Release();
            }
            DAS_CORE_LOG_WARN(
                "CaptureFactory::CreateInstance failed, result={}",
                create_result);

            // Get the factory's type name to identify which capture failed
            DAS::DasPtr<IDasReadOnlyString> factory_name;
            auto* type_info = static_cast<IDasTypeInfo*>(factory.Get());
            if (type_info)
            {
                type_info->GetRuntimeClassName(factory_name.Put());
            }
            if (!factory_name)
            {
                const auto create_name_result =
                    CreateIDasReadOnlyStringFromUtf8(
                        "unknown",
                        factory_name.Put());
                if (DAS::IsFailed(create_name_result))
                {
                    CreateNullDasString(factory_name.Put());
                }
            }

            CaptureManagerImpl::ErrorInfo error_info{};
            error_info.error_code = create_result;
            DAS::DasPtr<IDasReadOnlyString> p_error_msg;
            const auto                      create_error_msg_result =
                CreateIDasReadOnlyStringFromUtf8(
                    DAS_FMT_NS::format(
                        "Capture instance creation failed, result={}",
                        create_result)
                        .c_str(),
                    p_error_msg.Put());
            if (DAS::IsFailed(create_error_msg_result))
            {
                CreateNullDasString(p_error_msg.Put());
            }
            error_info.p_error_message = p_error_msg;
            capture_mgr->AddInstance(std::move(factory_name), error_info);
            overall_result = DAS_S_FALSE;
            continue;
        }

        DAS::DasPtr<IDasReadOnlyString> capture_name;
        auto* type_info = static_cast<IDasTypeInfo*>(p_raw_capture);
        if (type_info)
        {
            type_info->GetRuntimeClassName(capture_name.Put());
        }
        if (!capture_name)
        {
            const auto create_name_result =
                CreateIDasReadOnlyStringFromUtf8("unknown", capture_name.Put());
            if (DAS::IsFailed(create_name_result))
            {
                DAS_CORE_LOG_WARN(
                    "Failed to create fallback capture name, result={}",
                    create_name_result);
                CreateNullDasString(capture_name.Put());
            }
        }

        const char* p_capture_name_utf8 = "unknown";
        if (capture_name)
        {
            const char* p_resolved_name = nullptr;
            if (capture_name->GetUtf8(&p_resolved_name) >= 0
                && p_resolved_name)
            {
                p_capture_name_utf8 = p_resolved_name;
            }
        }

        auto* p_decorated_capture =
            Das::Core::Debug::MaybeDecorateCapture(
                p_raw_capture,
                p_capture_name_utf8);
        auto capture =
            DAS::DasPtr<Das::PluginInterface::IDasCapture>::Attach(
                p_decorated_capture);

        capture_mgr->AddInstance(std::move(capture_name), std::move(capture));
    }

    *pp_out = capture_mgr;
    capture_mgr->AddRef();

    return overall_result;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
