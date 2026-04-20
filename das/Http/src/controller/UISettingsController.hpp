#ifndef DAS_HTTP_CONTROLLER_UISETTINGSCONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_UISETTINGSCONTROLLER_HPP

#include "../service/DasHttpJson.h"
#include "Config.h"
#include "beast/Request.hpp"
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
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
            DasPtr<Das::ExportInterface::IDasJson> json;
            auto result = settings_service_.GetGlobalSettings(json.Put());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to get global settings");
            }

            IDasReadOnlyString* p_str = nullptr;
            auto                to_str_result = json->ToString(-1, &p_str);
            if (DAS::IsFailed(to_str_result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    to_str_result,
                    "Failed to serialize settings");
            }
            const char* c_str = nullptr;
            auto        get_result = p_str->GetUtf8(&c_str);
            if (DAS::IsFailed(get_result) || !c_str)
            {
                p_str->Release();
                return Beast::HttpResponse::CreateErrorResponse(
                    get_result,
                    "Failed to get settings string");
            }
            auto parsed = nlohmann::json::parse(c_str);
            p_str->Release();
            return Beast::HttpResponse::CreateSuccessResponse(parsed);
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

            DasPtr<Das::ExportInterface::IDasJson> json_data =
                DasPtr<Das::ExportInterface::IDasJson>::Attach(
                    DasHttpJson::MakeRaw(body));

            auto result =
                settings_service_.UpdateGlobalSettings(json_data.Get());
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
