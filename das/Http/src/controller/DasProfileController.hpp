#ifndef DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP

#include "Config.h"
#include "component/Helper.hpp"
#include "das/Core/ForeignInterfaceHost/DasStringJsonInterop.h"
#include "das/ExportInterface/DasLogger.h"
#include "das/ExportInterface/IDasSettings.h"
#include "das/ExportInterface/IDasTaskScheduler.h"
#include "das/IDasBase.h"
#include "das/Utils/StringUtils.h"
#include "dto/Profile.hpp"
#include "dto/Settings.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/web/server/api/ApiController.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

/**
 *  @brief 定义配置文件管理相关API
 *  Define profile related APIs
 */
class DasProfileManagerController final : public DAS::Http::DasApiController
{
    DAS::DasPtr<IDasTaskScheduler> p_task_scheduler_{};
    DAS::DasPtr<IDasJsonSetting>   p_settings_for_ui_{};

public:
    DasProfileManagerController()
    {
        // GetIDasSettingsForUi(p_settings_for_ui_.Put());
        GetIDasTaskScheduler(p_task_scheduler_.Put());
    }
    // 获取配置文件列表
    // Get profile list
    ENDPOINT("POST", DAS_HTTP_API_PREFIX "profile/list", get_profile_list)
    {
        auto response = ProfileDescListResponse::createShared();
        response->code = DAS_S_OK;
        response->message = "";

        std::vector<DAS::DasPtr<IDasProfile>> profiles(0);
        const auto profile_size = GetAllIDasProfile(0, nullptr);
        profiles.resize(profile_size);
        std::vector<IDasProfile**> profiles_pp{profiles.size()};
        std::transform(
            DAS_FULL_RANGE_OF(profiles),
            profiles_pp.begin(),
            [](auto& profile) { return profile.Put(); });

        if (const auto get_result =
                GetAllIDasProfile(profile_size, profiles_pp.data());
            DAS::IsFailed(get_result))
        {
            return DAS_HTTP_MAKE_RESPONSE(get_result);
        }

        response->data = ProfileDescList::createShared();
        response->data->profile_list =
            decltype(response->data->profile_list)::createShared();
        for (const auto& profile : profiles)
        {
            try
            {
                DAS::DasPtr<IDasReadOnlyString> p_profile_name{};
                if (const auto get_result = profile->GetStringProperty(
                        DAS_PROFILE_PROPERTY_NAME,
                        p_profile_name.Put());
                    DAS::IsFailed(get_result))
                {
                    DAS_THROW_EC(get_result);
                }
                const auto name =
                    DAS::Http::DasString2RawString(p_profile_name.Get());
                DAS::DasPtr<IDasReadOnlyString> p_profile_id{};
                if (const auto get_result = profile->GetStringProperty(
                        DAS_PROFILE_PROPERTY_ID,
                        p_profile_id.Put());
                    DAS::IsFailed(get_result))
                {
                    DAS_THROW_EC(get_result);
                }
                const auto id =
                    DAS::Http::DasString2RawString(p_profile_id.Get());
                auto data = ProfileDesc::createShared();
                data->profile_id = id;
                data->name = name;
                response->data->profile_list->emplace_back(std::move(data));
            }
            catch (const DAS::Core::DasException& ex)
            {
                DAS_LOG_ERROR(ex.what());
            }
        }

        return MakeResponse(response);
    }

    ENDPOINT(
        "POST",
        DAS_HTTP_API_PREFIX "profile/get",
        get_profile,
        BODY_DTO(Object<ProfileId>, profile_id))
    {
        if (profile_id.get() == nullptr)
        {
            return DAS_HTTP_MAKE_RESPONSE(DAS_E_INVALID_POINTER);
        }

        const auto p_profile_id = profile_id.get()->profile_id;
        if (p_profile_id.get() == nullptr)
        {
            return DAS_HTTP_MAKE_RESPONSE(DAS_E_INVALID_POINTER);
        }

        try
        {
            const auto profile_id_string = DAS::Http::RawString2DasString(
                profile_id.get()->profile_id.get()->c_str());
            DAS::DasPtr<IDasProfile> p_profile{};
            DAS_THROW_IF_FAILED_EC(
                FindIDasProfile(profile_id_string.Get(), p_profile.Put()));
            DAS::DasPtr<IDasJsonSetting> p_settings{};
            DAS_THROW_IF_FAILED_EC(p_profile->GetJsonSettingProperty(
                DAS_PROFILE_PROPERTY_PROFILE,
                p_settings.Put()));
            DAS::DasPtr<IDasReadOnlyString> p_json_settings{};
            DAS_THROW_IF_FAILED_EC(p_settings->ToString(p_json_settings.Put()));
            const auto response = DAS_FMT_NS::format(
                "{{code: " DAS_STR(DAS_S_OK) ", message: \"\", data: {}}}",
                DAS::Http::DasString2RawString(p_json_settings.Get()));
            return oatpp::web::protocol::http::outgoing::Response::createShared(
                Status::CODE_200,
                oatpp::web::protocol::http::outgoing::BufferBody::createShared(
                    String{response},
                    "application/json"));
        }
        catch (const DAS::Core::DasException& ex)
        {
            DAS_LOG_ERROR(ex.what());
            return MakeResponse(ex);
        }
    }

    // 获取配置文件状态
    // Get profile status
    ENDPOINT("POST", DAS_HTTP_API_PREFIX "profile/status", get_profile_status)
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

        return MakeResponse(response);
    }

    ENDPOINT(
        "POST",
        DAS_HTTP_API_PREFIX "profile/create",
        create_profile,
        BODY_STRING(String, body))
    {
        if (!body)
        {
            return DAS_HTTP_MAKE_RESPONSE(DAS_E_INVALID_POINTER);
        }
        try
        {
            const auto request = nlohmann::json::parse(*body);

            DAS::DasPtr<IDasReadOnlyString> p_profile_id{};
            request.at("profileId").get_to(p_profile_id);

            DAS::DasPtr<IDasReadOnlyString> p_profile_name{};
            request.at("profileName").get_to(p_profile_name);

            const auto data_string = request.at("profile").dump();
            DAS::DasPtr<IDasReadOnlyString> p_data_string{};
            DAS_THROW_IF_FAILED_EC(
                ::CreateIDasReadOnlyStringFromUtf8(
                    data_string.c_str(),
                    p_data_string.Put()))

            DAS_THROW_IF_FAILED_EC(
                ::CreateIDasProfile(
                    p_profile_id.Get(),
                    p_profile_name.Get(),
                    p_data_string.Get()))

            auto response = ApiResponse<void>::createShared();
            response->code = DAS_S_OK;
            response->message = "";
            return MakeResponse(response);
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_LOG_ERROR(ex.what());
            return DAS_HTTP_MAKE_RESPONSE(DAS_E_INVALID_JSON);
        }
        catch (const DAS::Core::DasException& ex)
        {
            DAS_LOG_ERROR(ex.what());
            return MakeResponse(ex);
        }
    }

    ENDPOINT(
        "POST",
        DAS_HTTP_API_PREFIX "profile/delete",
        delete_profile,
        BODY_DTO(Object<ProfileId>, body_object))
    {
        if (!body_object || !body_object->profile_id)
        {
            return DAS_HTTP_MAKE_RESPONSE(DAS_E_INVALID_POINTER);
        }

        try
        {
            DAS::DasPtr<IDasReadOnlyString> p_profile_id{};
            DAS_THROW_IF_FAILED_EC(
                ::CreateIDasReadOnlyStringFromUtf8(
                    body_object->profile_id->c_str(),
                    p_profile_id.Put()))
            DAS_THROW_IF_FAILED_EC(::DeleteIDasProfile(p_profile_id.Get()))

            const auto response = ApiResponse<void>::createShared();
            response->code = DAS_S_OK;
            response->message = "";
            return MakeResponse(response);
        }
        catch (const DAS::Core::DasException& ex)
        {
            DAS_LOG_ERROR(ex.what());
            return MakeResponse(ex);
        }
    }

    ENDPOINT(
        "POST",
        DAS_HTTP_API_PREFIX "profile/save",
        save_profile,
        BODY_STRING(String, body))
    {
        if (!body)
        {
            return DAS_HTTP_MAKE_RESPONSE(DAS_E_INVALID_POINTER);
        }
        try
        {
            const auto request = nlohmann::json::parse(*body);

            DAS::DasPtr<IDasReadOnlyString> p_profile_id{};
            request.at("profileId").get_to(p_profile_id);

            DAS::DasPtr<IDasProfile> p_profile{};
            DAS_THROW_IF_FAILED_EC(
                ::FindIDasProfile(p_profile_id.Get(), p_profile.Put()))

            const auto profile = request.at("profile").dump();
            DAS::DasPtr<IDasReadOnlyString> p_profile_string{};
            DAS_THROW_IF_FAILED_EC(
                ::CreateIDasReadOnlyStringFromUtf8(
                    profile.c_str(),
                    p_profile_string.Put()))
            DAS::DasPtr<IDasJsonSetting> p_profile_json{};
            DAS_THROW_IF_FAILED_EC(p_profile->GetJsonSettingProperty(
                DAS_PROFILE_PROPERTY_PROFILE,
                p_profile_json.Put()))
            DAS_THROW_IF_FAILED_EC(
                p_profile_json->FromString(p_profile_string.Get()))
            DAS_THROW_IF_FAILED_EC(p_profile_json->Save())
            return MakeSuccessResponse();
        }
        catch (const nlohmann::json::exception& ex)
        {
            DAS_LOG_ERROR(ex.what());
            return DAS_HTTP_MAKE_RESPONSE(DAS_E_INVALID_JSON);
        }
        catch (const DAS::Core::DasException& ex)
        {
            DAS_LOG_ERROR(ex.what());
            return MakeResponse(ex);
        }
    }

    ENDPOINT(
        "POST",
        DAS_HTTP_API_PREFIX "profile/enable",
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
            return createDtoResponse(Status::CODE_200, response);
        }

        response->code =
            p_task_scheduler_->SetEnabled(profile_enabled->enabled);
        response->message = "";
        return createDtoResponse(Status::CODE_200, response);
    }

    ENDPOINT(
        "POST",
        DAS_HTTP_API_PREFIX "profile/start",
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
            return createDtoResponse(Status::CODE_200, response);
        }

        response->code = p_task_scheduler_->ForceStart();
        response->message = "";
        return createDtoResponse(Status::CODE_200, response);
    }

    ENDPOINT(
        "POST",
        DAS_HTTP_API_PREFIX "profile/stop",
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
            return createDtoResponse(Status::CODE_200, response);
        }

        response->code = p_task_scheduler_->RequestStop();
        response->message = "";
        return createDtoResponse(Status::CODE_200, response);
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
    ENDPOINT("POST", DAS_HTTP_API_PREFIX "settings/task/list", get_task_list)
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
            return createDtoResponse(Status::CODE_200, response);
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
                return createDtoResponse(Status::CODE_200, response);
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

        return createDtoResponse(Status::CODE_200, response);
    }
};

#include OATPP_CODEGEN_END(ApiController)

#endif // DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP
