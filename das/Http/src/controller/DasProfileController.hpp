#ifndef DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP

#include "../service/DasHttpJson.h"
#include "Config.h"
#include "beast/Request.hpp"
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/IDasSettingsService.h>
#include <das/Utils/CommonUtils.hpp>
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

            DasPtr<IDasReadOnlyString> p_str;
            auto to_str_result = json->ToString(-1, p_str.Put());
            if (DAS::IsFailed(to_str_result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    to_str_result,
                    "Failed to serialize profile list");
            }
            const char* c_str = nullptr;
            auto        get_result = p_str->GetUtf8(&c_str);
            if (DAS::IsFailed(get_result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    get_result,
                    "Failed to get profile list string");
            }
            auto parsed = nlohmann::json::parse(c_str);
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
            auto                       cr = CreateIDasReadOnlyStringFromUtf8(
                profile_id.c_str(),
                p_pid.Put());
            if (DAS::IsFailed(cr))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    cr,
                    "Failed to create string");
            }

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
            auto                       cr = CreateIDasReadOnlyStringFromUtf8(
                profile_id.c_str(),
                p_pid.Put());
            if (DAS::IsFailed(cr))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    cr,
                    "Failed to create string");
            }

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
            auto                       cr =
                CreateIDasReadOnlyStringFromUtf8(pid.c_str(), p_pid.Put());
            if (DAS::IsFailed(cr))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    cr,
                    "Failed to create string");
            }

            DasPtr<Das::ExportInterface::IDasJson> json;
            auto result = settings_service_.GetProfile(p_pid.Get(), json.Put());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to get profile");
            }

            DasPtr<IDasReadOnlyString> p_str;
            auto to_str_result = json->ToString(-1, p_str.Put());
            if (DAS::IsFailed(to_str_result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    to_str_result,
                    "Failed to serialize profile");
            }
            const char* c_str = nullptr;
            auto        get_result = p_str->GetUtf8(&c_str);
            if (DAS::IsFailed(get_result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    get_result,
                    "Failed to get profile string");
            }
            auto parsed = nlohmann::json::parse(c_str);
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
            auto                       pid_cr =
                CreateIDasReadOnlyStringFromUtf8(pid.c_str(), p_pid.Put());
            if (DAS::IsFailed(pid_cr))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    pid_cr,
                    "Failed to create string");
            }

            DasPtr<Das::ExportInterface::IDasJson> json_data =
                DasPtr<Das::ExportInterface::IDasJson>::Attach(
                    DasHttpJson::MakeRaw(body));

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
            auto guid_str = request.GetPathParameter("guid");
            if (pid != "0")
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Profile ID must be 0 in v1.2");
            }

            DasPtr<IDasReadOnlyString> p_pid;
            auto                       pid_cr =
                CreateIDasReadOnlyStringFromUtf8(pid.c_str(), p_pid.Put());
            if (DAS::IsFailed(pid_cr))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    pid_cr,
                    "Failed to create string");
            }

            // Parse GUID string into DasGuid for service boundary
            DasGuid guid;
            auto    guid_cr = DasMakeDasGuid(guid_str.c_str(), &guid);
            if (DAS::IsFailed(guid_cr))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    guid_cr,
                    "Invalid GUID format");
            }

            DasPtr<Das::ExportInterface::IDasJson> json;
            auto result = settings_service_.GetPluginSettings(
                p_pid.Get(),
                &guid,
                json.Put());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to get plugin settings");
            }

            DasPtr<IDasReadOnlyString> p_str;
            auto to_str_result = json->ToString(-1, p_str.Put());
            if (DAS::IsFailed(to_str_result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    to_str_result,
                    "Failed to serialize plugin settings");
            }
            const char* c_str = nullptr;
            auto        get_result = p_str->GetUtf8(&c_str);
            if (DAS::IsFailed(get_result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    get_result,
                    "Failed to get plugin settings string");
            }
            auto parsed = nlohmann::json::parse(c_str);

            if (result == DAS_S_FALSE)
            {
                return Beast::HttpResponse::CreateSuccessResponse(
                    DAS_S_FALSE,
                    "Plugin settings were invalid and restored from manifest "
                    "defaults",
                    parsed);
            }
            return Beast::HttpResponse::CreateSuccessResponse(parsed);
        }

        Beast::HttpResponse UpdatePluginSettings(
            const Beast::HttpRequest& request)
        {
            auto pid = request.GetPathParameter("pid");
            auto guid_str = request.GetPathParameter("guid");
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
            auto                       pid_cr =
                CreateIDasReadOnlyStringFromUtf8(pid.c_str(), p_pid.Put());
            if (DAS::IsFailed(pid_cr))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    pid_cr,
                    "Failed to create string");
            }

            // Parse GUID string into DasGuid for service boundary
            DasGuid guid;
            auto    guid_cr = DasMakeDasGuid(guid_str.c_str(), &guid);
            if (DAS::IsFailed(guid_cr))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    guid_cr,
                    "Invalid GUID format");
            }

            DasPtr<Das::ExportInterface::IDasJson> json_data =
                DasPtr<Das::ExportInterface::IDasJson>::Attach(
                    DasHttpJson::MakeRaw(body));

            auto result = settings_service_.UpdatePluginSettings(
                p_pid.Get(),
                &guid,
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
