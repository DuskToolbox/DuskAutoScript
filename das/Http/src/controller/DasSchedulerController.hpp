#ifndef DAS_HTTP_CONTROLLER_DASSCHEDULERCONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_DASSCHEDULERCONTROLLER_HPP

#include "Config.h"
#include "beast/Request.hpp"
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/IDasSchedulerService.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/IDasGuidVector.h>
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

            auto result = scheduler_.Start();
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

            auto result = scheduler_.Stop();
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

            IDasSchedulerService::SchedulerState state =
                IDasSchedulerService::SchedulerState::Stopped;
            auto result = scheduler_.GetState(&state);
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to get scheduler state");
            }

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

                    DasGuid guid;
                    auto    parse_result =
                        DasMakeDasGuid(item.get<std::string>().c_str(), &guid);
                    if (DAS::IsFailed(parse_result))
                    {
                        DAS_CORE_LOG_WARN(
                            "Invalid GUID '{}' in disabled_guids",
                            item.get<std::string>());
                        continue;
                    }
                    disabled_guids.push_back(guid);
                }
            }

            DasPtr<IDasReadOnlyString> p_plugin_dir;
            auto                       u8_path = plugin_dir_.u8string();
            auto dir_cr = CreateIDasReadOnlyStringFromUtf8(
                reinterpret_cast<const char*>(u8_path.c_str()),
                p_plugin_dir.Put());
            if (DAS::IsFailed(dir_cr))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    dir_cr,
                    "Failed to create plugin dir string");
            }

            DasPtr<Das::ExportInterface::IDasGuidVector> p_guid_vec;
            auto gv_result = CreateIDasGuidVector(
                disabled_guids.empty() ? nullptr : disabled_guids.data(),
                disabled_guids.size(),
                p_guid_vec.Put());
            if (DAS::IsFailed(gv_result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    gv_result,
                    "Failed to create GUID vector");
            }

            DasPtr<Das::ExportInterface::IDasReadOnlyGuidVector>
                p_readonly_guids;
            p_guid_vec->ToConst(p_readonly_guids.Put());

            auto result = scheduler_.Initialize(
                p_plugin_dir.Get(),
                p_readonly_guids.Get());
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
