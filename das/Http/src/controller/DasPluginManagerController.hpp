#ifndef DAS_HTTP_CONTROLLER_DASPLUGINMANAGERCONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_DASPLUGINMANAGERCONTROLLER_HPP

#include "../component/DasInitializePluginManagerCallback.h"

#include "das/ExportInterface/DasLogger.h"
#include "das/ExportInterface/IDasPluginManager.h"
#include "das/IDasBase.h"
#include "das/Utils/fmt.h"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

#include "dto/Profile.hpp"
#include "dto/Settings.hpp"

#include "component/Helper.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

class DasPluginManagerController final : public DAS::Http::DasApiController
{
    DAS::DasPtr<IDasPluginManager>      p_plugin_manager_{};
    DAS::DasPtr<IDasPluginManagerForUi> p_plugin_manager_for_ui_{};

    static auto CreatePluginManager(
        IDasReadOnlyGuidVector*  p_guid_vector,
        IDasPluginManager**      pp_out_plugin_manager,
        IDasPluginManagerForUi** pp_out_plugin_manager_for_ui) -> DasResult
    {
        DAS::DasPtr<IDasGuidVector> p_empty_guids{};
        ::CreateIDasGuidVector(nullptr, 0, p_empty_guids.Put());
        DAS::DasPtr<IDasReadOnlyGuidVector> p_const_empty_guids{};
        p_empty_guids->ToConst(p_const_empty_guids.Put());
        DAS::DasPtr<IDasInitializeIDasPluginManagerWaiter> p_waiter{};
        const auto                                         p_callback =
            Das::MakeDasPtr<DasInitializePluginManagerCallback>();
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
    DasPluginManagerController(
        std::shared_ptr<ObjectMapper> object_mapper =
            oatpp::parser::json::mapping::ObjectMapper::createShared())
    {
    }

    // 获取应用列表
    // 激活指定配置文件
    ENDPOINT(
        "POST",
        "/api/profile/initialize",
        get_initialize,
        BODY_DTO(Object<IgnoredGuidList>, ignoredGuidList))
    {
        auto response = ApiResponse<void>::createShared();
        DAS::DasPtr<IDasGuidVector> p_guids{};

        if (const auto create_guid_result =
                ::CreateIDasGuidVector(nullptr, 0, p_guids.Put());
            DAS::IsFailed(create_guid_result))
        {
            const auto message = DAS_FMT_NS::format(
                "CreateIDasGuidVector failed. Error code = {}",
                create_guid_result);
            DAS_LOG_ERROR(message.c_str());
            response->code = create_guid_result;
            response->message = message;
            return createDtoResponse(Status::CODE_200, response);
        }
        for (const auto& guidString : *ignoredGuidList.get()->ignoredGuidList)
        {
            DasGuid pluginGuid{};
            if (const auto make_guid_result =
                    ::DasMakeDasGuid(guidString->c_str(), &pluginGuid);
                DAS::IsFailed(make_guid_result))
            {
                const auto message = DAS_FMT_NS::format(
                    "Make das guid failed. Error code = {}. Input = {}.",
                    make_guid_result,
                    guidString->c_str());
                DAS_LOG_ERROR(message.c_str());
                response->message = message;
                response->code = DAS_S_FALSE;
                continue;
            }
            if (const auto push_back_result = p_guids->PushBack(pluginGuid);
                DAS::IsFailed(push_back_result))
            {
                const auto message = DAS_FMT_NS::format(
                    "Push guid failed. Error code = {}. GUID = {}.",
                    push_back_result,
                    guidString->c_str());
                DAS_LOG_ERROR(message.c_str());
                response->code = DAS_S_FALSE;
                response->message = message;
            }
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
            response->code = error_code;
            response->message = message;
            return createDtoResponse(Status::CODE_200, response);
        }

        return createDtoResponse(Status::CODE_200, response);
    }

    // 获取插件列表
    // Get plugin list
    ENDPOINT(
        "POST",
        "/api/settings/plugin/list",
        get_plugin_list,
        BODY_DTO(oatpp::data::mapping::type::Int32, profileId))
    {

        auto response = PluginPackageDescList::createShared();
        response->code = DAS_S_OK;
        response->message = "";

        // temp test code
        auto plugin1 = PluginPackageDesc::createShared(); // AzurPromilia1
        plugin1->name = reinterpret_cast<const char*>(u8"蓝色星原-国服-插件1");
        plugin1->plugin_id = "4227E5C2-D23B-6CEA-407A-5EA189019626";

        response->data = {plugin1};

        return createDtoResponse(Status::CODE_200, response);
    }
};

#include OATPP_CODEGEN_END(ApiController)

#endif // DAS_HTTP_CONTROLLER_DASPLUGINMANAGERCONTROLLER_HPP
