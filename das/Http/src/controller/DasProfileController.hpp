#ifndef DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_DASPROFILECONTROLLER_HPP

#include "../service/DasHttpJson.h"
#include "Config.h"
#include "beast/Request.hpp"
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/IDasSettingsService.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/DasJsonCore.h>
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
            auto parsed = Das::Utils::ParseYyjsonFromString(c_str);
            if (!parsed)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_JSON,
                    "Failed to parse profile list JSON");
            }
            return Beast::HttpResponse::CreateSuccessResponse(parsed.value());
        }

        Beast::HttpResponse CreateProfile(const Beast::HttpRequest& request)
        {
            const auto& body_raw = request.JsonBody();
            auto        body_obj_opt = body_raw.as_object();
            if (!body_obj_opt)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Request body must be a JSON object");
            }
            const auto& body = body_obj_opt.value();
            if (body["profileId"].is_null() || !body["profileId"].is_string())
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Missing or invalid 'profileId' field");
            }

            auto profile_id_opt = body["profileId"].as_string();
            if (!profile_id_opt)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Missing or invalid 'profileId' field");
            }
            std::string                profile_id(profile_id_opt.value());
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

            // name is optional; when absent it stays nullptr and the
            // service applies its default.
            DasPtr<IDasReadOnlyString> p_name;
            {
                auto name_val = body["name"];
                if (name_val.is_string())
                {
                    auto name_opt = name_val.as_string();
                    if (name_opt)
                    {
                        std::string name(name_opt.value());
                        auto        name_cr =
                            CreateIDasReadOnlyStringFromUtf8(
                                name.c_str(),
                                p_name.Put());
                        if (DAS::IsFailed(name_cr))
                        {
                            return Beast::HttpResponse::CreateErrorResponse(
                                name_cr,
                                "Failed to create name string");
                        }
                    }
                }
            }

            auto result =
                settings_service_.CreateProfile(p_pid.Get(), p_name.Get());
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
            auto parsed = Das::Utils::ParseYyjsonFromString(c_str);
            if (!parsed)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_JSON,
                    "Failed to parse profile JSON");
            }
            return Beast::HttpResponse::CreateSuccessResponse(parsed.value());
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

        Beast::HttpResponse RenameProfile(const Beast::HttpRequest& request)
        {
            auto pid = request.GetPathParameter("pid");
            if (pid != "0")
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Profile ID must be 0 in v1.2");
            }

            const auto& body_raw = request.JsonBody();
            auto        body_obj_opt = body_raw.as_object();
            if (!body_obj_opt)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Request body must be a JSON object");
            }
            const auto& body = body_obj_opt.value();
            if (body["name"].is_null() || !body["name"].is_string())
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Missing or invalid 'name' field");
            }

            auto name_opt = body["name"].as_string();
            if (!name_opt)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Missing or invalid 'name' field");
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

            std::string                name(name_opt.value());
            DasPtr<IDasReadOnlyString> p_name;
            auto                       name_cr =
                CreateIDasReadOnlyStringFromUtf8(name.c_str(), p_name.Put());
            if (DAS::IsFailed(name_cr))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    name_cr,
                    "Failed to create name string");
            }

            auto result =
                settings_service_.RenameProfile(p_pid.Get(), p_name.Get());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to rename profile");
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
            auto parsed = Das::Utils::ParseYyjsonFromString(c_str);

            if (!parsed)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_JSON,
                    "Failed to parse plugin settings JSON");
            }

            if (result == DAS_S_FALSE)
            {
                return Beast::HttpResponse::CreateSuccessResponse(
                    DAS_S_FALSE,
                    "Plugin settings were invalid and restored to an "
                    "empty object",
                    parsed.value());
            }
            return Beast::HttpResponse::CreateSuccessResponse(parsed.value());
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
