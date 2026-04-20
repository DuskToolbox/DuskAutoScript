#ifndef DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP

#include "Config.h"
#include "beast/Request.hpp"
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
            auto json_str = settings_service_.GetProfileList();
            try
            {
                auto json = nlohmann::json::parse(json_str);
                return Beast::HttpResponse::CreateSuccessResponse(json);
            }
            catch (const nlohmann::json::exception&)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_JSON,
                    "Failed to parse profile list");
            }
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
            auto result = settings_service_.CreateProfile(profile_id);
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
            auto result = settings_service_.DeleteProfile(profile_id);
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

            auto json_str = settings_service_.GetProfile(pid);
            try
            {
                auto json = nlohmann::json::parse(json_str);
                return Beast::HttpResponse::CreateSuccessResponse(json);
            }
            catch (const nlohmann::json::exception&)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_JSON,
                    "Failed to parse profile data");
            }
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

            auto result = settings_service_.UpdateProfile(pid, body.dump());
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

            auto json_str = settings_service_.GetPluginSettings(pid, guid);
            try
            {
                auto json = nlohmann::json::parse(json_str);
                return Beast::HttpResponse::CreateSuccessResponse(json);
            }
            catch (const nlohmann::json::exception&)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_JSON,
                    "Failed to parse plugin settings");
            }
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

            auto result =
                settings_service_.UpdatePluginSettings(pid, guid, body.dump());
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
