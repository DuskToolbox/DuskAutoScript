#ifndef DAS_HTTP_CONTROLLER_UISETTINGSCONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_UISETTINGSCONTROLLER_HPP

#include "Config.h"
#include "beast/Request.hpp"
#include <das/Core/SettingsManager/SettingsManager.h>
#include <nlohmann/json.hpp>
#include <string>

namespace Das::Http
{

    struct DasUiSettingsController
    {
        explicit DasUiSettingsController(
            Das::Core::SettingsManager::SettingsManager& sm)
            : settings_manager_(sm)
        {
        }

        Beast::HttpResponse V1SettingsGet(const Beast::HttpRequest& request)
        {
            auto json_str = settings_manager_.GetGlobalSettings();
            try
            {
                auto json = nlohmann::json::parse(json_str);
                return Beast::HttpResponse::CreateSuccessResponse(json);
            }
            catch (const nlohmann::json::exception&)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_JSON,
                    "Failed to parse global settings");
            }
        }

        Beast::HttpResponse V1SettingsUpdate(const Beast::HttpRequest& request)
        {
            const auto& body = request.JsonBody();
            if (!body.is_object())
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Invalid request body");
            }

            auto result = settings_manager_.UpdateGlobalSettings(body.dump());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to update settings");
            }
            return Beast::HttpResponse::CreateSuccessResponse();
        }

    private:
        Das::Core::SettingsManager::SettingsManager& settings_manager_;
    };

} // namespace Das::Http

#endif // DAS_HTTP_CONTROLLER_UISETTINGSCONTROLLER_HPP
