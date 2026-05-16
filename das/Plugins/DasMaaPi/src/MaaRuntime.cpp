#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Utils/DasJsonCore.h>

#include "MaaHandle.h"

#ifndef DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY
#include <MaaFramework/MaaAPI.h>
#endif

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
#ifndef DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY
        class ScopedMaaApiBoundary final : public IMaaApiBoundary
        {
        public:
            MaaResourceHandle CreateResource() override
            {
                return reinterpret_cast<MaaResourceHandle>(MaaResourceCreate());
            }

            void DestroyResource(MaaResourceHandle resource) noexcept override
            {
                MaaResourceDestroy(reinterpret_cast<MaaResource*>(resource));
            }

            MaaApiResult LoadResource(
                MaaResourceHandle resource,
                std::string_view  path) override
            {
                auto* maa_resource = reinterpret_cast<MaaResource*>(resource);
                auto id = MaaResourcePostBundle(
                    maa_resource,
                    std::string(path).c_str());
                if (id == MaaInvalidId)
                {
                    return MaaApiResult::Failure(
                        id,
                        "MaaResourcePostBundle failed");
                }
                const auto status = MaaResourceWait(maa_resource, id);
                if (status != MaaStatus_Succeeded)
                {
                    return MaaApiResult::Failure(
                        status,
                        "Maa resource load failed");
                }
                return MaaApiResult::Ok(id);
            }

            std::optional<std::string> GetResourceHash(
                MaaResourceHandle resource) override
            {
                std::unique_ptr<MaaStringBuffer, decltype(&MaaStringBufferDestroy)>
                    buffer(MaaStringBufferCreate(), MaaStringBufferDestroy);
                if (!buffer)
                {
                    return std::nullopt;
                }
                if (!MaaResourceGetHash(
                        reinterpret_cast<MaaResource*>(resource),
                        buffer.get()))
                {
                    return std::nullopt;
                }
                const char* value = MaaStringBufferGet(buffer.get());
                return value ? std::optional<std::string>(value) : std::nullopt;
            }

            MaaControllerHandle CreateController(
                const ControllerSpec& spec) override
            {
                auto type = Lower(spec.type);
                if (type == "dbg" || type == "debug")
                {
                    return reinterpret_cast<MaaControllerHandle>(
                        MaaDbgControllerCreate(spec.read_path.c_str()));
                }
                if (type == "adb")
                {
                    if (spec.address.empty())
                    {
                        return kInvalidMaaControllerHandle;
                    }
                    const auto adb_path =
                        spec.adb_path.empty() ? std::string("adb")
                                              : spec.adb_path;
                    const auto config =
                        spec.config_json.empty() ? std::string("{}")
                                                 : spec.config_json;
                    return reinterpret_cast<MaaControllerHandle>(
                        MaaAdbControllerCreate(
                            adb_path.c_str(),
                            spec.address.c_str(),
                            MaaAdbScreencapMethod_Default,
                            MaaAdbInputMethod_Default,
                            config.c_str(),
                            spec.agent_path.c_str()));
                }
                return kInvalidMaaControllerHandle;
            }

            void DestroyController(
                MaaControllerHandle controller) noexcept override
            {
                MaaControllerDestroy(
                    reinterpret_cast<MaaController*>(controller));
            }

            MaaTaskerHandle CreateTasker() override
            {
                return reinterpret_cast<MaaTaskerHandle>(MaaTaskerCreate());
            }

            void DestroyTasker(MaaTaskerHandle tasker) noexcept override
            {
                MaaTaskerDestroy(reinterpret_cast<MaaTasker*>(tasker));
            }

            MaaApiResult BindResource(
                MaaTaskerHandle   tasker,
                MaaResourceHandle resource) override
            {
                if (!MaaTaskerBindResource(
                        reinterpret_cast<MaaTasker*>(tasker),
                        reinterpret_cast<MaaResource*>(resource)))
                {
                    return MaaApiResult::Failure(0, "MaaTaskerBindResource failed");
                }
                return MaaApiResult::Ok();
            }

            MaaApiResult BindController(
                MaaTaskerHandle     tasker,
                MaaControllerHandle controller) override
            {
                if (!MaaTaskerBindController(
                        reinterpret_cast<MaaTasker*>(tasker),
                        reinterpret_cast<MaaController*>(controller)))
                {
                    return MaaApiResult::Failure(
                        0,
                        "MaaTaskerBindController failed");
                }
                return MaaApiResult::Ok();
            }

            MaaApiResult PostTask(
                MaaTaskerHandle tasker,
                std::string_view entry,
                std::string_view pipeline_override) override
            {
                const auto id = MaaTaskerPostTask(
                    reinterpret_cast<MaaTasker*>(tasker),
                    std::string(entry).c_str(),
                    std::string(pipeline_override).c_str());
                if (id == MaaInvalidId)
                {
                    return MaaApiResult::Failure(id, "MaaTaskerPostTask failed");
                }
                return MaaApiResult::Ok(id);
            }

            MaaTaskStatus WaitTask(
                MaaTaskerHandle tasker,
                MaaAsyncId      task_id) override
            {
                return static_cast<MaaTaskStatus>(MaaTaskerWait(
                    reinterpret_cast<MaaTasker*>(tasker),
                    static_cast<MaaTaskId>(task_id)));
            }

            MaaApiResult PostStop(MaaTaskerHandle tasker) override
            {
                const auto id = MaaTaskerPostStop(
                    reinterpret_cast<MaaTasker*>(tasker));
                if (id == MaaInvalidId)
                {
                    return MaaApiResult::Failure(id, "MaaTaskerPostStop failed");
                }
                return MaaApiResult::Ok(id);
            }

            MaaAgentClientHandle CreateAgentClientV2(
                std::optional<std::string_view>) override
            {
                return kInvalidMaaAgentClientHandle;
            }

            MaaAgentClientHandle CreateAgentClientTcp(
                std::uint16_t) override
            {
                return kInvalidMaaAgentClientHandle;
            }

            void DestroyAgentClient(
                MaaAgentClientHandle) noexcept override
            {
            }

            std::optional<std::string> GetAgentClientIdentifier(
                MaaAgentClientHandle) override
            {
                return std::nullopt;
            }

            MaaApiResult BindAgentClientResource(
                MaaAgentClientHandle,
                MaaResourceHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Maa AgentClient boundary is not implemented");
            }

            MaaApiResult RegisterAgentClientResourceSink(
                MaaAgentClientHandle,
                MaaResourceHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Maa AgentClient boundary is not implemented");
            }

            MaaApiResult RegisterAgentClientControllerSink(
                MaaAgentClientHandle,
                MaaControllerHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Maa AgentClient boundary is not implemented");
            }

            MaaApiResult RegisterAgentClientTaskerSink(
                MaaAgentClientHandle,
                MaaTaskerHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Maa AgentClient boundary is not implemented");
            }

            MaaApiResult SetAgentClientTimeout(
                MaaAgentClientHandle,
                std::int64_t) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Maa AgentClient boundary is not implemented");
            }

            MaaApiResult ConnectAgentClient(
                MaaAgentClientHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Maa AgentClient boundary is not implemented");
            }

            bool DisconnectAgentClient(
                MaaAgentClientHandle) noexcept override
            {
                return false;
            }

            bool IsAgentClientConnected(
                MaaAgentClientHandle) override
            {
                return false;
            }

            bool IsAgentClientAlive(
                MaaAgentClientHandle) override
            {
                return false;
            }

        private:
            static std::string Lower(std::string value)
            {
                std::transform(
                    value.begin(),
                    value.end(),
                    value.begin(),
                    [](unsigned char ch) {
                        return static_cast<char>(std::tolower(ch));
                    });
                return value;
            }

        };
#else
        class ScopedMaaApiBoundary final : public IMaaApiBoundary
        {
        public:
            MaaResourceHandle CreateResource() override
            {
                return kInvalidMaaResourceHandle;
            }

            void DestroyResource(MaaResourceHandle) noexcept override {}

            MaaApiResult LoadResource(
                MaaResourceHandle,
                std::string_view) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            std::optional<std::string> GetResourceHash(
                MaaResourceHandle) override
            {
                return std::nullopt;
            }

            MaaControllerHandle CreateController(
                const ControllerSpec&) override
            {
                return kInvalidMaaControllerHandle;
            }

            void DestroyController(MaaControllerHandle) noexcept override {}

            MaaTaskerHandle CreateTasker() override
            {
                return kInvalidMaaTaskerHandle;
            }

            void DestroyTasker(MaaTaskerHandle) noexcept override {}

            MaaApiResult BindResource(
                MaaTaskerHandle,
                MaaResourceHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaApiResult BindController(
                MaaTaskerHandle,
                MaaControllerHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaApiResult PostTask(
                MaaTaskerHandle,
                std::string_view,
                std::string_view) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaTaskStatus WaitTask(
                MaaTaskerHandle,
                MaaAsyncId) override
            {
                return MaaTaskStatus::Invalid;
            }

            MaaApiResult PostStop(MaaTaskerHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaAgentClientHandle CreateAgentClientV2(
                std::optional<std::string_view>) override
            {
                return kInvalidMaaAgentClientHandle;
            }

            MaaAgentClientHandle CreateAgentClientTcp(
                std::uint16_t) override
            {
                return kInvalidMaaAgentClientHandle;
            }

            void DestroyAgentClient(
                MaaAgentClientHandle) noexcept override
            {
            }

            std::optional<std::string> GetAgentClientIdentifier(
                MaaAgentClientHandle) override
            {
                return std::nullopt;
            }

            MaaApiResult BindAgentClientResource(
                MaaAgentClientHandle,
                MaaResourceHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaApiResult RegisterAgentClientResourceSink(
                MaaAgentClientHandle,
                MaaResourceHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaApiResult RegisterAgentClientControllerSink(
                MaaAgentClientHandle,
                MaaControllerHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaApiResult RegisterAgentClientTaskerSink(
                MaaAgentClientHandle,
                MaaTaskerHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaApiResult SetAgentClientTimeout(
                MaaAgentClientHandle,
                std::int64_t) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaApiResult ConnectAgentClient(
                MaaAgentClientHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            bool DisconnectAgentClient(
                MaaAgentClientHandle) noexcept override
            {
                return false;
            }

            bool IsAgentClientConnected(
                MaaAgentClientHandle) override
            {
                return false;
            }

            bool IsAgentClientAlive(
                MaaAgentClientHandle) override
            {
                return false;
            }
        };
#endif

        IMaaApiBoundary* g_boundary_for_test = nullptr;

        void AddDiagnostic(
            MaaRuntimeResult& result,
            std::string       code,
            std::string       message,
            std::optional<std::int64_t> provider_code = std::nullopt,
            std::string       severity = "error")
        {
            result.diagnostics.emplace_back(MaaRuntimeDiagnostic{
                .severity = std::move(severity),
                .code = std::move(code),
                .message = std::move(message),
                .provider_code = provider_code});
        }

        bool StopRequested(PluginInterface::IDasStopToken* stop_token)
        {
            bool requested = false;
            return stop_token
                   && DAS::IsOk(stop_token->StopRequested(&requested))
                   && requested;
        }

        std::string SerializeJson(const yyjson::value& value)
        {
            auto serialized = Das::Utils::SerializeYyjsonValue(value);
            return serialized.value_or("{}");
        }

        template <typename ObjectRef>
        std::optional<std::string> OptionalStringField(
            const ObjectRef& obj,
            std::string_view key)
        {
            if (!obj.contains(key) || !obj[key].is_string())
            {
                return std::nullopt;
            }
            return std::string(obj[key].as_string().value_or(""));
        }

        template <typename ObjectRef>
        std::optional<std::string> OptionalStringField(
            const ObjectRef& obj,
            std::initializer_list<std::string_view> keys)
        {
            for (const auto key : keys)
            {
                auto value = OptionalStringField(obj, key);
                if (value)
                {
                    return value;
                }
            }
            return std::nullopt;
        }

        template <typename ObjectRef>
        std::optional<std::string> RequiredStringField(
            const ObjectRef& obj,
            std::string_view key,
            ParsedExecutionEnvelope& parsed)
        {
            auto result = OptionalStringField(obj, key);
            if (!result)
            {
                parsed.result = DAS_E_INVALID_ARGUMENT;
                parsed.message = "Missing string field: " + std::string(key);
            }
            return result;
        }

        template <typename JsonValue>
        yyjson::value CopyJsonValue(const JsonValue& value)
        {
            return yyjson::value(value);
        }

        template <typename ObjectRef>
        std::vector<std::string> StringArrayField(
            const ObjectRef& obj,
            std::string_view key)
        {
            std::vector<std::string> result;
            if (!obj.contains(key))
            {
                return result;
            }
            auto array = obj[key].as_array();
            if (!array)
            {
                return result;
            }
            for (auto it = array->begin(); it != array->end(); ++it)
            {
                if (it->is_string())
                {
                    result.emplace_back(it->as_string().value_or(""));
                }
            }
            return result;
        }

        template <typename ObjectRef>
        bool BoolField(
            const ObjectRef& obj,
            std::string_view key,
            bool default_value)
        {
            return obj.contains(key) && obj[key].is_bool()
                       ? obj[key].as_bool().value_or(default_value)
                       : default_value;
        }

        template <typename ObjectRef>
        std::string ControllerConfigJson(const ObjectRef& obj)
        {
            if (auto config_json =
                    OptionalStringField(obj, {"configJson", "config_json"}))
            {
                return *config_json;
            }
            if (!obj.contains(std::string_view("config")))
            {
                return "{}";
            }
            const auto& config = obj[std::string_view("config")];
            if (config.is_string())
            {
                return std::string(config.as_string().value_or("{}"));
            }
            return Das::Utils::SerializeYyjsonValue(config).value_or("{}");
        }

        template <typename ObjectRef>
        ControllerSpec ControllerSpecFromObject(
            const ObjectRef&  obj,
            std::string       default_name,
            std::string       default_type)
        {
            ControllerSpec spec;
            spec.name =
                OptionalStringField(obj, "name").value_or(std::move(default_name));
            spec.type =
                OptionalStringField(obj, "type").value_or(std::move(default_type));
            spec.read_path =
                OptionalStringField(obj, {"readPath", "read_path"}).value_or("");
            spec.address = OptionalStringField(obj, "address").value_or("");
            spec.adb_path =
                OptionalStringField(obj, {"adbPath", "adb_path"}).value_or("adb");
            spec.config_json = ControllerConfigJson(obj);
            spec.agent_path =
                OptionalStringField(obj, {"agentPath", "agent_path"}).value_or("");
            return spec;
        }

        ControllerSpec ControllerSpecFromRawJson(
            std::string       default_name,
            std::string_view  controller_json)
        {
            if (controller_json.empty())
            {
                ControllerSpec spec;
                spec.name = std::move(default_name);
                return spec;
            }
            auto parsed = Das::Utils::ParseYyjsonFromString(controller_json);
            auto obj = parsed ? parsed->as_object() : std::nullopt;
            if (!obj)
            {
                ControllerSpec spec;
                spec.name = std::move(default_name);
                return spec;
            }
            return ControllerSpecFromObject(*obj, std::move(default_name), "");
        }
    } // namespace

    IMaaApiBoundary& DefaultMaaApiBoundary()
    {
        static ScopedMaaApiBoundary boundary;
        return boundary;
    }

    void SetMaaApiBoundaryForTest(IMaaApiBoundary* boundary)
    {
        g_boundary_for_test = boundary;
    }

    IMaaApiBoundary& MaaApiBoundaryForRuntime()
    {
        return g_boundary_for_test ? *g_boundary_for_test
                                   : DefaultMaaApiBoundary();
    }

    ParsedExecutionEnvelope ParseExecutionEnvelope(const yyjson::value& value)
    {
        ParsedExecutionEnvelope parsed;
        auto root = value.as_object();
        if (!root)
        {
            parsed.result = DAS_E_INVALID_JSON;
            parsed.message = "Execution envelope must be a JSON object";
            return parsed;
        }

        if (root->contains(std::string_view("version"))
            && (*root)[std::string_view("version")].is_sint())
        {
            parsed.envelope.version = static_cast<int32_t>(
                (*root)[std::string_view("version")].as_sint().value_or(1));
        }
        parsed.envelope.plugin_guid =
            RequiredStringField(*root, "pluginGuid", parsed).value_or("");
        if (DAS::IsFailed(parsed.result))
        {
            return parsed;
        }
        parsed.envelope.task_type_guid =
            RequiredStringField(*root, "taskTypeGuid", parsed).value_or("");
        if (DAS::IsFailed(parsed.result))
        {
            return parsed;
        }

        if (!root->contains(std::string_view("maapi")))
        {
            parsed.result = DAS_E_INVALID_ARGUMENT;
            parsed.message = "Missing maapi execution plan";
            return parsed;
        }
        auto maapi = (*root)[std::string_view("maapi")].as_object();
        if (!maapi)
        {
            parsed.result = DAS_E_INVALID_ARGUMENT;
            parsed.message = "maapi must be an object";
            return parsed;
        }

        auto& plan = parsed.envelope.maapi;
        plan.interface_directory =
            RequiredStringField(*maapi, "interfaceDirectory", parsed)
                .value_or("");
        if (DAS::IsFailed(parsed.result))
        {
            return parsed;
        }
        plan.controller_name =
            RequiredStringField(*maapi, "controllerName", parsed).value_or("");
        if (DAS::IsFailed(parsed.result))
        {
            return parsed;
        }
        plan.resource_name =
            RequiredStringField(*maapi, "resourceName", parsed).value_or("");
        if (DAS::IsFailed(parsed.result))
        {
            return parsed;
        }
        plan.resource_paths = StringArrayField(*maapi, "resourcePaths");
        plan.resource_hash = OptionalStringField(*maapi, "resourceHash");
        plan.fail_fast = BoolField(*maapi, "failFast", true);
        plan.requires_agent_runtime =
            BoolField(*maapi, "requiresAgentRuntime", false);

        if (maapi->contains(std::string_view("piEnv")))
        {
            if (auto env = (*maapi)[std::string_view("piEnv")].as_object())
            {
                plan.pi_env.interface_version =
                    OptionalStringField(*env, "interfaceVersion").value_or("2");
                plan.pi_env.client_name =
                    OptionalStringField(*env, "clientName").value_or("DAS");
                plan.pi_env.client_language =
                    OptionalStringField(*env, "clientLanguage").value_or("cpp");
                plan.pi_env.project_version =
                    OptionalStringField(*env, "projectVersion").value_or("");
                plan.pi_env.controller_json =
                    OptionalStringField(*env, "controllerJson").value_or("");
                plan.pi_env.resource_json =
                    OptionalStringField(*env, "resourceJson").value_or("");
            }
        }

        if (maapi->contains(std::string_view("controller")))
        {
            auto controller =
                (*maapi)[std::string_view("controller")].as_object();
            if (!controller)
            {
                parsed.result = DAS_E_INVALID_ARGUMENT;
                parsed.message = "controller must be an object";
                return parsed;
            }
            plan.controller = ControllerSpecFromObject(
                *controller,
                plan.controller_name,
                "");
        }
        else
        {
            plan.controller = ControllerSpecFromRawJson(
                plan.controller_name,
                plan.pi_env.controller_json);
        }

        if (!maapi->contains(std::string_view("tasks")))
        {
            parsed.result = DAS_E_INVALID_ARGUMENT;
            parsed.message = "Missing task list";
            return parsed;
        }
        auto tasks = (*maapi)[std::string_view("tasks")].as_array();
        if (!tasks)
        {
            parsed.result = DAS_E_INVALID_ARGUMENT;
            parsed.message = "tasks must be an array";
            return parsed;
        }
        for (auto it = tasks->begin(); it != tasks->end(); ++it)
        {
            auto task_obj = it->as_object();
            if (!task_obj)
            {
                parsed.result = DAS_E_INVALID_ARGUMENT;
                parsed.message = "task item must be an object";
                return parsed;
            }

            MaaTaskExecutionDto task;
            task.task_name =
                RequiredStringField(*task_obj, "taskName", parsed).value_or("");
            if (DAS::IsFailed(parsed.result))
            {
                return parsed;
            }
            task.entry =
                RequiredStringField(*task_obj, "entry", parsed).value_or("");
            if (DAS::IsFailed(parsed.result))
            {
                return parsed;
            }
            if (task_obj->contains(std::string_view("pipelineOverride")))
            {
                task.pipeline_override =
                    CopyJsonValue(
                        (*task_obj)[std::string_view("pipelineOverride")]);
            }
            else
            {
                task.pipeline_override = Das::Utils::MakeYyjsonObject();
            }
            plan.tasks.emplace_back(std::move(task));
        }

        parsed.result = ValidateExecutionEnvelope(parsed.envelope);
        return parsed;
    }

    DasResult ValidateExecutionEnvelope(const ExecutionEnvelopeDto& envelope)
    {
        if (envelope.plugin_guid != kPluginGuidText
            || envelope.task_type_guid != kTaskGuidText)
        {
            return DAS_E_INVALID_ARGUMENT;
        }
        if (envelope.version != 1)
        {
            return DAS_E_INVALID_ARGUMENT;
        }
        if (envelope.maapi.requires_agent_runtime)
        {
            return DAS_E_NO_IMPLEMENTATION;
        }
        if (envelope.maapi.tasks.empty())
        {
            return DAS_E_INVALID_ARGUMENT;
        }
        return DAS_S_OK;
    }

    MaaRuntimeResult MaaRuntime::Run(
        const ExecutionEnvelopeDto&     envelope,
        IMaaApiBoundary&                boundary,
        PluginInterface::IDasStopToken* stop_token)
    {
        MaaRuntimeResult result;
        const auto validation = ValidateExecutionEnvelope(envelope);
        if (DAS::IsFailed(validation))
        {
            result.das_result = validation;
            AddDiagnostic(
                result,
                "invalid-envelope",
                "Execution envelope is not supported by the MAAPI runtime");
            return result;
        }

        ScopedResource resource(boundary, boundary.CreateResource());
        if (resource.get() == kInvalidMaaResourceHandle)
        {
            result.das_result = DAS_E_FAIL;
            AddDiagnostic(result, "create-resource-failed", "Maa resource creation failed");
            return result;
        }

        for (const auto& path : envelope.maapi.resource_paths)
        {
            auto load = boundary.LoadResource(resource.get(), path);
            if (!load.ok)
            {
                result.das_result = DAS_E_FAIL;
                AddDiagnostic(
                    result,
                    "load-resource-failed",
                    load.message,
                    load.provider_code);
                return result;
            }
        }

        if (envelope.maapi.resource_hash)
        {
            auto actual_hash = boundary.GetResourceHash(resource.get());
            if (actual_hash && *actual_hash != *envelope.maapi.resource_hash)
            {
                AddDiagnostic(
                    result,
                    "resource-hash-mismatch",
                    "Maa resource hash does not match the PI envelope",
                    std::nullopt,
                    "warning");
            }
        }

        ControllerSpec controller_spec = envelope.maapi.controller;
        if (controller_spec.name.empty())
        {
            controller_spec.name = envelope.maapi.controller_name;
        }
        ScopedController controller(
            boundary,
            boundary.CreateController(controller_spec));
        if (controller.get() == kInvalidMaaControllerHandle)
        {
            result.das_result = DAS_E_FAIL;
            AddDiagnostic(
                result,
                "create-controller-failed",
                "Maa controller creation failed");
            return result;
        }

        ScopedTasker tasker(boundary, boundary.CreateTasker());
        if (tasker.get() == kInvalidMaaTaskerHandle)
        {
            result.das_result = DAS_E_FAIL;
            AddDiagnostic(result, "create-tasker-failed", "Maa tasker creation failed");
            return result;
        }

        auto bind_resource = boundary.BindResource(tasker.get(), resource.get());
        if (!bind_resource.ok)
        {
            result.das_result = DAS_E_FAIL;
            AddDiagnostic(
                result,
                "bind-resource-failed",
                bind_resource.message,
                bind_resource.provider_code);
            return result;
        }

        auto bind_controller =
            boundary.BindController(tasker.get(), controller.get());
        if (!bind_controller.ok)
        {
            result.das_result = DAS_E_FAIL;
            AddDiagnostic(
                result,
                "bind-controller-failed",
                bind_controller.message,
                bind_controller.provider_code);
            return result;
        }

        for (const auto& task : envelope.maapi.tasks)
        {
            if (StopRequested(stop_token))
            {
                boundary.PostStop(tasker.get());
                result.stopped = true;
                result.das_result = DAS_E_FAIL;
                AddDiagnostic(
                    result,
                    "stop-requested",
                    "DAS stop token requested Maa tasker stop");
                return result;
            }

            auto post = boundary.PostTask(
                tasker.get(),
                task.entry,
                SerializeJson(task.pipeline_override));
            if (!post.ok)
            {
                result.das_result = DAS_E_FAIL;
                AddDiagnostic(
                    result,
                    "post-task-failed",
                    post.message,
                    post.provider_code);
                return result;
            }

            const auto status = boundary.WaitTask(tasker.get(), post.id);
            if (status == MaaTaskStatus::Succeeded)
            {
                result.completed_tasks.emplace_back(task.task_name);
                continue;
            }

            result.das_result = DAS_E_FAIL;
            AddDiagnostic(
                result,
                "task-failed",
                "Maa task failed: " + task.task_name,
                static_cast<std::int64_t>(status));
            if (envelope.maapi.fail_fast)
            {
                return result;
            }
        }

        return result;
    }
} // namespace Das::Plugins::DasMaaPi
