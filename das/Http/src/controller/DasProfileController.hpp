#ifndef DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP

#include "Config.h"
#include "beast/JsonUtils.hpp"
#include "beast/Request.hpp"
#include "component/Helper.hpp"
#include "das/Core/ForeignInterfaceHost/DasStringJsonInterop.h"
#include "das/ExportInterface/DasLogger.h"
#include "das/ExportInterface/IDasSettings.h"
#include "das/ExportInterface/IDasTaskScheduler.h"
#include "das/IDasBase.h"
#include "das/Utils/StringUtils.h"
#include "dto/Profile.hpp"
#include "dto/Settings.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Das::Http
{

    /**
     *  @brief 定义配置文件管理相关API
     *  Define profile related APIs
     */
    class DasProfileManagerController
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
        Beast::HttpResponse GetProfileList(const Beast::HttpRequest& request)
        {
            Dto::ProfileDescListResponse response;
            response.code = DAS_S_OK;
            response.message = "";

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
                return Beast::HttpResponse::CreateErrorResponse(
                    get_result,
                    "Failed to get profiles");
            }

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
                    Dto::ProfileDesc data;
                    data.profile_id = id;
                    data.name = name;
                    response.data.profile_list.push_back(std::move(data));
                }
                catch (const DasException& ex)
                {
                    DAS_LOG_ERROR(ex.what());
                    return Beast::HttpResponse::CreateErrorResponse(
                        ex.GetErrorCode(),
                        ex.what());
                }
            }

            return Beast::HttpResponse::CreateSuccessResponse(
                response.data.ToJson());
        }

        Beast::HttpResponse GetProfile(const Beast::HttpRequest& request)
        {
            try
            {
                const auto& json_body = request.JsonBody();
                if (!json_body.contains("profileId"))
                {
                    return Beast::HttpResponse::CreateErrorResponse(
                        DAS_E_INVALID_POINTER,
                        "Missing profileId");
                }

                const auto profile_id_string = DAS::Http::RawString2DasString(
                    json_body["profileId"].get<std::string>().c_str());
                DAS::DasPtr<IDasProfile> p_profile{};
                DAS_THROW_IF_FAILED_EC(
                    FindIDasProfile(profile_id_string.Get(), p_profile.Put()));
                DAS::DasPtr<IDasJsonSetting> p_settings{};
                DAS_THROW_IF_FAILED_EC(p_profile->GetJsonSettingProperty(
                    DAS_PROFILE_PROPERTY_PROFILE,
                    p_settings.Put()));
                DAS::DasPtr<IDasReadOnlyString> p_json_settings{};
                DAS_THROW_IF_FAILED_EC(
                    p_settings->ToString(p_json_settings.Put()));

                nlohmann::json data;
                data = nlohmann::json::parse(
                    DAS::Http::DasString2RawString(p_json_settings.Get()));
                return Beast::HttpResponse::CreateSuccessResponse(data);
            }
            catch (const DasException& ex)
            {
                DAS_LOG_ERROR(ex.what());
                return Beast::HttpResponse::CreateErrorResponse(
                    ex.GetErrorCode(),
                    ex.what());
            }
            catch (const std::exception& ex)
            {
                DAS_LOG_ERROR(ex.what());
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_UNEXPECTED,
                    ex.what());
            }
        }

        // 获取配置文件状态
        // Get profile status
        Beast::HttpResponse GetProfileStatus(const Beast::HttpRequest& request)
        {
            Dto::ProfileStatusList response;
            response.code = DAS_S_OK;
            response.message = "";

            // 目前只支持一个配置文件
            Dto::ProfileStatus profile1_status;
            profile1_status.profile_id = "0";
            profile1_status.run = p_task_scheduler_->IsTaskExecuting();
            profile1_status.enable = p_task_scheduler_->GetEnabled();
            response.data = {profile1_status};

            return Beast::HttpResponse::CreateSuccessResponse(
                nlohmann::json(response.data));
        }

        Beast::HttpResponse CreateProfile(const Beast::HttpRequest& request)
        {
            try
            {
                const auto& json_body = request.JsonBody();

                DAS::DasPtr<IDasReadOnlyString> p_profile_id{};
                const auto                      profile_id =
                    json_body.at("profileId").get<std::string>();
                DAS_THROW_IF_FAILED_EC(
                    ::CreateIDasReadOnlyStringFromUtf8(
                        profile_id.c_str(),
                        p_profile_id.Put()));

                DAS::DasPtr<IDasReadOnlyString> p_profile_name{};
                const auto                      profile_name =
                    json_body.at("profileName").get<std::string>();
                DAS_THROW_IF_FAILED_EC(
                    ::CreateIDasReadOnlyStringFromUtf8(
                        profile_name.c_str(),
                        p_profile_name.Put()));

                const auto data_string = json_body.at("profile").dump();
                DAS::DasPtr<IDasReadOnlyString> p_data_string{};
                DAS_THROW_IF_FAILED_EC(
                    ::CreateIDasReadOnlyStringFromUtf8(
                        data_string.c_str(),
                        p_data_string.Put()));

                DAS_THROW_IF_FAILED_EC(
                    ::CreateIDasProfile(
                        p_profile_id.Get(),
                        p_profile_name.Get(),
                        p_data_string.Get()));

                return Beast::HttpResponse::CreateSuccessResponse(nullptr);
            }
            catch (const nlohmann::json::exception& ex)
            {
                DAS_LOG_ERROR(ex.what());
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_JSON,
                    ex.what());
            }
            catch (const DasException& ex)
            {
                DAS_LOG_ERROR(ex.what());
                return Beast::HttpResponse::CreateErrorResponse(
                    ex.GetErrorCode(),
                    ex.what());
            }
        }

        Beast::HttpResponse DeleteProfile(const Beast::HttpRequest& request)
        {
            try
            {
                const auto& json_body = request.JsonBody();
                if (!json_body.contains("profileId"))
                {
                    return Beast::HttpResponse::CreateErrorResponse(
                        DAS_E_INVALID_POINTER,
                        "Missing profileId");
                }

                DAS::DasPtr<IDasReadOnlyString> p_profile_id{};
                const auto                      profile_id =
                    json_body["profileId"].get<std::string>();
                DAS_THROW_IF_FAILED_EC(
                    ::CreateIDasReadOnlyStringFromUtf8(
                        profile_id.c_str(),
                        p_profile_id.Put()));
                DAS_THROW_IF_FAILED_EC(::DeleteIDasProfile(p_profile_id.Get()));

                return Beast::HttpResponse::CreateSuccessResponse(nullptr);
            }
            catch (const DasException& ex)
            {
                DAS_LOG_ERROR(ex.what());
                return Beast::HttpResponse::CreateErrorResponse(
                    ex.GetErrorCode(),
                    ex.what());
            }
        }

        Beast::HttpResponse SetEnable(const Beast::HttpRequest& request)
        {
            try
            {
                const auto& json_body = request.JsonBody();
                const auto  profile_id = json_body.value("profileId", "");
                const auto  enabled = json_body.value("enabled", 0);

                if (profile_id != "0")
                {
                    const auto message = DAS_FMT_NS::format(
                        "Profile index out of range. Index = {}.",
                        profile_id);
                    DAS_LOG_ERROR(message.c_str());
                    return Beast::HttpResponse::CreateErrorResponse(
                        DAS_E_OUT_OF_RANGE,
                        message);
                }

                const auto result = p_task_scheduler_->SetEnabled(enabled);
                if (DAS::IsFailed(result))
                {
                    return Beast::HttpResponse::CreateErrorResponse(
                        result,
                        "Failed to set enabled");
                }

                Dto::ProfileStatusList response;
                response.code = DAS_S_OK;
                response.message = "";
                Dto::ProfileStatus status;
                status.profile_id = "0";
                status.run = p_task_scheduler_->IsTaskExecuting();
                status.enable = p_task_scheduler_->GetEnabled();
                response.data = {status};

                return Beast::HttpResponse::CreateSuccessResponse(
                    nlohmann::json(response.data));
            }
            catch (const std::exception& ex)
            {
                DAS_LOG_ERROR(ex.what());
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_UNEXPECTED,
                    ex.what());
            }
        }

        Beast::HttpResponse StartProfile(const Beast::HttpRequest& request)
        {
            try
            {
                const auto& json_body = request.JsonBody();
                const auto  profile_id = json_body.value("profileId", "");

                if (profile_id != "0")
                {
                    const auto message = DAS_FMT_NS::format(
                        "Profile index out of range. Index = {}.",
                        profile_id);
                    DAS_LOG_ERROR(message.c_str());
                    return Beast::HttpResponse::CreateErrorResponse(
                        DAS_E_OUT_OF_RANGE,
                        message);
                }

                const auto result = p_task_scheduler_->ForceStart();
                if (DAS::IsFailed(result))
                {
                    return Beast::HttpResponse::CreateErrorResponse(
                        result,
                        "Failed to start profile");
                }

                Dto::ProfileStatusList response;
                response.code = DAS_S_OK;
                response.message = "";
                Dto::ProfileStatus status;
                status.profile_id = "0";
                status.run = p_task_scheduler_->IsTaskExecuting();
                status.enable = p_task_scheduler_->GetEnabled();
                response.data = {status};

                return Beast::HttpResponse::CreateSuccessResponse(
                    nlohmann::json(response.data));
            }
            catch (const std::exception& ex)
            {
                DAS_LOG_ERROR(ex.what());
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_UNEXPECTED,
                    ex.what());
            }
        }

        Beast::HttpResponse StopProfile(const Beast::HttpRequest& request)
        {
            try
            {
                const auto& json_body = request.JsonBody();
                const auto  profile_id = json_body.value("profileId", "");

                if (profile_id != "0")
                {
                    const auto message = DAS_FMT_NS::format(
                        "Profile index out of range. Index = {}.",
                        profile_id);
                    DAS_LOG_ERROR(message.c_str());
                    return Beast::HttpResponse::CreateErrorResponse(
                        DAS_E_OUT_OF_RANGE,
                        message);
                }

                const auto result = p_task_scheduler_->RequestStop();
                if (DAS::IsFailed(result))
                {
                    return Beast::HttpResponse::CreateErrorResponse(
                        result,
                        "Failed to stop profile");
                }

                Dto::ProfileStatusList response;
                response.code = DAS_S_OK;
                response.message = "";
                Dto::ProfileStatus status;
                status.profile_id = "0";
                status.run = p_task_scheduler_->IsTaskExecuting();
                status.enable = p_task_scheduler_->GetEnabled();
                response.data = {status};

                return Beast::HttpResponse::CreateSuccessResponse(
                    nlohmann::json(response.data));
            }
            catch (const std::exception& ex)
            {
                DAS_LOG_ERROR(ex.what());
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_UNEXPECTED,
                    ex.what());
            }
        }

    private:
        static std::string ReadProperty(
            IDasTaskInfo* p_task_info,
            size_t        property_index)
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
                return "";
            }
            return p_property;
        }

        // 获取任务列表
        // Get task list
        Beast::HttpResponse GetTaskList(const Beast::HttpRequest& request)
        {
            Dto::TaskDescList response;
            response.code = DAS_S_OK;
            response.message = "";

            DAS::DasPtr<IDasTaskInfoVector> p_task_info_vector{};
            if (const auto error_code = p_task_scheduler_->GetAllWorkingTasks(
                    p_task_info_vector.Put());
                DAS::IsFailed(error_code))
            {
                response.code = error_code;
                const auto message = DAS_FMT_NS::format(
                    "GetAllWorkingTasks failed. Error code = {}.",
                    error_code);
                DAS_LOG_ERROR(message.c_str());
                response.message = message;
                return Beast::HttpResponse::CreateErrorResponse(
                    error_code,
                    message);
            }

            std::vector<Dto::TaskDesc> task_list;

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
                    response.code = error_code;
                    const auto message = DAS_FMT_NS::format(
                        "EnumByIndex failed. Error code = {}.",
                        error_code);
                    DAS_LOG_ERROR(message.c_str());
                    response.message = message;
                    return Beast::HttpResponse::CreateErrorResponse(
                        error_code,
                        message);
                }

                Dto::TaskDesc task_info;
                DasGuid       iid;
                p_task_info->GetIid(&iid);
                DAS::DasPtr<IDasReadOnlyString> p_iid_string{};
                DasGuidToString(&iid, p_iid_string.Put());
                const char* iid_string;
                p_iid_string->GetUtf8(&iid_string);
                task_info.plugin_id = iid_string;
                task_info.name = ReadProperty(
                    p_task_info.Get(),
                    DAS_TASK_INFO_PROPERTIES_NAME_INDEX);
                task_info.game_name = ReadProperty(
                    p_task_info.Get(),
                    DAS_TASK_INFO_PROPERTIES_GAME_NAME_INDEX);
                task_list.push_back(std::move(task_info));
            }

            nlohmann::json data;
            for (const auto& task : task_list)
            {
                data.push_back(task.ToJson());
            }
            return Beast::HttpResponse::CreateSuccessResponse(data);
        }
    };

} // namespace Das::Http

#endif // DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP
