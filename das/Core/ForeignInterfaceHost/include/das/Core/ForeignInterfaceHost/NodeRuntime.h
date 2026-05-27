#ifndef DAS_CORE_FOREIGNINTERFACEHOST_NODERUNTIME_H
#define DAS_CORE_FOREIGNINTERFACEHOST_NODERUNTIME_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/RemotePluginHost.h>
#include <das/Core/IPC/MainProcess/IHostLauncher.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/Expected.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

struct NodeRuntimePackagePaths
{
    std::filesystem::path package_root;
    std::filesystem::path node_modules_root;
    std::filesystem::path runtime_root;
    std::filesystem::path package_json_path;
    std::filesystem::path public_entry_path;
    std::filesystem::path host_script_path;
    std::filesystem::path wrapper_path;
    std::filesystem::path addon_path;
    std::filesystem::path app_root;
};

struct NodeHostLaunchDesc
{
    NodeHostLaunchDesc() = default;
    NodeHostLaunchDesc(const NodeHostLaunchDesc&) = delete;
    NodeHostLaunchDesc& operator=(const NodeHostLaunchDesc&) = delete;
    NodeHostLaunchDesc(NodeHostLaunchDesc&& other) noexcept;
    NodeHostLaunchDesc& operator=(NodeHostLaunchDesc&& other) noexcept;

    void RefreshPointers() noexcept;

    DAS::Core::IPC::HostLaunchDesc               launch_desc{};
    std::filesystem::path                        resolved_node_executable;
    NodeRuntimePackagePaths                      package_paths;
    DAS::DasPtr<IDasReadOnlyString>              executable;
    std::vector<DAS::DasPtr<IDasReadOnlyString>> arg_storage;
    std::vector<IDasReadOnlyString*>             arg_ptrs;
    DAS::DasPtr<IDasReadOnlyString>              working_directory;
    DAS::DasPtr<IDasReadOnlyString>              environment_config;
};

class NodeRuntime final : public IRuntimeProvider
{
public:
    NodeRuntime(
        std::filesystem::path package_root,
        std::filesystem::path node_modules_root);
    explicit NodeRuntime(
        std::unique_ptr<IRemotePluginHost> remote_plugin_host,
        std::chrono::milliseconds          timeout = std::chrono::seconds{30});

    static auto ResolveNodeExecutable()
        -> DAS::Utils::Expected<std::filesystem::path>;

    static auto ResolvePackagePaths(
        const std::filesystem::path& package_root,
        const std::filesystem::path& node_modules_root)
        -> NodeRuntimePackagePaths;

    auto BuildHostLaunchDesc() const
        -> DAS::Utils::Expected<NodeHostLaunchDesc>;

    auto LoadPlugin(const RuntimeLoadRequest& request)
        -> DAS::Utils::Expected<RuntimeLoadResult> override;

private:
    std::filesystem::path              package_root_;
    std::filesystem::path              node_modules_root_;
    std::unique_ptr<IRemotePluginHost> remote_plugin_host_;
    std::chrono::milliseconds          timeout_{std::chrono::seconds{30}};
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_NODERUNTIME_H
