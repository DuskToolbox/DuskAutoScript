#ifndef DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP

#include "Config.h"
#include "beast/Request.hpp"
#include <das/Core/SettingsManager/SettingsManager.h>
#include <nlohmann/json.hpp>
#include <string>

namespace Das::Http
{

    class DasProfileController
    {
    public:
        explicit DasProfileController(
            Das::Core::SettingsManager::SettingsManager& sm)
            : settings_manager_(sm)
        {
        }

        Beast::HttpResponse GetProfileList(const Beast::HttpRequest& request)
        {
            auto json_str = settings_manager_.GetProfileList();
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
            auto result = settings_manager_.CreateProfile(profile_id);
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
            auto result = settings_manager_.DeleteProfile(profile_id);
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

            auto json_str = settings_manager_.GetProfile(pid);
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

            auto result = settings_manager_.UpdateProfile(pid, body.dump());
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

            auto json_str = settings_manager_.GetPluginSettings(pid, guid);
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
                settings_manager_.UpdatePluginSettings(pid, guid, body.dump());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to update plugin settings");
            }
            return Beast::HttpResponse::CreateSuccessResponse();
        }

    private:
        Das::Core::SettingsManager::SettingsManager& settings_manager_;
    };

} // namespace Das::Http

#endif // DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP
