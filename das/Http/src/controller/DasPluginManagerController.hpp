#ifndef DAS_HTTP_CONTROLLER_DASPLUGINMANAGERCONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_DASPLUGINMANAGERCONTROLLER_HPP

#include "Config.h"
#include "../component/DasInitializePluginManagerCallback.h"
#include "das/ExportInterface/DasLogger.h"
#include "das/ExportInterface/IDasPluginManager.h"
#include "das/ExportInterface/IDasTaskScheduler.h"
#include "das/IDasBase.h"
#include "das/Utils/fmt.h"
#include "dto/Profile.hpp"
#include "dto/Settings.hpp"
#include "component/Helper.hpp"
#include "beast/Request.hpp"
#include "beast/JsonUtils.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Das::Http
{

class DasPluginManagerController
{
    DAS::DasPtr<IDasPluginManager>      p_plugin_manager_{};
    DAS::DasPtr<IDasPluginManagerForUi> p_plugin_manager_for_ui_{};

    static DasResult SetGlobalSchedulerJsonState(IDasReadOnlyString& profile_id)
    {
        DAS::DasPtr<IDasProfile> p_profile_{};
        if (const auto find_result =
                ::FindIDasProfile(&profile_id, p_profile_.Put());
            DAS::IsFailed(find_result))
        {
            const char* profile_id_string;
            profile_id.GetUtf8(&profile_id_string);
            const auto message = DAS_FMT_NS::format(
                "Find profile failed. Id = {}.",
                profile_id_string);
            DAS_LOG_ERROR(message.c_str());
            return find_result;
        }
        DAS::DasPtr<IDasJsonSetting> p_scheduler_state{};
        if (const auto get_result = p_profile_->GetJsonSettingProperty(
                DAS_PROFILE_PROPERTY_SCHEDULER_STATE,
                p_scheduler_state.Put());
            DAS::IsFailed(get_result))
        {
            const char* profile_id_string;
            profile_id.GetUtf8(&profile_id_string);
            const auto message = DAS_FMT_NS::format(
                "Get scheduler state failed. Profile id = {}.",
                profile_id_string);
            return get_result;
        }

        return SetIDasTaskSchedulerJsonState(p_scheduler_state.Get());
    }

    static DasResult CreatePluginManager(
        IDasReadOnlyGuidVector*  p_guid_vector,
        IDasPluginManager**      pp_out_plugin_manager,
        IDasPluginManagerForUi** pp_out_plugin_manager_for_ui)
    {
        DAS::DasPtr<IDasGuidVector> p_empty_guids{};
        ::CreateIDasGuidVector(nullptr, 0, p_empty_guids.Put());
        DAS::DasPtr<IDasReadOnlyGuidVector> p_const_empty_guids{};
        p_empty_guids->ToConst(p_const_empty_guids.Put());
        DAS::DasPtr<IDasInitializeIDasPluginManagerWaiter> p_waiter{};
        const auto p_callback = Das::MakeDasPtr<DasInitializePluginManagerCallback>();
        const auto error_code = ::InitializeIDasPluginManager(
            p_guid_vector,
            p_callback.Get(),
            p_waiter.Put());

        if (DAS::IsFailed(error_code))
        {
            const auto create_message = DAS_FMT_NS::format(
                "InitializeIDasPluginManager return {}.",
                error_code);
            DAS_LOG_ERROR(create_message.c_str());
            return error_code;
        }

        const auto create_message = DAS_FMT_NS::format(
            "InitializeIDasPluginManager return {}.",
            error_code);
        DAS_LOG_INFO(create_message.c_str());

        p_waiter->Wait();

        const auto wait_message =
            DAS_FMT_NS::format("Wait return {}.", error_code);
        DAS_LOG_INFO(wait_message.c_str());

        const auto initialize_result = p_callback->GetInitializeResult();
        const auto initialize_message =
            DAS_FMT_NS::format("Initialize return {}.", initialize_result);
        DAS_LOG_INFO(initialize_message.c_str());

        if (const auto p_plugin_for_ui = p_callback->GetPluginManagerForUi())
        {
            DAS::Utils::SetResult(
                p_plugin_for_ui.Get(),
                pp_out_plugin_manager_for_ui);
        }
        return ::GetExistingIDasPluginManager(pp_out_plugin_manager);
    };

public:
    DasPluginManagerController() {}

    // 获取应用列表
    // 激活指定配置文件
    Beast::HttpResponse Initialize(const Beast::HttpRequest& request)
    {
        try
        {
            const auto& json_body = request.JsonBody();
            
            DAS::DasPtr<IDasGuidVector> p_guids{};

            if (const auto create_guid_result =
                    ::CreateIDasGuidVector(nullptr, 0, p_guids.Put());
                DAS::IsFailed(create_guid_result))
            {
                const auto message = DAS_FMT_NS::format(
                    "CreateIDasGuidVector failed. Error code = {}",
                    create_guid_result);
                DAS_LOG_ERROR(message.c_str());
                return Beast::HttpResponse::CreateErrorResponse(create_guid_result, message);
            }
            
            if (json_body.contains("ignoredGuidList") && json_body["ignoredGuidList"].is_array())
            {
                for (const auto& guid_string : json_body["ignoredGuidList"])
                {
                    if (!guid_string.is_string()) continue;
                    
                    DasGuid pluginGuid{};
                    if (const auto make_guid_result =
                            ::DasMakeDasGuid(guid_string.get<std::string>().c_str(), &pluginGuid);
                        DAS::IsFailed(make_guid_result))
                    {
                        const auto message = DAS_FMT_NS::format(
                            "Make das guid failed. Error code = {}. Input = {}.",
                            make_guid_result,
                            guid_string.get<std::string>());
                        DAS_LOG_ERROR(message.c_str());
                        continue;
                    }
                    if (const auto push_back_result = p_guids->PushBack(pluginGuid);
                        DAS::IsFailed(push_back_result))
                    {
                        const auto message = DAS_FMT_NS::format(
                            "Push guid failed. Error code = {}. GUID = {}.",
                            push_back_result,
                            guid_string.get<std::string>());
                        DAS_LOG_ERROR(message.c_str());
                    }
                }
            }

            DAS::DasPtr<IDasReadOnlyString> p_profile_id{};
            const auto profile_id = json_body.value("profileId", "");
            if (const auto get_result = CreateIDasReadOnlyStringFromUtf8(
                    profile_id.c_str(),
                    p_profile_id.Put());
                DAS::IsFailed(get_result))
            {
                const auto message = DAS_FMT_NS::format(
                    "Get profile id failed. Error code = {}.",
                    get_result);
                DAS_LOG_ERROR(message.c_str());
                return Beast::HttpResponse::CreateErrorResponse(get_result, message);
            }
            // 必须先初始化调度器，再初始化PluginManager
            if (const auto error_code = SetGlobalSchedulerJsonState(*p_profile_id);
                DAS::IsFailed(error_code))
            {
                const auto message = DAS_FMT_NS::format(
                    "InitializeGlobalScheduler failed. Error code = {}.",
                    error_code);
                DAS_LOG_ERROR(message.c_str());
                return Beast::HttpResponse::CreateErrorResponse(error_code, message);
            }

            DAS::DasPtr<IDasReadOnlyGuidVector> p_const_guids{};
            p_guids->ToConst(p_const_guids.Put());
            if (const auto error_code = CreatePluginManager(
                    p_const_guids.Get(),
                    p_plugin_manager_.Put(),
                    p_plugin_manager_for_ui_.Put());
                DAS::IsFailed(error_code))
            {
                const auto message = DAS_FMT_NS::format(
                    "CreatePluginManager failed. Error code = {}",
                    error_code);
                DAS_LOG_ERROR(message.c_str());
                return Beast::HttpResponse::CreateErrorResponse(error_code, message);
            }

            return Beast::HttpResponse::CreateSuccessResponse(nullptr);
        }
        catch (const std::exception& ex)
        {
            DAS_LOG_ERROR(ex.what());
            return Beast::HttpResponse::CreateErrorResponse(DAS_E_UNEXPECTED, ex.what());
        }
    }

    // 获取插件列表
    // Get plugin list
    Beast::HttpResponse GetPluginList(const Beast::HttpRequest& request)
    {
        // temp test code
        Dto::PluginPackageDesc plugin1;
        plugin1.name = reinterpret_cast<const char*>(u8"蓝色星原-国服-插件1");
        plugin1.plugin_id = "4227E5C2-D23B-6CEA-407A-5EA189019626";

        nlohmann::json data = nlohmann::json::array();
        data.push_back(plugin1.ToJson());

        return Beast::HttpResponse::CreateSuccessResponse(data);
    }
};

} // namespace Das::Http

#endif // DAS_HTTP_CONTROLLER_DASPLUGINMANAGERCONTROLLER_HPP
