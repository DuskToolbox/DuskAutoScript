#ifndef DAS_HTTP_CONTROLLER_DASSCHEDULERCONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_DASSCHEDULERCONTROLLER_HPP

#include "Config.h"
#include "beast/Request.hpp"
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/IDasSchedulerService.h>
#include <das/Utils/fmt.h>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Das::Http
{

    class DasSchedulerController
    {
    public:
        explicit DasSchedulerController(
            IDasSchedulerService&        scheduler,
            const std::filesystem::path& plugin_dir)
            : scheduler_(scheduler), plugin_dir_(plugin_dir)
        {
        }

        Beast::HttpResponse Enable(const Beast::HttpRequest& request)
        {
            auto profile = request.GetPathParameter("profile");
            if (profile != "0")
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Profile ID must be 0 in v1.2");
            }

            auto result = scheduler_.Enable();
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to enable scheduler");
            }
            return Beast::HttpResponse::CreateSuccessResponse();
        }

        Beast::HttpResponse Disable(const Beast::HttpRequest& request)
        {
            auto profile = request.GetPathParameter("profile");
            if (profile != "0")
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Profile ID must be 0 in v1.2");
            }

            auto result = scheduler_.Disable();
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to disable scheduler");
            }
            return Beast::HttpResponse::CreateSuccessResponse();
        }

        Beast::HttpResponse Status(const Beast::HttpRequest& request)
        {
            auto profile = request.GetPathParameter("profile");
            if (profile != "0")
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Profile ID must be 0 in v1.2");
            }

            auto           state = scheduler_.Status();
            nlohmann::json data;
            data["state"] =
                (state == IDasSchedulerService::SchedulerState::Running)
                    ? "running"
                    : "stopped";
            return Beast::HttpResponse::CreateSuccessResponse(data);
        }

        Beast::HttpResponse Initialize(const Beast::HttpRequest& request)
        {
            auto profile = request.GetPathParameter("profile");
            if (profile != "0")
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Profile ID must be 0 in v1.2");
            }

            const auto& body = request.JsonBody();

            std::vector<DasGuid> disabled_guids;
            if (body.contains("disabled_guids"))
            {
                if (!body["disabled_guids"].is_array())
                {
                    return Beast::HttpResponse::CreateErrorResponse(
                        DAS_E_INVALID_ARGUMENT,
                        "disabled_guids must be an array");
                }

                for (const auto& item : body["disabled_guids"])
                {
                    if (!item.is_string())
                    {
                        DAS_CORE_LOG_WARN(
                            "Skipping non-string element in disabled_guids");
                        continue;
                    }

                    try
                    {
                        auto guid =
                            Das::Core::ForeignInterfaceHost::MakeDasGuid(
                                item.get<std::string>());
                        disabled_guids.push_back(guid);
                    }
                    catch (const std::exception& e)
                    {
                        DAS_CORE_LOG_WARN(
                            "Invalid GUID '{}' in disabled_guids: {}",
                            item.get<std::string>(),
                            e.what());
                    }
                }
            }

            auto result = scheduler_.Initialize(plugin_dir_, disabled_guids);
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to initialize scheduler");
            }
            return Beast::HttpResponse::CreateSuccessResponse();
        }

    private:
        IDasSchedulerService& scheduler_;
        std::filesystem::path plugin_dir_;
    };

} // namespace Das::Http

#endif
