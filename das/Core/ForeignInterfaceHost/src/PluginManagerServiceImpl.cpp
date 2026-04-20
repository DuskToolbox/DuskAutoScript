#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/IDasCaptureManagerImpl.h>
#include <das/Core/ForeignInterfaceHost/IDasStringVectorImpl.h>
#include <das/Core/ForeignInterfaceHost/PluginManagerServiceImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/SettingsManager/SettingsManager.h>
#include <das/DasExport.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/abi/IDasCapture.h>
#include <das/_autogen/idl/wrapper/IDasTypeInfo.hpp>
#include <new>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

PluginManagerServiceImpl::PluginManagerServiceImpl(PluginManager& mgr)
    : mgr_(mgr)
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
    const DasGuid& iid,
    void**         pp_out)
{
    if (pp_out == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    return mgr_.GetComponentFactoryManager().CreateComponent(
        iid,
        reinterpret_cast<Das::PluginInterface::IDasComponent**>(pp_out));
}

DasResult PluginManagerServiceImpl::GetPluginSettingsFieldNames(
    const DasGuid&                           plugin_guid,
    Das::ExportInterface::IDasStringVector** pp_out) const
{
    DAS_UTILS_CHECK_POINTER(pp_out)

    Das::ExportInterface::IDasStringVector* raw_result = nullptr;
    auto create_result = CreateIDasStringVector(&raw_result);
    if (DAS::IsFailed(create_result))
    {
        return create_result;
    }

    auto* desc = mgr_.FindPluginPackageByGuid(plugin_guid);
    if (!desc)
    {
        // Return empty vector, not an error
        *pp_out = raw_result;
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
            continue;
        }
        raw_result->PushBack(field_name.Get());
    }

    *pp_out = raw_result;
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
            auto       config_str = settings_json.dump();
            const auto create_result = CreateIDasReadOnlyStringFromUtf8(
                config_str.c_str(),
                plugin_config.Put());
            if (DAS::IsFailed(create_result))
            {
                DAS_CORE_LOG_WARN(
                    "Failed to create IDasReadOnlyString for plugin config, guid={}",
                    guid_str);
            }
        }

        DAS::DasPtr<Das::PluginInterface::IDasCapture> capture;
        const auto create_result = factory->CreateInstance(
            p_environment_config,
            plugin_config.Get(),
            capture.Put());

        if (DAS::IsFailed(create_result) || !capture)
        {
            DAS_CORE_LOG_WARN(
                "CaptureFactory::CreateInstance failed, result={}",
                create_result);

            CaptureManagerImpl::ErrorInfo error_info{};
            error_info.error_code = create_result;
            std::string error_msg = DAS_FMT_NS::format(
                "Capture instance creation failed, result={}",
                create_result);
            DAS::DasPtr<IDasReadOnlyString> p_error_msg;
            CreateIDasReadOnlyStringFromUtf8(
                error_msg.c_str(),
                p_error_msg.Put());
            error_info.p_error_message = p_error_msg;
            capture_mgr->AddInstance(error_info);
            overall_result = DAS_S_FALSE;
            continue;
        }

        DAS::DasPtr<IDasReadOnlyString> capture_name;
        auto* type_info = static_cast<IDasTypeInfo*>(capture.Get());
        if (type_info)
        {
            type_info->GetRuntimeClassName(capture_name.Put());
        }
        if (!capture_name)
        {
            CreateIDasReadOnlyStringFromUtf8("unknown", capture_name.Put());
        }

        capture_mgr->AddInstance(std::move(capture_name), std::move(capture));
    }

    *pp_out = capture_mgr;
    capture_mgr->AddRef();

    return overall_result;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

DAS_C_API DasResult CreateDasPluginManagerService(
    Das::Core::ForeignInterfaceHost::PluginManager& mgr,
    IDasPluginManagerService**                      pp_out)
{
    if (pp_out == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    try
    {
        auto* impl =
            new Das::Core::ForeignInterfaceHost::PluginManagerServiceImpl(mgr);
        impl->AddRef();
        *pp_out = impl;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}
