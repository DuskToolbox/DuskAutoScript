#ifndef DAS_HTTP_CONTROLLER_CONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_CONTROLLER_HPP

#include "AutoStarRail/ExportInterface/AsrLogger.h"
#include "AutoStarRail/ExportInterface/IAsrPluginManager.h"
#include "AutoStarRail/IAsrBase.h"
#include "AutoStarRail/PluginInterface/IAsrTask.h"
#include "AutoStarRail/Utils/fmt.h"
#include "oatpp/core/base/Environment.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/core/macro/component.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/web/server/api/ApiController.hpp"


#include "dto/Global.hpp"
#include "dto/Log.hpp"
#include "dto/Profile.hpp"
#include "dto/Settings.hpp"

#include "ControllerUtils.hpp"
#include <string>

#include OATPP_CODEGEN_BEGIN(ApiController)

class Controller : public oatpp::web::server::api::ApiController
{
private:
    ASR::AsrPtr<AsrHttpLogReader> p_reader{ASR::MakeAsrPtr<AsrHttpLogReader>()};
    ASR::AsrPtr<IAsrLogRequester> p_requester{};

    ASR::AsrPtr<IAsrPluginManager> p_plugin_manager{};

    std::shared_ptr<ObjectMapper> jsonObjectMapper{
        oatpp::parser::json::mapping::ObjectMapper::createShared()};

    static auto CreatePluginManager() -> ASR::AsrPtr<IAsrPluginManager>
    {
        ASR::AsrPtr<IAsrPluginManager> result{};
        ASR::AsrPtr<IAsrGuidVector>    p_empty_guids{};
        ::CreateIAsrGuidVector(nullptr, 0, p_empty_guids.Put());
        ASR::AsrPtr<IAsrReadOnlyGuidVector> p_const_empty_guids{};
        p_empty_guids->ToConst(p_const_empty_guids.Put());
        const auto error_code = ::CreateIAsrPluginManagerAndGetResult(
            p_const_empty_guids.Get(),
            result.Put());

        const auto create_message = ASR_FMT_NS::format(
            "CreateIAsrPluginManagerAndGetResult return {}.",
            error_code);
        ASR_LOG_INFO(create_message.c_str());

        return result;
    }

public:
    Controller(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController{objectMapper}
    {
        ::CreateIAsrLogRequester(32, p_requester.Put());
        // TODO:
        // 自此开始获取日志，未来要必须将日志单独一个Controller并最早初始化
        p_plugin_manager = CreatePluginManager();
    }

    /**
     *  定义日志相关API
     *  Define log related APIs
     */

    // 获取日志
    // Get logs

    ENDPOINT("GET", "/api/logs", get_logs)
    {

        auto response = Logs::createShared();
        response->code = ASR_S_OK;
        response->message = "";
        response->data = LogsData::createShared();
        response->data->logs = {};
        // const auto error_code = p_requester->RequestOne(p_reader.Get());

        // error_code = p_requester->RequestOne(p_reader.Get());
        // OATPP_LOGD("日志接口", std::to_string(i).c_str());
        // std::string str = sp_one_message->data();
        // OATPP_LOGD("日志接口", str.c_str());
        // response->data->logs->push_back(reinterpret_cast<const
        // char*>(str.c_str()));

        // int i = 0;

        // ASR_LOG_INFO("访问日志接口");

        while (true)
        {
            const auto error_code = p_requester->RequestOne(p_reader.Get());
            // OATPP_LOGD("日志接口", std::to_string(i).c_str());
            // OATPP_LOGD("日志接口", sp_one_message->data());

            if (error_code == ASR_S_OK)
            {
                OATPP_LOGD("日志接口", "1");
                // OATPP_LOGD("日志接口", sp_one_message->data());
                response->data->logs->push_back(
                    p_reader->GetMessage().data());
            }
            // else if (
            //     !response->data->logs->empty()
            //     && error_code == ASR_E_OUT_OF_RANGE)
            // {
            else if (error_code == ASR_E_OUT_OF_RANGE)
            {
                OATPP_LOGD("日志接口", "2");
                OATPP_LOGD("日志接口", std::to_string(error_code).c_str());
                response->code = ASR_S_OK;
                break;
            }
            else
            {
                OATPP_LOGD("日志接口", "3");
                OATPP_LOGD("日志接口", std::to_string(error_code).c_str());
                response->code = error_code;
                break;
            }
        }

        return createDtoResponse(
            Status::CODE_200,
            jsonObjectMapper->writeToString(response));
    }

    /**
     *  定义配置文件管理相关API
     *  Define profile related APIs
     */

    // 获取配置文件列表
    // Get profile list
    ENDPOINT("GET", "/api/profile/list", get_profile_list)
    {

        auto response = ProfileDescList::createShared();
        response->code = ASR_S_OK;
        response->message = "";

        // temp test code
        auto profile1 = ProfileDesc::createShared();
        profile1->name = reinterpret_cast<const char*>(u8"配置0");
        profile1->profile_id = "0";

        auto profile2 = ProfileDesc::createShared();
        profile2->name = reinterpret_cast<const char*>(u8"様式ファイル1");
        profile2->profile_id = "1";

        auto profile3 = ProfileDesc::createShared();
        profile3->name = reinterpret_cast<const char*>(
            u8"様式ファイル2 長さが制限を超えています");
        profile3->profile_id = "2";

        response->data = {profile1, profile2, profile3};
        // temp test code

        return createDtoResponse(
            Status::CODE_200,
            jsonObjectMapper->writeToString(response));
    }

    // 获取配置文件状态
    // Get profile status
    ENDPOINT("GET", "/api/profile/status", get_profile_status)
    {

        auto response = ProfileStatusList::createShared();
        response->code = ASR_S_OK;
        response->message = "";

        // temp test code
        auto profile1_status = ProfileStatus::createShared();
        profile1_status->profile_id = "0";
        profile1_status->run = false;
        profile1_status->enable = true;

        auto profile2_status = ProfileStatus::createShared();
        profile2_status->profile_id = "1";
        profile2_status->run = false;
        profile2_status->enable = false;

        auto profile3_status = ProfileStatus::createShared();
        profile3_status->profile_id = "2";
        profile3_status->run = false;
        profile3_status->enable = false;

        response->data = {profile1_status, profile2_status, profile3_status};
        // temp test code

        return createDtoResponse(
            Status::CODE_200,
            jsonObjectMapper->writeToString(response));
    }

    // 获取配置文件状态
    // Get profile status
    // ENDPOINT(
    //     "GET",
    //     "/api/profile/status",
    //     get_profile_status,
    //     QUERIES(QueryParams, queryParams))
    // {

    //     auto response = ProfileStatusList::createShared();
    //     response->code = ASR_S_OK;
    //     response->message = "";

    //     if (queryParams.getSize() == 0)
    //     {
    //         // temp test code
    //         auto profile1_status = ProfileStatus::createShared();
    //         profile1_status->run = false;
    //         profile1_status->enable = true;

    //         auto profile2_status = ProfileStatus::createShared();
    //         profile2_status->run = false;
    //         profile2_status->enable = false;

    //         auto profile3_status = ProfileStatus::createShared();
    //         profile3_status->run = false;
    //         profile3_status->enable = false;

    //         response->data = {
    //             profile1_status,
    //             profile2_status,
    //             profile3_status};
    //         // temp test code

    //         return createDtoResponse(
    //             Status::CODE_200,
    //             jsonObjectMapper->writeToString(response));
    //     }
    //     else
    //     {
    //         response->message = "配置文件接口:参数数量错误";
    //         return createDtoResponse(
    //             Status::CODE_200,
    //             jsonObjectMapper->writeToString(response));
    //     }
    // }

    // 启动配置文件
    // Start profile
    ENDPOINT(
        "POST",
        "/api/profile/start",
        start_profile,
        // BODY_DTO(Int32, profile_id)
        // BODY_STRING(String, profile_id))
        BODY_DTO(Object<ProfileId>, profile_id))
    {
        // std::string a = "启动配置文件" + std::to_string(profile_id);
        std::string a = "启动配置文件" + profile_id->profile_id;
        ASR_LOG_INFO(a.c_str());

        AsrGuid    touch_iid;
        const auto create_result =
            AsrMakeAsrGuid("6C78DCAD-9173-4EE3-84CC-546973658460", &touch_iid);
        ASR::AsrPtr<IAsrTask> p_task{};
        p_plugin_manager->FindInterface(touch_iid, p_task.PutVoid());
        ASR::AsrPtr<IAsrReadOnlyString> p_profile{};
        ::CreateIAsrReadOnlyStringFromUtf8(
            R"(
{
    "input": {
        "x": 100,
        "y": 200
    }
}
        )",
            p_profile.Put());
        p_task->Do(p_profile.Get());

        auto response =
            ApiResponse<oatpp::Object<ProfileRunning>>::createShared();
        response->code = ASR_S_OK;
        response->message = "";
        response->data = ProfileRunning::createShared();

        response->data->profile_id = profile_id->profile_id;
        response->data->run = true;

        // // temp test code
        // auto profile1_status = ProfileStatus::createShared();
        // profile1_status->profile_id = "0";
        // profile1_status->run = false;
        // profile1_status->enable = true;

        // auto profile2_status = ProfileStatus::createShared();
        // profile2_status->profile_id = "1";
        // profile2_status->run = false;
        // profile2_status->enable = false;

        // auto profile3_status = ProfileStatus::createShared();
        // profile3_status->profile_id = "2";
        // profile3_status->run = false;
        // profile3_status->enable = false;

        // response->data = {profile1_status, profile2_status,
        // profile3_status};
        // // temp test code

        return createDtoResponse(
            Status::CODE_200,
            jsonObjectMapper->writeToString(response));
    }

    // 启动配置文件
    // Start profile
    ENDPOINT(
        "POST",
        "/api/profile/stop",
        stop_profile,
        // BODY_DTO(Int32, profile_id)
        // BODY_STRING(String, profile_id))
        BODY_DTO(Object<ProfileId>, profile_id))
    {

        // std::string a = "停止配置文件" + std::to_string(profile_id);
        std::string a = "停止配置文件" + profile_id->profile_id;
        ASR_LOG_INFO(a.c_str());

        auto response =
            ApiResponse<oatpp::Object<ProfileRunning>>::createShared();
        response->code = ASR_S_OK;
        response->message = "";
        response->data = ProfileRunning::createShared();

        response->data->profile_id = profile_id->profile_id;
        response->data->run = false;

        // // temp test code
        // auto profile1_status = ProfileStatus::createShared();
        // profile1_status->profile_id = "0";
        // profile1_status->run = false;
        // profile1_status->enable = true;

        // auto profile2_status = ProfileStatus::createShared();
        // profile2_status->profile_id = "1";
        // profile2_status->run = false;
        // profile2_status->enable = false;

        // auto profile3_status = ProfileStatus::createShared();
        // profile3_status->profile_id = "2";
        // profile3_status->run = false;
        // profile3_status->enable = false;

        // response->data = {profile1_status, profile2_status,
        // profile3_status};
        // // temp test code

        return createDtoResponse(
            Status::CODE_200,
            jsonObjectMapper->writeToString(response));
    }

    // 停止配置文件
    // Stop profile

    /**
     *  定义设置相关API
     *  Define settings related APIs
     */

    // 获取应用列表
    // Get app list
    ENDPOINT("GET", "/api/settings/app/list", get_app_list)
    {

        auto response = AppDescList::createShared();
        response->code = ASR_S_OK;
        response->message = "";

        // temp test code
        auto app1 = AppDesc::createShared(); // AzurPromilia
        app1->name = reinterpret_cast<const char*>(u8"蓝色星原-国服");
        app1->package_name = "com.manjuu.azurpromilia";

        auto app2 = AppDesc::createShared(); // Resonance
        app2->name = reinterpret_cast<const char*>(u8"雷索纳斯-国服");
        app2->package_name = "com.hermes.goda";

        response->data = {app1, app2};
        // temp test code

        return createDtoResponse(
            Status::CODE_200,
            jsonObjectMapper->writeToString(response));
    }

    // 获取插件列表
    // Get plugin list
    ENDPOINT("GET", "/api/settings/plugin/list", get_plugin_list)
    {

        auto response = PluginDescList::createShared();
        response->code = ASR_S_OK;
        response->message = "";

        // temp test code
        auto plugin1 = PluginDesc::createShared(); // AzurPromilia1
        plugin1->name = reinterpret_cast<const char*>(u8"蓝色星原-国服-插件1");
        plugin1->plugin_id = "4227E5C2-D23B-6CEA-407A-5EA189019626";

        auto plugin2 = PluginDesc::createShared(); // Resonance1
        plugin2->name = reinterpret_cast<const char*>(u8"雷索纳斯-国服-插件1");
        plugin2->plugin_id = "CF6F77EC-8378-0FA6-9A5B-46B12ADC14A3";

        auto plugin3 = PluginDesc::createShared(); // AzurPromilia2
        plugin3->name = reinterpret_cast<const char*>(u8"蓝色星原-国服-插件2");
        plugin3->plugin_id = "C90CE541-E04D-1908-F6A4-62F81AE695CE";

        auto plugin4 = PluginDesc::createShared(); // Resonance2
        plugin4->name = reinterpret_cast<const char*>(u8"雷索纳斯-国服-插件2");
        plugin4->plugin_id = "2FA49598-7DFF-7D3C-CC51-AE539EA41DEB";

        response->data = {plugin1, plugin2, plugin3, plugin4};
        // temp test code

        return createDtoResponse(
            Status::CODE_200,
            jsonObjectMapper->writeToString(response));
    }

    // 获取任务列表
    // Get task list
    ENDPOINT("GET", "/api/settings/task/list", get_task_list)
    {

        auto response = TaskDescList::createShared();
        response->code = ASR_S_OK;
        response->message = "";

        // temp test code
        auto task1 = TaskDesc::createShared();
        task1->name = reinterpret_cast<const char*>(u8"主线关卡");
        task1->plugin_id = "4227E5C2-D23B-6CEA-407A-5EA189019626";
        task1->package_name = "com.manjuu.azurpromilia";
        task1->sub_group = reinterpret_cast<const char*>(u8"每日");

        auto task2 = TaskDesc::createShared();
        task2->name = reinterpret_cast<const char*>(u8"困难关卡");
        task2->plugin_id = "C90CE541-E04D-1908-F6A4-62F81AE695CE";
        task2->package_name = "com.manjuu.azurpromilia";
        task2->sub_group = reinterpret_cast<const char*>(u8"每日");

        auto task3 = TaskDesc::createShared();
        task3->name = reinterpret_cast<const char*>(u8"任务奖励");
        task3->plugin_id = "4227E5C2-D23B-6CEA-407A-5EA189019626";
        task3->package_name = "com.manjuu.azurpromilia";
        task3->sub_group = reinterpret_cast<const char*>(u8"收获");

        auto task4 = TaskDesc::createShared();
        task4->name = reinterpret_cast<const char*>(u8"每日悬赏");
        task4->plugin_id = "CF6F77EC-8378-0FA6-9A5B-46B12ADC14A3";
        task4->package_name = "com.hermes.goda";
        task4->sub_group = reinterpret_cast<const char*>(u8"每日");

        auto task5 = TaskDesc::createShared();
        task5->name = reinterpret_cast<const char*>(u8"每日悬赏");
        task5->plugin_id = "2FA49598-7DFF-7D3C-CC51-AE539EA41DEB";
        task5->package_name = "com.hermes.goda";
        task5->sub_group = reinterpret_cast<const char*>(u8"倒垃圾");

        auto task6 = TaskDesc::createShared();
        task6->name = reinterpret_cast<const char*>(u8"每日悬赏");
        task6->plugin_id = "CF6F77EC-8378-0FA6-9A5B-46B12ADC14A3";
        task6->package_name = "com.hermes.goda";
        task6->sub_group = reinterpret_cast<const char*>(u8"每日");

        response->data = {task1, task2, task3, task4, task5, task6};
        // temp test code

        return createDtoResponse(
            Status::CODE_200,
            jsonObjectMapper->writeToString(response));
    }
};

#include OATPP_CODEGEN_END(ApiController)

#endif // DAS_HTTP_CONTROLLER_CONTROLLER_HPP
