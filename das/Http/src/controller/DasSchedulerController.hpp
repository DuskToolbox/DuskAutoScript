#ifndef DAS_HTTP_CONTROLLER_DASSCHEDULERCONTROLLER_HPP
#define DAS_HTTP_CONTROLLER_DASSCHEDULERCONTROLLER_HPP

#include "Config.h"
#include "beast/Request.hpp"
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/IDasSchedulerService.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/fmt.h>
#include <das/_autogen/idl/abi/IDasGuidVector.h>
#include <filesystem>
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

        // ── Lifecycle ──

        Beast::HttpResponse Initialize(const Beast::HttpRequest& request)
        {
            auto profile = request.GetPathParameter("profile");
            if (profile != "0")
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

            std::vector<DasGuid> disabled_guids;
            if (body.contains("disabledGuids")
                && !body["disabledGuids"].is_null())
            {
                auto dg_arr_opt = body["disabledGuids"].as_array();
                if (!dg_arr_opt)
                {
                    return Beast::HttpResponse::CreateErrorResponse(
                        DAS_E_INVALID_ARGUMENT,
                        "disabledGuids must be an array");
                }

                for (const auto& item : dg_arr_opt.value())
                {
                    if (!item.is_string())
                    {
                        DAS_LOG_WARNING(
                            "Skipping non-string element in "
                            "disabledGuids");
                        continue;
                    }

                    DasGuid guid;
                    auto    str_opt = item.as_string();
                    if (!str_opt)
                    {
                        continue;
                    }
                    auto parse_result = DasMakeDasGuid(
                        std::string(str_opt.value()).c_str(),
                        &guid);
                    if (DAS::IsFailed(parse_result))
                    {
                        DAS_LOG_WARNING(
                            DAS_FMT_NS::format(
                                "Invalid GUID '{}' in disabledGuids",
                                std::string(str_opt.value()))
                                .c_str());
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

        Beast::HttpResponse Start(const Beast::HttpRequest& request)
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
                    "Failed to start scheduler");
            }
            return Beast::HttpResponse::CreateSuccessResponse();
        }

        Beast::HttpResponse Stop(const Beast::HttpRequest& request)
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
                    "Failed to stop scheduler");
            }
            return Beast::HttpResponse::CreateSuccessResponse();
        }

        // ── State query ──

        Beast::HttpResponse Get(const Beast::HttpRequest& request)
        {
            auto profile = request.GetPathParameter("profile");
            if (profile != "0")
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Profile ID must be 0 in v1.2");
            }

            DasPtr<IDasReadOnlyString> p_json;
            auto                       result = scheduler_.Get(p_json.Put());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to get scheduler state");
            }

            const char* c_str = nullptr;
            auto        get_result = p_json->GetUtf8(&c_str);
            if (DAS::IsFailed(get_result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    get_result,
                    "Failed to get scheduler state string");
            }

            auto parsed = Das::Utils::ParseYyjsonFromString(c_str);
            if (!parsed)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_JSON,
                    "Failed to parse scheduler state JSON");
            }
            return Beast::HttpResponse::CreateSuccessResponse(parsed.value());
        }

        Beast::HttpResponse RepositoryGet(const Beast::HttpRequest& request)
        {
            return DispatchRepositoryBody(
                request,
                "get",
                [this](
                    IDasReadOnlyString*,
                    IDasReadOnlyString** pp_out)
                { return scheduler_.GetTaskRepository(pp_out); });
        }

        Beast::HttpResponse RepositoryCreate(const Beast::HttpRequest& request)
        {
            return DispatchRepositoryBody(
                request,
                "create",
                [this](
                    IDasReadOnlyString* p_body,
                    IDasReadOnlyString** pp_out)
                {
                    return scheduler_.CreateRepositoryEntry(
                        p_body,
                        pp_out);
                });
        }

        // ── Task instance mutations ──

        Beast::HttpResponse AddTask(const Beast::HttpRequest& request)
        {
            auto profile = request.GetPathParameter("profile");
            auto task_guid_str = request.GetPathParameter("taskGuid");
            if (profile != "0")
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Profile ID must be 0 in v1.2");
            }

            DasGuid task_guid;
            auto    guid_cr = DasMakeDasGuid(task_guid_str.c_str(), &task_guid);
            if (DAS::IsFailed(guid_cr))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Invalid task GUID format");
            }

            int64_t out_task_id = 0;
            auto    result = scheduler_.AddTask(task_guid, &out_task_id);
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to add task");
            }

            auto data = Das::Utils::MakeYyjsonObject();
            auto data_obj_opt = data.as_object();
            if (data_obj_opt)
            {
                data_obj_opt.value()["taskId"] =
                    static_cast<int64_t>(out_task_id);
            }
            return Beast::HttpResponse::CreateSuccessResponse(data);
        }

        Beast::HttpResponse DeleteTask(const Beast::HttpRequest& request)
        {
            auto profile = request.GetPathParameter("profile");
            auto task_id_str = request.GetPathParameter("taskId");
            if (profile != "0")
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Profile ID must be 0 in v1.2");
            }

            int64_t task_id = 0;
            try
            {
                task_id = std::stoll(task_id_str);
            }
            catch (const std::exception&)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Invalid task ID format");
            }

            auto result = scheduler_.DeleteTask(task_id);
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to delete task");
            }
            return Beast::HttpResponse::CreateSuccessResponse();
        }

        Beast::HttpResponse UpdateTaskProperties(
            const Beast::HttpRequest& request)
        {
            auto profile = request.GetPathParameter("profile");
            auto task_id_str = request.GetPathParameter("taskId");
            if (profile != "0")
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Profile ID must be 0 in v1.2");
            }

            int64_t task_id = 0;
            try
            {
                task_id = std::stoll(task_id_str);
            }
            catch (const std::exception&)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Invalid task ID format");
            }

            const auto& body = request.JsonBody();
            if (!body.is_object())
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Request body must be a JSON object");
            }

            auto props_opt = Das::Utils::SerializeYyjsonValue(body);
            if (!props_opt)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_JSON,
                    "Failed to serialize request body");
            }

            DasPtr<IDasReadOnlyString> p_props;
            auto                       cr = CreateIDasReadOnlyStringFromUtf8(
                props_opt.value().c_str(),
                p_props.Put());
            if (DAS::IsFailed(cr))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    cr,
                    "Failed to create properties string");
            }

            auto result =
                scheduler_.UpdateTaskProperties(task_id, p_props.Get());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to update task properties");
            }
            return Beast::HttpResponse::CreateSuccessResponse();
        }

        Beast::HttpResponse UpdateTaskInternalProperties(
            const Beast::HttpRequest& request)
        {
            auto profile = request.GetPathParameter("profile");
            auto task_id_str = request.GetPathParameter("taskId");
            if (profile != "0")
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Profile ID must be 0 in v1.2");
            }

            int64_t task_id = 0;
            try
            {
                task_id = std::stoll(task_id_str);
            }
            catch (const std::exception&)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Invalid task ID format");
            }

            const auto& body = request.JsonBody();
            if (!body.is_object())
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Request body must be a JSON object");
            }

            auto props_opt = Das::Utils::SerializeYyjsonValue(body);
            if (!props_opt)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_JSON,
                    "Failed to serialize request body");
            }

            DasPtr<IDasReadOnlyString> p_props;
            auto                       cr = CreateIDasReadOnlyStringFromUtf8(
                props_opt.value().c_str(),
                p_props.Put());
            if (DAS::IsFailed(cr))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    cr,
                    "Failed to create properties string");
            }

            auto result =
                scheduler_.UpdateTaskInternalProperties(task_id, p_props.Get());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to update task internal properties");
            }
            return Beast::HttpResponse::CreateSuccessResponse();
        }

        // HTTP contract: task authoring endpoints.
        // Method/path:
        //   POST /api/v1/scheduler/{profile}/tasks/{taskId}/authoring/get
        //   POST /api/v1/scheduler/{profile}/tasks/{taskId}/authoring/apply
        //   POST /api/v1/scheduler/{profile}/tasks/{taskId}/authoring/compile
        // request body: JSON object; apply carries baseRevision plus change
        // payload, get/compile carry provider request context.
        // success response: standard envelope with Scheduler-returned lower
        // camelCase JSON in data.
        // errorKind matrix: taskNotFound, capabilityMissing,
        // revisionConflict, authoringValidationFailed, pluginFailure,
        // componentFailure, persistenceFailure.
        // HTTP thread boundary: controller validates and delegates to
        // IDasSchedulerService only; plugin authoring logic runs in
        // SchedulerService, not inside HTTP handlers.
        Beast::HttpResponse AuthoringGet(const Beast::HttpRequest& request)
        {
            return DispatchAuthoring(
                request,
                "get",
                [this](
                    int64_t task_id,
                    IDasReadOnlyString* p_body,
                    IDasReadOnlyString** pp_out)
                {
                    return scheduler_.GetTaskAuthoringDocument(
                        task_id,
                        p_body,
                        pp_out);
                });
        }

        Beast::HttpResponse AuthoringApply(const Beast::HttpRequest& request)
        {
            return DispatchAuthoring(
                request,
                "apply",
                [this](
                    int64_t task_id,
                    IDasReadOnlyString* p_body,
                    IDasReadOnlyString** pp_out)
                {
                    return scheduler_.ApplyTaskAuthoringChange(
                        task_id,
                        p_body,
                        pp_out);
                });
        }

        Beast::HttpResponse AuthoringCompile(const Beast::HttpRequest& request)
        {
            return DispatchAuthoring(
                request,
                "compile",
                [this](
                    int64_t task_id,
                    IDasReadOnlyString* p_body,
                    IDasReadOnlyString** pp_out)
                {
                    return scheduler_.CompileTaskAuthoring(
                        task_id,
                        p_body,
                        pp_out);
                });
        }

    private:
        template <typename Fn>
        Beast::HttpResponse DispatchAuthoring(
            const Beast::HttpRequest& request,
            const std::string&        operation,
            Fn&&                      fn)
        {
            auto profile = request.GetPathParameter("profile");
            if (profile != "0")
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Profile ID must be 0 in v1.2");
            }

            int64_t task_id = 0;
            try
            {
                task_id = std::stoll(request.GetPathParameter("taskId"));
            }
            catch (const std::exception&)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Invalid task ID format");
            }

            const auto& body = request.JsonBody();
            if (!body.is_object())
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Request body must be a JSON object");
            }

            auto body_json = Das::Utils::SerializeYyjsonValue(body);
            if (!body_json)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_JSON,
                    "Failed to serialize authoring request body");
            }

            DasPtr<IDasReadOnlyString> p_body;
            auto cr = CreateIDasReadOnlyStringFromUtf8(
                body_json->c_str(),
                p_body.Put());
            if (DAS::IsFailed(cr))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    cr,
                    "Failed to create authoring request string");
            }

            DasPtr<IDasReadOnlyString> p_result;
            auto result = fn(task_id, p_body.Get(), p_result.Put());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to dispatch task authoring " + operation);
            }

            const char* raw = nullptr;
            auto        get_result = p_result->GetUtf8(&raw);
            if (DAS::IsFailed(get_result) || raw == nullptr)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    get_result,
                    "Failed to read task authoring " + operation + " result");
            }

            auto parsed = Das::Utils::ParseYyjsonFromString(raw);
            if (!parsed)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_JSON,
                    "Failed to parse task authoring " + operation + " result");
            }

            auto obj = parsed->as_object();
            if (obj && obj->contains(std::string_view("errorKind")))
            {
                auto message = std::string{
                    (*obj)[std::string_view("message")]
                        .as_string()
                        .value_or("Task authoring request failed")};
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_FAIL,
                    message,
                    *parsed);
            }

            return Beast::HttpResponse::CreateSuccessResponse(*parsed);
        }

        template <typename Fn>
        Beast::HttpResponse DispatchRepositoryBody(
            const Beast::HttpRequest& request,
            const std::string&        operation,
            Fn&&                      fn)
        {
            auto profile = request.GetPathParameter("profile");
            if (profile != "0")
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Profile ID must be 0 in v1.2");
            }

            if (request.Body().empty())
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Request body must be a JSON object");
            }

            auto parsed_body = Das::Utils::ParseYyjsonFromString(
                request.Body());
            if (!parsed_body)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_JSON,
                    "Invalid repository request JSON");
            }

            if (!parsed_body->is_object())
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_ARGUMENT,
                    "Request body must be a JSON object");
            }

            auto body_json = Das::Utils::SerializeYyjsonValue(*parsed_body);
            if (!body_json)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_JSON,
                    "Failed to serialize repository request body");
            }

            DasPtr<IDasReadOnlyString> p_body;
            auto cr = CreateIDasReadOnlyStringFromUtf8(
                body_json->c_str(),
                p_body.Put());
            if (DAS::IsFailed(cr))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    cr,
                    "Failed to create repository request string");
            }

            DasPtr<IDasReadOnlyString> p_result;
            auto result = fn(p_body.Get(), p_result.Put());
            if (DAS::IsFailed(result))
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    result,
                    "Failed to dispatch repository " + operation);
            }

            const char* raw = nullptr;
            auto        get_result = p_result->GetUtf8(&raw);
            if (DAS::IsFailed(get_result) || raw == nullptr)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    get_result,
                    "Failed to read repository " + operation + " result");
            }

            auto parsed = Das::Utils::ParseYyjsonFromString(raw);
            if (!parsed)
            {
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_INVALID_JSON,
                    "Failed to parse repository " + operation + " result");
            }

            auto obj = parsed->as_object();
            if (obj && obj->contains(std::string_view("errorKind")))
            {
                auto message = std::string{
                    (*obj)[std::string_view("message")]
                        .as_string()
                        .value_or("Repository request failed")};
                return Beast::HttpResponse::CreateErrorResponse(
                    DAS_E_FAIL,
                    message,
                    *parsed);
            }

            return Beast::HttpResponse::CreateSuccessResponse(*parsed);
        }

        IDasSchedulerService& scheduler_;
        std::filesystem::path plugin_dir_;
    };

} // namespace Das::Http

#endif
