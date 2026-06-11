#ifndef DAS_HTTP_CONTROLLER_DASPLUGINMANAGERCONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_DASPLUGINMANAGERCONTROLLER_HPP

#include "Config.h"
#include "beast/Request.hpp"
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/IDasPluginManagerService.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/DasJson.h>

namespace Das::Http
{

    class DasPluginManagerController
    {
    public:
        explicit DasPluginManagerController(
            IDasPluginManagerService& plugin_manager_service)
            : plugin_manager_service_(plugin_manager_service)
        {
        }

        // POST /plugin/list/get
        Beast::HttpResponse GetPluginList(const Beast::HttpRequest& request)
        {
            DasPtr<Das::ExportInterface::IDasJson> json;
            auto                                   result =
                plugin_manager_service_.ScanInstalledPlugins(json.Put());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to scan installed plugins");
            }

            DasPtr<IDasReadOnlyString> p_str;
            auto to_str_result = json->ToString(-1, p_str.Put());
            if (DAS::IsFailed(to_str_result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    to_str_result,
                    "Failed to serialize plugin list");
            }
            const char* c_str = nullptr;
            auto        get_result = p_str->GetUtf8(&c_str);
            if (DAS::IsFailed(get_result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    get_result,
                    "Failed to get plugin list string");
            }
            auto parsed = Das::Utils::ParseYyjsonFromString(c_str);
            if (!parsed)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_JSON,
                    "Failed to parse plugin list JSON");
            }
            return Beast::HttpResponse::CreateSuccessResponse(parsed.value());
        }

        // POST /plugin/update -- receive binary ZIP body
        Beast::HttpResponse UpdatePlugin(const Beast::HttpRequest& request)
        {
            auto content_type = request.GetHeader("Content-Type");
            if (!content_type.empty()
                && content_type.find("application/zip") == std::string::npos
                && content_type.find("application/octet-stream")
                       == std::string::npos)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Expected Content-Type: application/zip or "
                    "application/octet-stream");
            }

            const auto& raw_body = request.Body();
            if (raw_body.empty())
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Empty request body");
            }

            auto result = plugin_manager_service_.InstallPluginPackageData(
                reinterpret_cast<const uint8_t*>(raw_body.data()),
                static_cast<uint64_t>(raw_body.size()));
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to install plugin");
            }
            return Beast::HttpResponse::CreateSuccessResponse();
        }

        // POST /plugin/{guid}/delete
        Beast::HttpResponse DeletePlugin(const Beast::HttpRequest& request)
        {
            auto guid_str = request.GetPathParameter("guid");
            if (guid_str.empty())
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Missing GUID path parameter");
            }

            DasGuid guid;
            auto    guid_cr = DasMakeDasGuid(guid_str.c_str(), &guid);
            if (DAS::IsFailed(guid_cr))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Invalid GUID format");
            }

            auto result =
                plugin_manager_service_.MarkPluginPackageForDeletion(&guid);
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to mark plugin for deletion");
            }
            return Beast::HttpResponse::CreateSuccessResponse();
        }

        // POST /plugin/{guid}/detail
        Beast::HttpResponse GetPluginDetail(const Beast::HttpRequest& request)
        {
            auto guid_str = request.GetPathParameter("guid");
            if (guid_str.empty())
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Missing GUID path parameter");
            }

            DasGuid guid;
            auto    guid_cr = DasMakeDasGuid(guid_str.c_str(), &guid);
            if (DAS::IsFailed(guid_cr))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Invalid GUID format");
            }

            DasPtr<Das::ExportInterface::IDasJson> json;
            auto result = plugin_manager_service_.GetPluginPackageDetail(
                &guid,
                json.Put());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to get plugin detail");
            }

            DasPtr<IDasReadOnlyString> p_str;
            auto to_str_result = json->ToString(-1, p_str.Put());
            if (DAS::IsFailed(to_str_result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    to_str_result,
                    "Failed to serialize plugin detail");
            }
            const char* c_str = nullptr;
            auto        get_result = p_str->GetUtf8(&c_str);
            if (DAS::IsFailed(get_result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    get_result,
                    "Failed to get plugin detail string");
            }
            auto parsed = Das::Utils::ParseYyjsonFromString(c_str);
            if (!parsed)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_JSON,
                    "Failed to parse plugin detail JSON");
            }
            return Beast::HttpResponse::CreateSuccessResponse(parsed.value());
        }

    private:
        IDasPluginManagerService& plugin_manager_service_;
    };

} // namespace Das::Http

#endif
