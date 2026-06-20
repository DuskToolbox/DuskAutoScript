#ifndef DAS_CORE_FOREIGNINTERFACEHOST_NATIVEIPCRUNTIME_H
#define DAS_CORE_FOREIGNINTERFACEHOST_NATIVEIPCRUNTIME_H

#include <das/Core/ForeignInterfaceHost/RemotePluginHost.h>

#include <chrono>
#include <filesystem>
#include <memory>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class NativeIpcRuntime final : public IRuntimeProvider
{
public:
    explicit NativeIpcRuntime(
        std::filesystem::path              host_exe_path,
        std::unique_ptr<IRemotePluginHost> remote_plugin_host,
        RuntimeLifecycleCallbacks          callbacks,
        std::chrono::milliseconds          timeout = std::chrono::seconds{30});

    DasResult LoadPlugin(
        const RuntimeLoadRequest& request,
        RuntimeLoadResult*        out_result) override;

private:
    std::filesystem::path              host_exe_path_;
    std::unique_ptr<IRemotePluginHost> remote_plugin_host_;
    RuntimeLifecycleCallbacks          callbacks_;
    std::chrono::milliseconds          timeout_;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_NATIVEIPCRUNTIME_H
