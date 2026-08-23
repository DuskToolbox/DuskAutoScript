#ifndef DAS_CORE_FOREIGNINTERFACEHOST_REMOTEPLUGINHOST_H
#define DAS_CORE_FOREIGNINTERFACEHOST_REMOTEPLUGINHOST_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/RuntimeProvider.h>
#include <das/Core/IPC/MainProcess/IHostLauncher.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/DasSharedRef.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

struct RemotePluginLoadRequest
{
    DAS::Core::IPC::HostLaunchDesc launch_desc{};
    std::filesystem::path          manifest_path;
    DasGuid                        plugin_guid{};
    std::chrono::milliseconds      timeout{std::chrono::seconds{30}};
    std::function<void(uint16_t session_id, int exit_code)> on_process_exit;
    std::function<void(DasGuid guid)> on_heartbeat_timeout;

    std::optional<uint16_t> target_session_id;
    std::string             remote_manifest_path;
};

class IRemotePluginHost
{
public:
    virtual ~IRemotePluginHost() = default;

    virtual auto LoadPlugin(const RemotePluginLoadRequest& request)
        -> DAS::Utils::Expected<RuntimeLoadResult> = 0;
};

class IpcRemotePluginHost final : public IRemotePluginHost
{
public:
    explicit IpcRemotePluginHost(
        DAS::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext>
            ipc_context);

    auto LoadPlugin(const RemotePluginLoadRequest& request)
        -> DAS::Utils::Expected<RuntimeLoadResult> override;

private:
    DAS::DasSharedRef<DAS::Core::IPC::MainProcess::IIpcContext> ipc_context_;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_REMOTEPLUGINHOST_H
