#ifndef DAS_HTTP_CONTROLLER_CONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_CONTROLLER_HPP


#include "dto/Global.hpp"
#include "dto/Settings.hpp"

#include <string>

#include OATPP_CODEGEN_BEGIN(ApiController)
#include "dto/Profile.hpp"

#include <das/ExportInterface/DasLogger.h>

class DasController final : public oatpp::web::server::api::ApiController
{
private:

    std::shared_ptr<ObjectMapper> jsonObjectMapper{
        oatpp::parser::json::mapping::ObjectMapper::createShared()};



public:
    DasController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : ApiController{objectMapper}
    {

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
    //     response->code = DAS_S_OK;
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
        "/api/profile/stop",
        stop_profile,
        // BODY_DTO(Int32, profile_id)
        // BODY_STRING(String, profile_id))
        BODY_DTO(Object<ProfileId>, profile_id))
    {

        // std::string a = "停止配置文件" + std::to_string(profile_id);
        std::string a = "停止配置文件" + profile_id->profile_id;
        DAS_LOG_INFO(a.c_str());

        auto response =
            ApiResponse<oatpp::Object<ProfileRunning>>::createShared();
        response->code = DAS_S_OK;
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
        response->code = DAS_S_OK;
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

};

#include OATPP_CODEGEN_END(ApiController)

#endif // DAS_HTTP_CONTROLLER_CONTROLLER_HPP
