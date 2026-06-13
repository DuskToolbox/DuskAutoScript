#ifndef DAS_CORE_FOREIGNINTERFACEHOST_RUNTIMEPROVIDER_H
#define DAS_CORE_FOREIGNINTERFACEHOST_RUNTIMEPROVIDER_H

#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHostEnum.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <das/Utils/Expected.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class IRemotePluginHost;

struct RuntimeLoadRequest
{
    std::filesystem::path    manifest_path;
    std::filesystem::path    runtime_path;
    std::filesystem::path    node_modules_root;
    DasGuid                  plugin_guid{};
    ForeignInterfaceLanguage language{};
    LoadMode                 load_mode{LoadMode::InProcess};
    uint16_t                 main_process_owner_session_id = 0;

    // IPC lifecycle callbacks (set by PluginManager for IPC mode).
    // on_process_exit fires on Host process termination (session_id +
    // exit_code). on_heartbeat_timeout fires when IPC heartbeat times out
    // (plugin guid).
    std::function<void(uint16_t session_id, int exit_code)> on_process_exit;
    std::function<void(DasGuid guid)> on_heartbeat_timeout;
};

struct RuntimeLoadResult
{
    DAS::DasPtr<IDasBase> object;
    uint16_t              owner_session_id = 0;
};

struct RuntimeProviderFactoryDesc
{
    ForeignInterfaceLanguage           language{};
    LoadMode                           load_mode{LoadMode::InProcess};
    std::filesystem::path              native_host_exe_path;
    std::unique_ptr<IRemotePluginHost> remote_plugin_host;
};

class IRuntimeProvider
{
public:
    virtual ~IRuntimeProvider() = default;

    virtual auto LoadPlugin(const RuntimeLoadRequest& request)
        -> DAS::Utils::Expected<RuntimeLoadResult> = 0;
};

DAS_API auto CreateLocalRuntimeProvider(
    DAS::DasPtr<IForeignLanguageRuntime> runtime)
    -> DAS::Utils::Expected<std::unique_ptr<IRuntimeProvider>>;

DAS_API auto CreateLocalRuntimeProvider(
    const ForeignLanguageRuntimeFactoryDesc& desc)
    -> DAS::Utils::Expected<std::unique_ptr<IRuntimeProvider>>;

DAS_API auto CreateNativeIpcRuntimeProvider(
    std::filesystem::path              host_exe_path,
    std::unique_ptr<IRemotePluginHost> remote_plugin_host)
    -> DAS::Utils::Expected<std::unique_ptr<IRuntimeProvider>>;

DAS_API auto CreateNodeRuntimeProvider(
    std::unique_ptr<IRemotePluginHost> remote_plugin_host)
    -> DAS::Utils::Expected<std::unique_ptr<IRuntimeProvider>>;

DAS_API auto CreateRuntimeProvider(RuntimeProviderFactoryDesc desc)
    -> DAS::Utils::Expected<std::unique_ptr<IRuntimeProvider>>;

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_RUNTIMEPROVIDER_H
