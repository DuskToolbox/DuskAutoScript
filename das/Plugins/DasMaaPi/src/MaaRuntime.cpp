#include <das/Plugins/DasMaaPi/ExecutionEnvelope.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Utils/DasJsonCore.h>

#include "AgentProcessRunner.h"
#include "AgentRuntimeService.h"
#include "MaaHandle.h"

#ifndef DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY
#include <MaaAgentClient/MaaAgentClientAPI.h>
#include <MaaFramework/MaaAPI.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// PI V2 协议 `agent: object | object[]` 的多态反序列化：单对象包成 1 元素
// vector，数组直接成 vector。替换原手写 ReadAgentSpecs 的三形态识别；每个
// 元素的字段解析委托 default_caster<AgentSpecDto>（field_name_rule =
// snake_to_camel，读 camelCase），envelope 路径的 agent 始终由 PiCompiler
// 产出为 camelCase。
template <>
struct yyjson::caster<
    std::vector<Das::Plugins::DasMaaPi::AgentRuntime::AgentSpecDto>>
{
    using AgentSpecDto = Das::Plugins::DasMaaPi::AgentRuntime::AgentSpecDto;

    template <yyjson::json_value Json>
    static std::vector<AgentSpecDto> from_json(const Json& json)
    {
        // 元素字段解析委托 default_caster<AgentSpecDto>（field_name_rule =
        // snake_to_camel）。B′ 之后 envelope 由 PiCompiler 用 default_caster
        // to_json 产出，optional identifier 以 null 存在，聚合反射 is_null
        // 跳过即可处理；caster 只保留 object|array 多态分流。
        std::vector<AgentSpecDto> result;
        if (auto single = json.as_object())
        {
            result.emplace_back(yyjson::cast<AgentSpecDto>(json));
            return result;
        }
        if (auto array = json.as_array())
        {
            for (auto it = array->begin(); it != array->end(); ++it)
            {
                result.emplace_back(yyjson::cast<AgentSpecDto>(*it));
            }
            return result;
        }
        throw yyjson::bad_cast("agent must be an object or array");
    }
};

// MaaExecutionPlanDto：整体委托 default_caster 聚合反射（field_name_rule =
// snake_to_camel）。controller 作为普通非 optional 字段直接读——PiCompiler 是
// envelope 唯一生产者且总产出 controller 对象（PiCompiler.cpp:520），运行时无
// 「无 controller → piEnv 兜底」路径，故移除两源 fallback（原 fallback 是死代码，
// 仅合成测试覆盖）。缺 key 抛 std::out_of_range、类型错误抛 yyjson::bad_cast，
// 由 ParseExecutionEnvelope 统一映射为 DAS_E_INVALID_ARGUMENT。
template <>
struct yyjson::caster<Das::Plugins::DasMaaPi::MaaExecutionPlanDto>
{
    template <yyjson::json_value Json>
    static Das::Plugins::DasMaaPi::MaaExecutionPlanDto from_json(const Json& json)
    {
        auto obj = json.as_object();
        if (!obj)
        {
            throw yyjson::bad_cast("maapi must be an object");
        }
        return yyjson::detail::default_caster<
            Das::Plugins::DasMaaPi::MaaExecutionPlanDto>::from_json(*obj);
    }
};

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
                auto  id = MaaResourcePostBundle(
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
                std::unique_ptr<
                    MaaStringBuffer,
                    decltype(&MaaStringBufferDestroy)>
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
                    const auto adb_path = spec.adb_path.empty()
                                              ? std::string("adb")
                                              : spec.adb_path;
                    const auto config = spec.config_json.empty()
                                            ? std::string("{}")
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
                    return MaaApiResult::Failure(
                        0,
                        "MaaTaskerBindResource failed");
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
                MaaTaskerHandle  tasker,
                std::string_view entry,
                std::string_view pipeline_override) override
            {
                const auto id = MaaTaskerPostTask(
                    reinterpret_cast<MaaTasker*>(tasker),
                    std::string(entry).c_str(),
                    std::string(pipeline_override).c_str());
                if (id == MaaInvalidId)
                {
                    return MaaApiResult::Failure(
                        id,
                        "MaaTaskerPostTask failed");
                }
                return MaaApiResult::Ok(id);
            }

            MaaTaskStatus WaitTask(MaaTaskerHandle tasker, MaaAsyncId task_id)
                override
            {
                return static_cast<MaaTaskStatus>(MaaTaskerWait(
                    reinterpret_cast<MaaTasker*>(tasker),
                    static_cast<MaaTaskId>(task_id)));
            }

            MaaApiResult PostStop(MaaTaskerHandle tasker) override
            {
                const auto id =
                    MaaTaskerPostStop(reinterpret_cast<MaaTasker*>(tasker));
                if (id == MaaInvalidId)
                {
                    return MaaApiResult::Failure(
                        id,
                        "MaaTaskerPostStop failed");
                }
                return MaaApiResult::Ok(id);
            }

            MaaAgentClientHandle CreateAgentClientV2(
                std::optional<std::string_view> identifier) override
            {
                std::unique_ptr<
                    MaaStringBuffer,
                    decltype(&MaaStringBufferDestroy)>
                                       buffer(nullptr, MaaStringBufferDestroy);
                const MaaStringBuffer* id_buffer = nullptr;
                if (identifier)
                {
                    buffer.reset(MaaStringBufferCreate());
                    if (!buffer
                        || !MaaStringBufferSetEx(
                            buffer.get(),
                            identifier->data(),
                            identifier->size()))
                    {
                        return kInvalidMaaAgentClientHandle;
                    }
                    id_buffer = buffer.get();
                }
                return reinterpret_cast<MaaAgentClientHandle>(
                    MaaAgentClientCreateV2(id_buffer));
            }

            MaaAgentClientHandle CreateAgentClientTcp(
                std::uint16_t port) override
            {
                return reinterpret_cast<MaaAgentClientHandle>(
                    MaaAgentClientCreateTcp(port));
            }

            void DestroyAgentClient(
                MaaAgentClientHandle client) noexcept override
            {
                MaaAgentClientDestroy(
                    reinterpret_cast<MaaAgentClient*>(client));
            }

            std::optional<std::string> GetAgentClientIdentifier(
                MaaAgentClientHandle client) override
            {
                std::unique_ptr<
                    MaaStringBuffer,
                    decltype(&MaaStringBufferDestroy)>
                    buffer(MaaStringBufferCreate(), MaaStringBufferDestroy);
                if (!buffer)
                {
                    return std::nullopt;
                }
                if (!MaaAgentClientIdentifier(
                        reinterpret_cast<MaaAgentClient*>(client),
                        buffer.get()))
                {
                    return std::nullopt;
                }
                const char* value = MaaStringBufferGet(buffer.get());
                return value ? std::optional<std::string>(value) : std::nullopt;
            }

            MaaApiResult BindAgentClientResource(
                MaaAgentClientHandle client,
                MaaResourceHandle    resource) override
            {
                if (!MaaAgentClientBindResource(
                        reinterpret_cast<MaaAgentClient*>(client),
                        reinterpret_cast<MaaResource*>(resource)))
                {
                    return MaaApiResult::Failure(
                        0,
                        "MaaAgentClientBindResource failed");
                }
                return MaaApiResult::Ok();
            }

            MaaApiResult RegisterAgentClientResourceSink(
                MaaAgentClientHandle client,
                MaaResourceHandle    resource) override
            {
                if (!MaaAgentClientRegisterResourceSink(
                        reinterpret_cast<MaaAgentClient*>(client),
                        reinterpret_cast<MaaResource*>(resource)))
                {
                    return MaaApiResult::Failure(
                        0,
                        "MaaAgentClientRegisterResourceSink failed");
                }
                return MaaApiResult::Ok();
            }

            MaaApiResult RegisterAgentClientControllerSink(
                MaaAgentClientHandle client,
                MaaControllerHandle  controller) override
            {
                if (!MaaAgentClientRegisterControllerSink(
                        reinterpret_cast<MaaAgentClient*>(client),
                        reinterpret_cast<MaaController*>(controller)))
                {
                    return MaaApiResult::Failure(
                        0,
                        "MaaAgentClientRegisterControllerSink failed");
                }
                return MaaApiResult::Ok();
            }

            MaaApiResult RegisterAgentClientTaskerSink(
                MaaAgentClientHandle client,
                MaaTaskerHandle      tasker) override
            {
                if (!MaaAgentClientRegisterTaskerSink(
                        reinterpret_cast<MaaAgentClient*>(client),
                        reinterpret_cast<MaaTasker*>(tasker)))
                {
                    return MaaApiResult::Failure(
                        0,
                        "MaaAgentClientRegisterTaskerSink failed");
                }
                return MaaApiResult::Ok();
            }

            MaaApiResult SetAgentClientTimeout(
                MaaAgentClientHandle client,
                std::int64_t         milliseconds) override
            {
                if (!MaaAgentClientSetTimeout(
                        reinterpret_cast<MaaAgentClient*>(client),
                        milliseconds))
                {
                    return MaaApiResult::Failure(
                        0,
                        "MaaAgentClientSetTimeout failed");
                }
                return MaaApiResult::Ok();
            }

            MaaApiResult ConnectAgentClient(
                MaaAgentClientHandle client) override
            {
                if (!MaaAgentClientConnect(
                        reinterpret_cast<MaaAgentClient*>(client)))
                {
                    return MaaApiResult::Failure(
                        0,
                        "MaaAgentClientConnect failed");
                }
                return MaaApiResult::Ok();
            }

            bool DisconnectAgentClient(
                MaaAgentClientHandle client) noexcept override
            {
                return MaaAgentClientDisconnect(
                    reinterpret_cast<MaaAgentClient*>(client));
            }

            bool IsAgentClientConnected(MaaAgentClientHandle client) override
            {
                return MaaAgentClientConnected(
                    reinterpret_cast<MaaAgentClient*>(client));
            }

            bool IsAgentClientAlive(MaaAgentClientHandle client) override
            {
                return MaaAgentClientAlive(
                    reinterpret_cast<MaaAgentClient*>(client));
            }

        private:
            static std::string Lower(std::string value)
            {
                std::transform(
                    value.begin(),
                    value.end(),
                    value.begin(),
                    [](unsigned char ch)
                    { return static_cast<char>(std::tolower(ch)); });
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

            MaaApiResult LoadResource(MaaResourceHandle, std::string_view)
                override
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

            MaaControllerHandle CreateController(const ControllerSpec&) override
            {
                return kInvalidMaaControllerHandle;
            }

            void DestroyController(MaaControllerHandle) noexcept override {}

            MaaTaskerHandle CreateTasker() override
            {
                return kInvalidMaaTaskerHandle;
            }

            void DestroyTasker(MaaTaskerHandle) noexcept override {}

            MaaApiResult BindResource(MaaTaskerHandle, MaaResourceHandle)
                override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            MaaApiResult BindController(MaaTaskerHandle, MaaControllerHandle)
                override
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

            MaaTaskStatus WaitTask(MaaTaskerHandle, MaaAsyncId) override
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

            MaaAgentClientHandle CreateAgentClientTcp(std::uint16_t) override
            {
                return kInvalidMaaAgentClientHandle;
            }

            void DestroyAgentClient(MaaAgentClientHandle) noexcept override {}

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

            MaaApiResult ConnectAgentClient(MaaAgentClientHandle) override
            {
                return MaaApiResult::Failure(
                    0,
                    "Real Maa boundary disabled in tests");
            }

            bool DisconnectAgentClient(MaaAgentClientHandle) noexcept override
            {
                return false;
            }

            bool IsAgentClientConnected(MaaAgentClientHandle) override
            {
                return false;
            }

            bool IsAgentClientAlive(MaaAgentClientHandle) override
            {
                return false;
            }
        };
#endif

        IMaaApiBoundary*                   g_boundary_for_test = nullptr;
        AgentRuntime::IAgentProcessRunner* g_agent_runner_for_test = nullptr;

        void AddDiagnostic(
            MaaRuntimeResult&           result,
            std::string                 code,
            std::string                 message,
            std::optional<std::int64_t> provider_code = std::nullopt,
            std::string                 severity = "error")
        {
            result.diagnostics.emplace_back(
                MaaRuntimeDiagnostic{
                    .severity = std::move(severity),
                    .code = std::move(code),
                    .message = std::move(message),
                    .provider_code = provider_code});
        }

        void AddAgentDiagnostics(
            MaaRuntimeResult&                          result,
            const AgentRuntime::AgentRuntimeResultDto& agent_result)
        {
            for (const auto& diagnostic : agent_result.diagnostics)
            {
                AddDiagnostic(
                    result,
                    diagnostic.code,
                    diagnostic.message,
                    std::nullopt,
                    diagnostic.severity);
            }
        }

        AgentRuntime::PiEnvDto ToAgentPiEnv(const PiEnvSnapshotDto& env)
        {
            AgentRuntime::PiEnvDto result;
            result.interface_version = env.interface_version;
            result.client_name = env.client_name;
            result.client_language = env.client_language;
            result.project_version = env.project_version;
            result.controller_json = env.controller_json;
            result.resource_json = env.resource_json;
            return result;
        }

        AgentRuntime::AgentRuntimeRequestDto BuildAgentStartRequest(
            const ExecutionEnvelopeDto& envelope)
        {
            AgentRuntime::AgentRuntimeRequestDto request;
            request.operation = "start";
            request.interface_directory = envelope.maapi.interface_directory;
            request.agent = envelope.maapi.agent;
            request.pi_env = ToAgentPiEnv(envelope.maapi.pi_env);
            return request;
        }

        class ScopedAgentRuntimeSession final
        {
        public:
            void Arm(
                AgentRuntime::AgentRuntimeService&   service,
                std::string                          session_id,
                AgentRuntime::AgentRuntimeOptionsDto options)
            {
                service_ = &service;
                session_id_ = std::move(session_id);
                options_ = options;
            }

            AgentRuntime::AgentRuntimeResultDto StopNow()
            {
                if (!service_ || !session_id_)
                {
                    return {};
                }

                auto result = service_->Stop(*session_id_, options_);
                service_ = nullptr;
                session_id_.reset();
                return result;
            }

            ~ScopedAgentRuntimeSession() { (void)StopNow(); }

        private:
            AgentRuntime::AgentRuntimeService*   service_ = nullptr;
            std::optional<std::string>           session_id_;
            AgentRuntime::AgentRuntimeOptionsDto options_;
        };

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

        // 手写 Field/FromObject helper（OptionalStringField / RequiredStringField /
        // StringArrayField / BoolField / OptionalInt32Field / CopyJsonValue）已由
        // default_caster + caster 特化全面取代并移除；SerializeJson 保留
        // （MaaFW 边界 opaque JSON 透传，MaaRuntime::Run 在用）。
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

    void SetAgentProcessRunnerForTest(AgentRuntime::IAgentProcessRunner* runner)
    {
        g_agent_runner_for_test = runner;
    }

    AgentRuntime::IAgentProcessRunner* AgentProcessRunnerForRuntime()
    {
        return g_agent_runner_for_test;
    }

    IMaaApiBoundary& MaaApiBoundaryForRuntime()
    {
        return g_boundary_for_test ? *g_boundary_for_test
                                   : DefaultMaaApiBoundary();
    }

    ParsedExecutionEnvelope ParseExecutionEnvelope(const yyjson::value& value)
    {
        ParsedExecutionEnvelope parsed;
        try
        {
            // 整体委托 default_caster<ExecutionEnvelopeDto>：外层 version/
            // pluginGuid/taskTypeGuid 自动反射，maapi 走 caster<
            // MaaExecutionPlanDto>（controller 两源 fallback）。default_caster
            // 聚合反射缺 key 抛 std::out_of_range，类型/结构错误抛
            // yyjson::bad_cast（含 maapi caster 抛出的），统一映射为参数错误。
            parsed.envelope = yyjson::cast<ExecutionEnvelopeDto>(value);
        }
        catch (const std::exception& e)
        {
            parsed.result = DAS_E_INVALID_ARGUMENT;
            parsed.message = e.what();
            return parsed;
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
        if (envelope.maapi.requires_agent_runtime
            && envelope.maapi.agent.empty())
        {
            return DAS_E_INVALID_ARGUMENT;
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
        const auto       validation = ValidateExecutionEnvelope(envelope);
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
            AddDiagnostic(
                result,
                "create-resource-failed",
                "Maa resource creation failed");
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
            AddDiagnostic(
                result,
                "create-tasker-failed",
                "Maa tasker creation failed");
            return result;
        }

        auto bind_resource =
            boundary.BindResource(tasker.get(), resource.get());
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

        AgentRuntime::BoostAgentProcessRunner default_agent_runner;
        auto&                                 agent_runner =
            g_agent_runner_for_test
                ? *g_agent_runner_for_test
                : static_cast<AgentRuntime::IAgentProcessRunner&>(
                      default_agent_runner);
        AgentRuntime::AgentRuntimeService agent_service(boundary, agent_runner);
        ScopedAgentRuntimeSession         agent_session;
        if (envelope.maapi.requires_agent_runtime)
        {
            auto agent_request = BuildAgentStartRequest(envelope);
            auto agent_start = agent_service.Start(
                agent_request,
                AgentRuntime::AgentRuntimeMaaContext{
                    .resource = resource.get(),
                    .controller = controller.get(),
                    .tasker = tasker.get()});
            if (agent_start.status != "succeeded" || !agent_start.session_id)
            {
                result.das_result = DAS_E_FAIL;
                AddAgentDiagnostics(result, agent_start);
                if (result.diagnostics.empty())
                {
                    AddDiagnostic(
                        result,
                        "agent-runtime-start-failed",
                        "PI agent runtime startup failed");
                }
                return result;
            }
            agent_session.Arm(
                agent_service,
                *agent_start.session_id,
                agent_request.options);
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

        if (envelope.maapi.requires_agent_runtime)
        {
            auto agent_stop = agent_session.StopNow();
            if (agent_stop.status == "failed")
            {
                result.das_result = DAS_E_FAIL;
                AddAgentDiagnostics(result, agent_stop);
                if (result.diagnostics.empty())
                {
                    AddDiagnostic(
                        result,
                        "agent-runtime-stop-failed",
                        "PI agent runtime cleanup failed");
                }
            }
            else if (agent_stop.signals.timed_out)
            {
                AddDiagnostic(
                    result,
                    "agent-runtime-stop-timeout",
                    "PI agent runtime cleanup timed out",
                    std::nullopt,
                    "warning");
            }
        }

        return result;
    }
} // namespace Das::Plugins::DasMaaPi
