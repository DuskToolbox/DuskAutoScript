#ifndef DAS_HTTP_CONTROLLER_DASPLUGINMANAGERCONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_DASPLUGINMANAGERCONTROLLER_HPP

#include "Config.h"
#include "beast/Request.hpp"
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/PluginScanner.h>
#include <das/Core/ForeignInterfaceHost/PluginZipExtractor.h>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace Das::Http
{

    class DasPluginManagerController
    {
    public:
        explicit DasPluginManagerController(
            const std::filesystem::path& plugin_dir)
            : plugin_dir_(plugin_dir)
        {
        }

        // POST /plugin/list/get
        Beast::HttpResponse GetPluginList(const Beast::HttpRequest& request)
        {
            auto descs =
                Das::Core::ForeignInterfaceHost::ScanPlugins(plugin_dir_);
            nlohmann::json data = nlohmann::json::array();
            for (const auto& desc : descs)
            {
                data.push_back(
                    Das::Core::ForeignInterfaceHost::PluginPackageDescToJson(
                        desc));
            }
            return Beast::HttpResponse::CreateSuccessResponse(data);
        }

        // POST /plugin/update -- receive binary ZIP body
        Beast::HttpResponse UpdatePlugin(const Beast::HttpRequest& request)
        {
            // Must use Body() instead of JsonBody():
            // binary ZIP data is not JSON, JsonBody() parse failure returns
            // empty object
            const auto& raw_body = request.Body();
            if (raw_body.empty())
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Empty request body");
            }

            auto result = Das::Core::ForeignInterfaceHost::InstallPlugin(
                plugin_dir_,
                std::string_view(raw_body.data(), raw_body.size()));
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
            try
            {
                guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(guid_str);
            }
            catch (const std::exception&)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Invalid GUID format");
            }

            auto result = Das::Core::ForeignInterfaceHost::MarkForDeletion(
                plugin_dir_,
                guid);
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to mark plugin for deletion");
            }
            return Beast::HttpResponse::CreateSuccessResponse();
        }

    private:
        std::filesystem::path plugin_dir_;
    };

} // namespace Das::Http

#endif
