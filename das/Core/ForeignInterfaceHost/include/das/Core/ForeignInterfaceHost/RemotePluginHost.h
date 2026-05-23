#ifndef DAS_CORE_FOREIGNINTERFACEHOST_REMOTEPLUGINHOST_H
#define DAS_CORE_FOREIGNINTERFACEHOST_REMOTEPLUGINHOST_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/RuntimeProvider.h>
#include <das/Core/IPC/MainProcess/IHostLauncher.h>

#include <chrono>
#include <filesystem>
#include <functional>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

struct RemotePluginLoadRequest
{
    DAS::Core::IPC::HostLaunchDesc launch_desc{};
    std::filesystem::path          manifest_path;
    DasGuid                        plugin_guid{};
    std::chrono::milliseconds      timeout{std::chrono::seconds{30}};
    std::function<void(uint16_t session_id, int exit_code)> on_process_exit;
    std::function<void(DasGuid guid)> on_heartbeat_timeout;
};

class IRemotePluginHost
{
public:
    virtual ~IRemotePluginHost() = default;

    virtual auto LoadPlugin(const RemotePluginLoadRequest& request)
        -> DAS::Utils::Expected<RuntimeLoadResult> = 0;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_REMOTEPLUGINHOST_H
