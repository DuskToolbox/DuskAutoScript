#ifndef DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP

#include "Config.h"
#include "beast/Request.hpp"
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/IDasSettingsService.h>
#include <nlohmann/json.hpp>
#include <string>

namespace Das::Http
{

    class DasProfileController
    {
    public:
        explicit DasProfileController(IDasSettingsService& settings_svc)
            : settings_service_(settings_svc)
        {
        }

        Beast::HttpResponse GetProfileList(const Beast::HttpRequest& request)
        {
            DasPtr<Das::ExportInterface::IDasJson> json;
            auto result = settings_service_.GetProfileList(json.Put());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to get profile list");
            }

            IDasReadOnlyString* p_str = nullptr;
            auto                to_str_result = json->ToString(-1, &p_str);
            if (DAS::IsFailed(to_str_result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    to_str_result,
                    "Failed to serialize profile list");
            }
            const char* c_str = nullptr;
            p_str->GetUtf8(&c_str);
            auto parsed = nlohmann::json::parse(c_str);
            p_str->Release();
            return Beast::HttpResponse::CreateSuccessResponse(parsed);
        }

        Beast::HttpResponse CreateProfile(const Beast::HttpRequest& request)
        {
            const auto& body = request.JsonBody();
            if (!body.contains("profileId") || !body["profileId"].is_string())
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Missing or invalid 'profileId' field");
            }

            auto profile_id = body["profileId"].get<std::string>();
            DasPtr<IDasReadOnlyString> p_pid;
            CreateIDasReadOnlyStringFromUtf8(profile_id.c_str(), p_pid.Put());
            auto result = settings_service_.CreateProfile(p_pid.Get());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to create profile");
            }
            return Beast::HttpResponse::CreateSuccessResponse();
        }

        Beast::HttpResponse DeleteProfile(const Beast::HttpRequest& request)
        {
            const auto& body = request.JsonBody();
            if (!body.contains("profileId") || !body["profileId"].is_string())
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Missing or invalid 'profileId' field");
            }

            auto profile_id = body["profileId"].get<std::string>();
            DasPtr<IDasReadOnlyString> p_pid;
            CreateIDasReadOnlyStringFromUtf8(profile_id.c_str(), p_pid.Put());
            auto result = settings_service_.DeleteProfile(p_pid.Get());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to delete profile");
            }
            return Beast::HttpResponse::CreateSuccessResponse();
        }

        Beast::HttpResponse GetProfile(const Beast::HttpRequest& request)
        {
            auto pid = request.GetPathParameter("pid");
            if (pid != "0")
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Profile ID must be 0 in v1.2");
            }

            DasPtr<IDasReadOnlyString> p_pid;
            CreateIDasReadOnlyStringFromUtf8(pid.c_str(), p_pid.Put());

            DasPtr<Das::ExportInterface::IDasJson> json;
            auto result = settings_service_.GetProfile(p_pid.Get(), json.Put());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to get profile");
            }

            IDasReadOnlyString* p_str = nullptr;
            json->ToString(-1, &p_str);
            const char* c_str = nullptr;
            p_str->GetUtf8(&c_str);
            auto parsed = nlohmann::json::parse(c_str);
            p_str->Release();
            return Beast::HttpResponse::CreateSuccessResponse(parsed);
        }

        Beast::HttpResponse UpdateProfile(const Beast::HttpRequest& request)
        {
            auto pid = request.GetPathParameter("pid");
            if (pid != "0")
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Profile ID must be 0 in v1.2");
            }

            const auto& body = request.JsonBody();
            if (!body.is_object())
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Invalid request body");
            }

            DasPtr<IDasReadOnlyString> p_pid;
            CreateIDasReadOnlyStringFromUtf8(pid.c_str(), p_pid.Put());

            DasPtr<Das::ExportInterface::IDasJson> json_data;
            ParseDasJsonFromString(body.dump().c_str(), json_data.Put());

            auto result =
                settings_service_.UpdateProfile(p_pid.Get(), json_data.Get());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to update profile");
            }
            return Beast::HttpResponse::CreateSuccessResponse();
        }

        Beast::HttpResponse GetPluginSettings(const Beast::HttpRequest& request)
        {
            auto pid = request.GetPathParameter("pid");
            auto guid = request.GetPathParameter("guid");
            if (pid != "0")
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Profile ID must be 0 in v1.2");
            }

            DasPtr<IDasReadOnlyString> p_pid;
            CreateIDasReadOnlyStringFromUtf8(pid.c_str(), p_pid.Put());
            DasPtr<IDasReadOnlyString> p_guid;
            CreateIDasReadOnlyStringFromUtf8(guid.c_str(), p_guid.Put());

            DasPtr<Das::ExportInterface::IDasJson> json;
            auto result = settings_service_.GetPluginSettings(
                p_pid.Get(),
                p_guid.Get(),
                json.Put());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to get plugin settings");
            }

            IDasReadOnlyString* p_str = nullptr;
            json->ToString(-1, &p_str);
            const char* c_str = nullptr;
            p_str->GetUtf8(&c_str);
            auto parsed = nlohmann::json::parse(c_str);
            p_str->Release();
            return Beast::HttpResponse::CreateSuccessResponse(parsed);
        }

        Beast::HttpResponse UpdatePluginSettings(
            const Beast::HttpRequest& request)
        {
            auto pid = request.GetPathParameter("pid");
            auto guid = request.GetPathParameter("guid");
            if (pid != "0")
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Profile ID must be 0 in v1.2");
            }

            const auto& body = request.JsonBody();
            if (!body.is_object())
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Invalid request body");
            }

            DasPtr<IDasReadOnlyString> p_pid;
            CreateIDasReadOnlyStringFromUtf8(pid.c_str(), p_pid.Put());
            DasPtr<IDasReadOnlyString> p_guid;
            CreateIDasReadOnlyStringFromUtf8(guid.c_str(), p_guid.Put());

            DasPtr<Das::ExportInterface::IDasJson> json_data;
            ParseDasJsonFromString(body.dump().c_str(), json_data.Put());

            auto result = settings_service_.UpdatePluginSettings(
                p_pid.Get(),
                p_guid.Get(),
                json_data.Get());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to update plugin settings");
            }
            return Beast::HttpResponse::CreateSuccessResponse();
        }

    private:
        IDasSettingsService& settings_service_;
    };

} // namespace Das::Http

#endif // DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP
