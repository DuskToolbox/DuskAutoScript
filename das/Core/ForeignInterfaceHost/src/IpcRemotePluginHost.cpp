#include <das/Core/ForeignInterfaceHost/RemotePluginHost.h>

#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/Logger/Logger.h>

#include <limits>
#include <tl/expected.hpp>
#include <utility>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace
{
    auto TimeoutToMilliseconds(std::chrono::milliseconds timeout)
        -> DAS::Utils::Expected<uint32_t>
    {
        if (timeout.count() < 0)
        {
            return tl::make_unexpected(DAS_E_INVALID_ARGUMENT);
        }

        if (timeout.count() > std::numeric_limits<uint32_t>::max())
        {
            return tl::make_unexpected(DAS_E_INVALID_ARGUMENT);
        }

        return static_cast<uint32_t>(timeout.count());
    }

    auto StopAndReturnError(
        DAS::Core::IPC::IHostLauncher& launcher,
        DasResult result) -> DAS::Utils::Expected<RuntimeLoadResult>
    {
        launcher.Stop();
        return tl::make_unexpected(result);
    }
} // namespace

IpcRemotePluginHost::IpcRemotePluginHost(
    DAS::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context)
    : ipc_context_{std::move(ipc_context)}
{
}

auto IpcRemotePluginHost::LoadPlugin(const RemotePluginLoadRequest& request)
    -> DAS::Utils::Expected<RuntimeLoadResult>
{
    auto timeout_ms = TimeoutToMilliseconds(request.timeout);
    if (!timeout_ms)
    {
        return tl::make_unexpected(timeout_ms.error());
    }

    // Session-based routing: use already-connected Host via WebSocket
    if (request.target_session_id.has_value())
    {
        const uint16_t    session_id = request.target_session_id.value();
        const std::string manifest_str = request.remote_manifest_path.empty()
                                             ? request.manifest_path.string()
                                             : request.remote_manifest_path;

        DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
        auto result = ipc_context_.get().LoadPluginAsync(
            session_id,
            manifest_str.c_str(),
            op.Put(),
            request.timeout);
        if (DAS::IsFailed(result))
        {
            DAS_CORE_LOG_ERROR(
                "IpcRemotePluginHost: LoadPluginAsync(session_id={}) failed for {}, result={}",
                session_id,
                manifest_str,
                result);
            return tl::make_unexpected(result);
        }

        auto sender =
            DAS::Core::IPC::async_op(ipc_context_.get(), std::move(op));
        auto wait_result =
            DAS::Core::IPC::wait(ipc_context_.get(), std::move(sender));
        if (!wait_result)
        {
            DAS_CORE_LOG_ERROR(
                "IpcRemotePluginHost: timed out waiting for remote plugin load: {}",
                manifest_str);
            return tl::make_unexpected(DAS_E_IPC_REMOTE_ERROR);
        }

        auto [ipc_result, raw_proxy] = *wait_result;
        if (DAS::IsFailed(ipc_result) || !raw_proxy)
        {
            DAS_CORE_LOG_ERROR(
                "IpcRemotePluginHost: remote plugin load failed for {}, result={}",
                manifest_str,
                ipc_result);
            if (raw_proxy)
            {
                raw_proxy->Release();
            }
            return tl::make_unexpected(
                DAS::IsFailed(ipc_result) ? ipc_result
                                          : DAS_E_IPC_REMOTE_ERROR);
        }

        RuntimeLoadResult load_result{};
        load_result.object = DAS::DasPtr<IDasBase>::Attach(raw_proxy);
        load_result.owner_session_id = session_id;
        return load_result;
    }

    // Original path: create child process via HostLauncher
    DAS::Core::IPC::IHostLauncher* raw_launcher = nullptr;
    auto result = ipc_context_.get().CreateHostLauncher(&raw_launcher);
    if (DAS::IsFailed(result) || !raw_launcher)
    {
        if (DAS::IsOk(result))
        {
            result = DAS_E_INVALID_POINTER;
        }
        DAS_CORE_LOG_ERROR(
            "IpcRemotePluginHost: CreateHostLauncher failed, result={}",
            result);
        return tl::make_unexpected(result);
    }

    DAS::DasPtr<DAS::Core::IPC::IHostLauncher> launcher(raw_launcher);

    uint16_t owner_session_id = 0;
    result = launcher->StartWithDesc(
        &request.launch_desc,
        *timeout_ms,
        &owner_session_id);
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR(
            "IpcRemotePluginHost: StartWithDesc failed, result={}",
            result);
        launcher->Stop();
        return tl::make_unexpected(result);
    }

    if (auto* concrete_launcher =
            dynamic_cast<DAS::Core::IPC::HostLauncher*>(launcher.Get()))
    {
        concrete_launcher->SetAssociatedGuid(request.plugin_guid);
        if (request.on_process_exit)
        {
            concrete_launcher->SetOnProcessExit(request.on_process_exit);
        }
        if (request.on_heartbeat_timeout)
        {
            concrete_launcher->SetOnHeartbeatTimeout(
                request.on_heartbeat_timeout);
        }
    }

    DAS::DasPtr<IDasAsyncLoadPluginOperation> op;
    const auto manifest_path = request.manifest_path.string();
    result = ipc_context_.get().LoadPluginAsync(
        launcher.Get(),
        manifest_path.c_str(),
        op.Put(),
        request.timeout);
    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR(
            "IpcRemotePluginHost: LoadPluginAsync failed for {}, result={}",
            manifest_path,
            result);
        return StopAndReturnError(*launcher, result);
    }

    auto sender = DAS::Core::IPC::async_op(ipc_context_.get(), std::move(op));
    auto wait_result =
        DAS::Core::IPC::wait(ipc_context_.get(), std::move(sender));
    if (!wait_result)
    {
        DAS_CORE_LOG_ERROR(
            "IpcRemotePluginHost: timed out waiting for remote plugin load: {}",
            manifest_path);
        return StopAndReturnError(*launcher, DAS_E_IPC_REMOTE_ERROR);
    }

    auto [ipc_result, raw_proxy] = *wait_result;
    if (DAS::IsFailed(ipc_result) || !raw_proxy)
    {
        DAS_CORE_LOG_ERROR(
            "IpcRemotePluginHost: remote plugin load failed for {}, result={}",
            manifest_path,
            ipc_result);
        if (raw_proxy)
        {
            raw_proxy->Release();
        }
        return StopAndReturnError(
            *launcher,
            DAS::IsFailed(ipc_result) ? ipc_result : DAS_E_IPC_REMOTE_ERROR);
    }

    RuntimeLoadResult load_result{};
    load_result.object = DAS::DasPtr<IDasBase>::Attach(raw_proxy);
    load_result.owner_session_id = owner_session_id;
    return load_result;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
