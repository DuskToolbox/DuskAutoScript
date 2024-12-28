#ifndef DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP

#include "component/Helper.hpp"
#include "das/ExportInterface/DasLogger.h"
#include "das/ExportInterface/IDasSettings.h"
#include "das/ExportInterface/IDasTaskScheduler.h"
#include "das/IDasBase.h"
#include "dto/Profile.hpp"
#include "dto/Settings.hpp"
#include "nlohmann/json.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/core/macro/component.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

#include <das/Utils/StringUtils.h>

/**
 *  @brief 定义配置文件管理相关API
 *  Define profile related APIs
 */
class DasProfileManagerController final
    : public oatpp::web::server::api::ApiController
{
    std::shared_ptr<ObjectMapper> json_object_mapper_{
        oatpp::parser::json::mapping::ObjectMapper::createShared()};
    DAS::DasPtr<IDasTaskScheduler> p_task_scheduler_{};
    DAS::DasPtr<IDasSettingsForUi> p_settings_for_ui_{};

public:
    DasProfileManagerController(
        OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : ApiController{objectMapper}
    {
        GetIDasSettingsForUi(p_settings_for_ui_.Put());
        GetIDasTaskScheduler(p_task_scheduler_.Put());
    }

    ENDPOINT("POST", "/api/profile/global", get_global_profile)
    {
        auto response = ProfileDescList::createShared();
        response->code = DAS_S_OK;
        response->message = "";

        DAS::DasPtr<IDasReadOnlyString> p_settings_json{};
        const auto                      get_result =
            ::DasLoadExtraStringForUi(p_settings_json.Put());
        if (DAS::IsFailed(get_result))
        {
            const auto message =
                DAS::Http::GetPredefinedErrorMessage(get_result);
            DAS_LOG_ERROR(message.c_str());
            response->code = get_result;
            response->message = message;
            return createDtoResponse(
                Status::CODE_200,
                json_object_mapper_->writeToString(response));
        }

        const char* p_u8_settings{nullptr};
        if (const auto get_u8_result = p_settings_json->GetUtf8(&p_u8_settings);
            DAS::IsFailed(get_result))
        {
            response->code = get_u8_result;
            response->message = DAS_FMT_NS::format(
                "Call GetUtf8 failed. Error code = {}.",
                get_result);
            return createDtoResponse(
                Status::CODE_200,
                json_object_mapper_->writeToString(response));
        }

        response->code = get_result;
        response->data = p_u8_settings;
        return createDtoResponse(
            Status::CODE_200,
            json_object_mapper_->writeToString(response));
    }

    // 获取配置文件列表
    // Get profile list
    ENDPOINT("POST", "/api/profile/list", get_profile_list)
    {
        auto response = ProfileDescList::createShared();
        response->code = DAS_S_OK;
        response->message = "";

        DAS::DasPtr<IDasReadOnlyString> p_settings_json{};
        if (const auto get_result =
                p_settings_for_ui_->ToString(p_settings_json.Put());
            DAS::IsFailed(get_result))
        {
            const auto message = DAS_FMT_NS::format(
                "Get settings json failed. Error code: {}",
                get_result);
            DAS_LOG_ERROR(message.c_str());
            response->code = get_result;
            response->message = message;
            return createDtoResponse(
                Status::CODE_200,
                json_object_mapper_->writeToString(response));
        }

        const char* p_u8_settings_json;
        if (const auto get_result =
                p_settings_json->GetUtf8(&p_u8_settings_json);
            DAS::IsFailed(get_result))
        {
            const auto message = DAS_FMT_NS::format(
                "Get settings json string failed. Error code: {}",
                get_result);
            DAS_LOG_ERROR(message.c_str());
            response->code = get_result;
            response->message = message;
            return createDtoResponse(
                Status::CODE_200,
                json_object_mapper_->writeToString(response));
        }

        std::string profile_name{
            DAS_UTILS_STRINGUTILS_DEFINE_U8STR("默认配置0")};
        const auto settings = nlohmann::json::parse(p_u8_settings_json);
        if (settings.contains("name"))
        {
            settings["name"].get_to(profile_name);
        }

        const auto profile0 = ProfileDesc::createShared();
        profile0->name = profile_name;
        profile0->profile_id = "0";

        response->data = {profile0};

        return createDtoResponse(
            Status::CODE_200,
            json_object_mapper_->writeToString(response));
    }

    // 获取配置文件状态
    // Get profile status
    ENDPOINT("POST", "/api/profile/status", get_profile_status)
    {
        auto response = ProfileStatusList::createShared();
        response->code = DAS_S_OK;
        response->message = "";

        // 目前只支持一个配置文件
        auto profile1_status = ProfileStatus::createShared();
        profile1_status->profile_id = "0";
        profile1_status->run = p_task_scheduler_->IsTaskExecuting();
        profile1_status->enable = p_task_scheduler_->GetEnabled();

        response->data = {profile1_status};

        return createDtoResponse(
            Status::CODE_200,
            json_object_mapper_->writeToString(response));
    }

    ENDPOINT(
        "POST",
        "/api/profile/enable",
        set_enable,
        BODY_DTO(Object<ProfileEnabled>, profile_enabled))
    {
        auto response = ProfileStatusList::createShared();
        if (profile_enabled->profile_id != "0")
        {
            response->code = DAS_E_OUT_OF_RANGE;
            const auto message = DAS_FMT_NS::format(
                "Profile index out of range. Index = {}.",
                profile_enabled->profile_id->c_str());
            DAS_LOG_ERROR(message.c_str());
            response->message = message;
            return createDtoResponse(
                Status::CODE_200,
                json_object_mapper_->writeToString(response));
        }

        response->code =
            p_task_scheduler_->SetEnabled(profile_enabled->enabled);
        response->message = "";
        return createDtoResponse(
            Status::CODE_200,
            json_object_mapper_->writeToString(response));
    }

    ENDPOINT(
        "POST",
        "/api/profile/start",
        start_profile,
        BODY_DTO(Object<ProfileId>, profile_id))
    {
        auto response = ProfileStatusList::createShared();
        if (profile_id->profile_id != "0")
        {
            response->code = DAS_E_OUT_OF_RANGE;
            const auto message = DAS_FMT_NS::format(
                "Profile index out of range. Index = {}.",
                profile_id->profile_id->c_str());
            DAS_LOG_ERROR(message.c_str());
            response->message = message;
            return createDtoResponse(
                Status::CODE_200,
                json_object_mapper_->writeToString(response));
        }

        response->code = p_task_scheduler_->ForceStart();
        response->message = "";
        return createDtoResponse(
            Status::CODE_200,
            json_object_mapper_->writeToString(response));
    }

    ENDPOINT(
        "POST",
        "/api/profile/stop",
        stop_profile,
        BODY_DTO(Object<ProfileId>, profile_id))
    {
        auto response = ProfileStatusList::createShared();
        if (profile_id->profile_id != "0")
        {
            response->code = DAS_E_OUT_OF_RANGE;
            const auto message = DAS_FMT_NS::format(
                "Profile index out of range. Index = {}.",
                profile_id->profile_id->c_str());
            DAS_LOG_ERROR(message.c_str());
            response->message = message;
            return createDtoResponse(
                Status::CODE_200,
                json_object_mapper_->writeToString(response));
        }

        response->code = p_task_scheduler_->RequestStop();
        response->message = "";
        return createDtoResponse(
            Status::CODE_200,
            json_object_mapper_->writeToString(response));
    }

private:
    static void ReadProperty(
        oatpp::data::mapping::type::String& property_string,
        IDasTaskInfo*                       p_task_info,
        size_t                              property_index)
    {
        const char* p_property{nullptr};
        if (const auto error_code = p_task_info->GetProperty(
                DAS_TASK_INFO_PROPERTIES[property_index],
                &p_property);
            DAS::IsFailed(error_code))
        {
            const auto message = DAS_FMT_NS::format(
                "GetProperty failed. Error code = {}.",
                error_code);
            DAS_LOG_ERROR(message.c_str());
            property_string = "";
        }
        property_string = p_property;
    }

    // 获取任务列表
    // Get task list
    ENDPOINT("POST", "/api/settings/task/list", get_task_list)
    {

        auto response = TaskDescList::createShared();
        response->code = DAS_S_OK;
        response->message = "";

        DAS::DasPtr<IDasTaskInfoVector> p_task_info_vector{};
        if (const auto error_code =
                p_task_scheduler_->GetAllWorkingTasks(p_task_info_vector.Put());
            DAS::IsFailed(error_code))
        {
            response->code = error_code;
            const auto message = DAS_FMT_NS::format(
                "GetAllWorkingTasks failed. Error code = {}.",
                error_code);
            DAS_LOG_ERROR(message.c_str());
            response->message = message;
            return createDtoResponse(
                Status::CODE_200,
                json_object_mapper_->writeToString(response));
        }

        for (auto [error_code, index, p_task_info] =
                 std::tuple<DasResult, size_t, DAS::DasPtr<IDasTaskInfo>>{
                     DAS_E_UNDEFINED_RETURN_VALUE,
                     0,
                     {}};
             error_code =
                 p_task_info_vector->EnumByIndex(index, p_task_info.Put()),
                                      error_code != DAS_E_OUT_OF_RANGE;
             ++index)
        {
            if (DAS::IsFailed(error_code))
            {
                response->code = error_code;
                const auto message = DAS_FMT_NS::format(
                    "EnumByIndex failed. Error code = {}.",
                    error_code);
                DAS_LOG_ERROR(message.c_str());
                response->message = message;
                return createDtoResponse(
                    Status::CODE_200,
                    json_object_mapper_->writeToString(response));
            }

            auto    task_info = TaskDesc::createShared();
            DasGuid iid;
            p_task_info->GetIid(&iid);
            DAS::DasPtr<IDasReadOnlyString> p_iid_string{};
            DasGuidToString(&iid, p_iid_string.Put());
            const char* iid_string;
            p_iid_string->GetUtf8(&iid_string);
            task_info->plugin_id = iid_string;
            DAS::DasPtr<IDasReadOnlyString> p_label{};
            ReadProperty(
                task_info->name,
                p_task_info.Get(),
                DAS_TASK_INFO_PROPERTIES_NAME_INDEX);
            ReadProperty(
                task_info->game_name,
                p_task_info.Get(),
                DAS_TASK_INFO_PROPERTIES_GAME_NAME_INDEX);
            response->data->emplace_back(std::move(task_info));
        }

        return createDtoResponse(
            Status::CODE_200,
            json_object_mapper_->writeToString(response));
    }
};

#include OATPP_CODEGEN_END(ApiController)

#endif // DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP
