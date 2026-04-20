#ifndef DAS_HTTP_CONTROLLER_UISETTINGSCONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_UISETTINGSCONTROLLER_HPP

#include "Config.h"
#include "beast/Request.hpp"
#include <das/IDasSettingsService.h>
#include <nlohmann/json.hpp>
#include <string>

namespace Das::Http
{

    struct DasUiSettingsController
    {
        explicit DasUiSettingsController(IDasSettingsService& settings_svc)
            : settings_service_(settings_svc)
        {
        }

        Beast::HttpResponse V1SettingsGet(const Beast::HttpRequest& request)
        {
            auto json_str = settings_service_.GetGlobalSettings();
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

            auto result = settings_service_.UpdateGlobalSettings(body.dump());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to update settings");
            }
            return Beast::HttpResponse::CreateSuccessResponse();
        }

    private:
        IDasSettingsService& settings_service_;
    };

} // namespace Das::Http

#endif // DAS_HTTP_CONTROLLER_UISETTINGSCONTROLLER_HPP
