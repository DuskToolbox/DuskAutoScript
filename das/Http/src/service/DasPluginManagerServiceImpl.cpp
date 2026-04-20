#include "DasPluginManagerServiceImpl.h"

#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/CommonUtils.hpp>

namespace Das::Http
{

    DasPluginManagerServiceImpl::DasPluginManagerServiceImpl(
        IDasPluginManagerService& plugin_manager_service,
        IDasSettingsService&      settings_service)
        : plugin_manager_service_(plugin_manager_service),
          settings_service_(settings_service)
    {
    }

    DasResult DasPluginManagerServiceImpl::CreateComponent(
        const DasGuid&                        iid,
        Das::PluginInterface::IDasComponent** pp_out_component)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_component)

        auto& factory_mgr =
            plugin_manager_service_.GetComponentFactoryManager();

        return factory_mgr.CreateComponent(iid, pp_out_component);
    }

    DasResult DasPluginManagerServiceImpl::CreateCaptureManager(
        IDasReadOnlyString*                        p_environment_config,
        Das::ExportInterface::IDasCaptureManager** pp_out_capture_manager)
    {
        DAS_UTILS_CHECK_POINTER(pp_out_capture_manager)
        DAS_UTILS_CHECK_POINTER(p_environment_config)

        auto capture_features = plugin_manager_service_.GetFeaturesByType(
            Das::PluginInterface::DAS_PLUGIN_FEATURE_CAPTURE_FACTORY);

        if (capture_features.empty())
        {
            DAS_CORE_LOG_WARN("No CAPTURE_FACTORY features found");
            return DAS_E_NOT_FOUND;
        }

        auto* capture_mgr =
            new Das::Core::ForeignInterfaceHost::CaptureManagerImpl();

        capture_mgr->ReserveInstanceContainer(capture_features.size());

        DasResult overall_result = DAS_S_OK;

        for (auto* feat : capture_features)
        {
            if (!feat->interface_ptr)
            {
                DAS_CORE_LOG_WARN(
                    "CAPTURE_FACTORY feature has null interface_ptr");
                continue;
            }

            // QI 获取 IDasCaptureFactory
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

            // 从 SettingsManager 按工厂所属插件 GUID 获取 plugin_config
            const auto guid_str =
                Das::Core::ForeignInterfaceHost::DasGuidToStdString(
                    feat->plugin_guid);

            // profile_id 硬编码 "0" (v1.2)
            auto settings_json =
                settings_service_.GetPluginSettingsJson("0", guid_str);

            // 将 nlohmann::json 转为 IDasReadOnlyString 传递给工厂
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

            // env_config 透传
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

                Das::Core::ForeignInterfaceHost::CaptureManagerImpl::ErrorInfo
                    error_info{};
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

            // 获取 Capture 实例名称
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

            capture_mgr->AddInstance(
                std::move(capture_name),
                std::move(capture));
        }

        *pp_out_capture_manager = capture_mgr;
        capture_mgr->AddRef();

        return overall_result;
    }

} // namespace Das::Http
