#pragma once

#include <das/Plugins/DasMaaPi/MaaApiBoundary.h>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Das::Plugins::DasMaaPi::Test
{
    class FakeMaaApiBoundary final : public IMaaApiBoundary
    {
    public:
        std::vector<std::string>             calls;
        std::string                          resource_hash = "hash-expected";
        std::map<std::string, MaaTaskStatus> wait_status_by_entry;
        std::map<std::string, MaaApiResult>  post_result_by_entry;
        std::optional<ControllerSpec>        last_controller_spec;
        std::optional<ControllerInitParams>  last_controller_init_params;
        // Configurable result for the typed-init overload. Default constructs
        // a fresh handle so MaaRuntime::Run proceeds through the happy path;
        // tests exercising Tier2 stubs or HWND-resolution failure set this to
        // carry an error_code and INVALID_MAA_CONTROLLER_HANDLE.
        CreateControllerResult       create_from_params_result;
        std::optional<std::string>   last_agent_client_identifier;
        std::optional<std::uint16_t> last_agent_client_tcp_port;
        std::vector<std::string> agent_client_identifiers{"agent-client-id"};
        MaaApiResult bind_agent_client_resource_result = MaaApiResult::Ok();
        MaaApiResult register_agent_client_resource_sink_result =
            MaaApiResult::Ok();
        MaaApiResult register_agent_client_controller_sink_result =
            MaaApiResult::Ok();
        MaaApiResult register_agent_client_tasker_sink_result =
            MaaApiResult::Ok();
        MaaApiResult set_agent_client_timeout_result = MaaApiResult::Ok();
        MaaApiResult connect_agent_client_result = MaaApiResult::Ok();
        std::optional<MaaResourceHandle> last_bound_agent_resource;
        std::optional<MaaResourceHandle> last_registered_agent_resource_sink;
        std::optional<MaaControllerHandle>
                                       last_registered_agent_controller_sink;
        std::optional<MaaTaskerHandle> last_registered_agent_tasker_sink;

        MaaResourceHandle CreateResource() override
        {
            calls.emplace_back("CreateResource");
            return NextHandle();
        }

        void DestroyResource(MaaResourceHandle resource) noexcept override
        {
            calls.emplace_back("DestroyResource:" + std::to_string(resource));
        }

        MaaApiResult LoadResource(MaaResourceHandle, std::string_view path)
            override
        {
            calls.emplace_back("LoadResource:" + std::string(path));
            return MaaApiResult::Ok(NextId());
        }

        std::optional<std::string> GetResourceHash(MaaResourceHandle) override
        {
            calls.emplace_back("GetResourceHash");
            return resource_hash;
        }

        CreateControllerResult CreateControllerFromParams(
            const ControllerSpec&       spec,
            const ControllerInitParams& params) override
        {
            last_controller_spec = spec;
            last_controller_init_params = params;
            calls.emplace_back(
                "CreateControllerFromParams:" + spec.name + ":"
                + params.controller_type);
            CreateControllerResult result = create_from_params_result;
            // Default-construct a fresh handle when the test hasn't configured
            // a failure so the happy-path dispatch can proceed past
            // CreateController without each caller wiring up a result.
            if (result.handle == INVALID_MAA_CONTROLLER_HANDLE
                && result.error_code.empty())
            {
                result.handle = NextHandle();
            }
            return result;
        }

        void DestroyController(MaaControllerHandle controller) noexcept override
        {
            calls.emplace_back(
                "DestroyController:" + std::to_string(controller));
        }

        MaaTaskerHandle CreateTasker() override
        {
            calls.emplace_back("CreateTasker");
            return NextHandle();
        }

        void DestroyTasker(MaaTaskerHandle tasker) noexcept override
        {
            calls.emplace_back("DestroyTasker:" + std::to_string(tasker));
        }

        MaaApiResult BindResource(MaaTaskerHandle, MaaResourceHandle) override
        {
            calls.emplace_back("BindResource");
            return MaaApiResult::Ok();
        }

        MaaApiResult BindController(MaaTaskerHandle, MaaControllerHandle)
            override
        {
            calls.emplace_back("BindController");
            return MaaApiResult::Ok();
        }

        // Configurable connection results. Defaults synthesise the happy path
        // (PostConnection ok + fresh id, WaitConnection Succeeded, connected
        // true) so existing Run tests keep working without per-test wiring;
        // tests exercising the short-circuit paths override the relevant
        // field. post_connection_result defaults to Ok() with id=0 — the
        // override generates a fresh id on the happy path, mirroring PostTask.
        MaaApiResult  post_connection_result = MaaApiResult::Ok();
        MaaTaskStatus wait_connection_status = MaaTaskStatus::Succeeded;
        bool          is_connected_result = true;

        MaaApiResult PostConnection(MaaControllerHandle) override
        {
            calls.emplace_back("PostConnection");
            if (post_connection_result.ok)
            {
                return MaaApiResult::Ok(NextId());
            }
            return post_connection_result;
        }

        MaaTaskStatus WaitConnection(
            MaaControllerHandle,
            MaaAsyncId connection_id) override
        {
            calls.emplace_back(
                "WaitConnection:" + std::to_string(connection_id));
            return wait_connection_status;
        }

        bool IsConnected(MaaControllerHandle) override
        {
            calls.emplace_back("IsConnected");
            return is_connected_result;
        }

        MaaApiResult PostTask(
            MaaTaskerHandle,
            std::string_view entry,
            std::string_view pipeline_override) override
        {
            last_entry_ = std::string(entry);
            calls.emplace_back(
                "PostTask:" + std::string(entry) + ":"
                + std::string(pipeline_override));
            if (auto it = post_result_by_entry.find(last_entry_);
                it != post_result_by_entry.end())
            {
                return it->second;
            }
            return MaaApiResult::Ok(NextId());
        }

        MaaTaskStatus WaitTask(MaaTaskerHandle, MaaAsyncId task_id) override
        {
            calls.emplace_back("WaitTask:" + std::to_string(task_id));
            if (auto it = wait_status_by_entry.find(last_entry_);
                it != wait_status_by_entry.end())
            {
                return it->second;
            }
            return MaaTaskStatus::Succeeded;
        }

        MaaApiResult PostStop(MaaTaskerHandle) override
        {
            calls.emplace_back("PostStop");
            return MaaApiResult::Ok(NextId());
        }

        MaaApiResult SetCallback(
            MaaTaskerHandle,
            MaaMsgCallbackFn,
            void* trans_arg) override
        {
            calls.emplace_back("SetCallback");
            last_set_callback_trans_arg = trans_arg;
            return set_callback_result;
        }

        // Configurable sink-registration results. Defaults mirror the tasker
        // sink happy path so existing Run tests proceed without per-test
        // wiring; tests exercising the warning-degradation path override
        // the relevant field. Last-trans-arg capture lets a test assert all
        // three sinks share the same logger context pointer.
        MaaApiResult set_callback_result = MaaApiResult::Ok();
        MaaApiResult register_resource_sink_result = MaaApiResult::Ok();
        MaaApiResult register_controller_sink_result = MaaApiResult::Ok();
        void*        last_set_callback_trans_arg = nullptr;
        void*        last_registered_resource_sink_trans_arg = nullptr;
        void*        last_registered_controller_sink_trans_arg = nullptr;

        MaaApiResult RegisterResourceSink(
            MaaResourceHandle,
            MaaMsgCallbackFn,
            void* trans_arg) override
        {
            calls.emplace_back("RegisterResourceSink");
            last_registered_resource_sink_trans_arg = trans_arg;
            if (register_resource_sink_result.ok)
            {
                return MaaApiResult::Ok(NextId());
            }
            return register_resource_sink_result;
        }

        MaaApiResult RegisterControllerSink(
            MaaControllerHandle,
            MaaMsgCallbackFn,
            void* trans_arg) override
        {
            calls.emplace_back("RegisterControllerSink");
            last_registered_controller_sink_trans_arg = trans_arg;
            if (register_controller_sink_result.ok)
            {
                return MaaApiResult::Ok(NextId());
            }
            return register_controller_sink_result;
        }

        MaaAgentClientHandle CreateAgentClientV2(
            std::optional<std::string_view> identifier) override
        {
            last_agent_client_identifier =
                identifier ? std::optional<std::string>(*identifier)
                           : std::nullopt;
            calls.emplace_back(
                "CreateAgentClientV2:"
                + last_agent_client_identifier.value_or("<auto>"));
            return NextHandle();
        }

        MaaAgentClientHandle CreateAgentClientTcp(std::uint16_t port) override
        {
            last_agent_client_tcp_port = port;
            calls.emplace_back("CreateAgentClientTcp:" + std::to_string(port));
            return NextHandle();
        }

        void DestroyAgentClient(MaaAgentClientHandle client) noexcept override
        {
            calls.emplace_back("DestroyAgentClient:" + std::to_string(client));
        }

        std::optional<std::string> GetAgentClientIdentifier(
            MaaAgentClientHandle) override
        {
            calls.emplace_back("GetAgentClientIdentifier");
            if (identifier_index_ < agent_client_identifiers.size())
            {
                return agent_client_identifiers[identifier_index_++];
            }
            return "agent-client-id";
        }

        MaaApiResult BindAgentClientResource(
            MaaAgentClientHandle,
            MaaResourceHandle resource) override
        {
            last_bound_agent_resource = resource;
            calls.emplace_back("BindAgentClientResource");
            return bind_agent_client_resource_result;
        }

        MaaApiResult RegisterAgentClientResourceSink(
            MaaAgentClientHandle,
            MaaResourceHandle resource) override
        {
            last_registered_agent_resource_sink = resource;
            calls.emplace_back("RegisterAgentClientResourceSink");
            return register_agent_client_resource_sink_result;
        }

        MaaApiResult RegisterAgentClientControllerSink(
            MaaAgentClientHandle,
            MaaControllerHandle controller) override
        {
            last_registered_agent_controller_sink = controller;
            calls.emplace_back("RegisterAgentClientControllerSink");
            return register_agent_client_controller_sink_result;
        }

        MaaApiResult RegisterAgentClientTaskerSink(
            MaaAgentClientHandle,
            MaaTaskerHandle tasker) override
        {
            last_registered_agent_tasker_sink = tasker;
            calls.emplace_back("RegisterAgentClientTaskerSink");
            return register_agent_client_tasker_sink_result;
        }

        MaaApiResult SetAgentClientTimeout(
            MaaAgentClientHandle,
            std::int64_t milliseconds) override
        {
            calls.emplace_back(
                "SetAgentClientTimeout:" + std::to_string(milliseconds));
            return set_agent_client_timeout_result;
        }

        MaaApiResult ConnectAgentClient(MaaAgentClientHandle) override
        {
            calls.emplace_back("ConnectAgentClient");
            return connect_agent_client_result;
        }

        bool DisconnectAgentClient(
            MaaAgentClientHandle client) noexcept override
        {
            calls.emplace_back(
                "DisconnectAgentClient:" + std::to_string(client));
            return true;
        }

        bool IsAgentClientConnected(MaaAgentClientHandle) override
        {
            calls.emplace_back("IsAgentClientConnected");
            return true;
        }

        bool IsAgentClientAlive(MaaAgentClientHandle) override
        {
            calls.emplace_back("IsAgentClientAlive");
            return true;
        }

        bool Contains(std::string_view expected) const
        {
            return std::find(calls.begin(), calls.end(), expected)
                   != calls.end();
        }

        // Substring match across recorded calls. PostTask calls carry a
        // pipeline_override suffix (e.g. "PostTask:StartE2E:<json>"), so a
        // prefix cannot be matched by Contains (which requires an exact
        // whole-string match).
        bool ContainsSubstring(std::string_view needle) const
        {
            return std::any_of(
                calls.begin(),
                calls.end(),
                [&](const std::string& call)
                { return call.find(needle) != std::string::npos; });
        }

    private:
        MaaResourceHandle NextHandle() { return next_handle_++; }
        MaaAsyncId        NextId() { return next_id_++; }

        MaaResourceHandle next_handle_ = 1;
        MaaAsyncId        next_id_ = 100;
        std::string       last_entry_;
        std::size_t       identifier_index_ = 0;
    };
} // namespace Das::Plugins::DasMaaPi::Test
