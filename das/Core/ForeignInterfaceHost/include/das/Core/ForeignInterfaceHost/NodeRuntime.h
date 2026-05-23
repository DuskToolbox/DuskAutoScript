#ifndef DAS_CORE_FOREIGNINTERFACEHOST_NODERUNTIME_H
#define DAS_CORE_FOREIGNINTERFACEHOST_NODERUNTIME_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/RemotePluginHost.h>
#include <das/Core/IPC/MainProcess/IHostLauncher.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/Expected.h>

#include <filesystem>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

struct NodeRuntimePackagePaths
{
    std::filesystem::path host_script_path;
    std::filesystem::path wrapper_path;
    std::filesystem::path addon_path;
    std::string           host_script_relative;
    std::string           wrapper_relative;
    std::string           addon_relative;
};

struct NodeHostLaunchDesc
{
    NodeHostLaunchDesc() = default;
    NodeHostLaunchDesc(const NodeHostLaunchDesc&) = delete;
    NodeHostLaunchDesc& operator=(const NodeHostLaunchDesc&) = delete;
    NodeHostLaunchDesc(NodeHostLaunchDesc&& other) noexcept;
    NodeHostLaunchDesc& operator=(NodeHostLaunchDesc&& other) noexcept;

    void RefreshPointers() noexcept;

    DAS::Core::IPC::HostLaunchDesc launch_desc{};
    std::filesystem::path          resolved_node_executable;
    NodeRuntimePackagePaths        package_paths;
    DAS::DasPtr<IDasReadOnlyString> executable;
    std::vector<DAS::DasPtr<IDasReadOnlyString>> arg_storage;
    std::vector<IDasReadOnlyString*>             arg_ptrs;
    DAS::DasPtr<IDasReadOnlyString> working_directory;
};

class NodeRuntime final : public IRuntimeProvider
{
public:
    explicit NodeRuntime(std::filesystem::path package_dir);
    explicit NodeRuntime(
        std::unique_ptr<IRemotePluginHost> remote_plugin_host,
        std::chrono::milliseconds timeout = std::chrono::seconds{30});

    static auto ResolveNodeExecutable()
        -> DAS::Utils::Expected<std::filesystem::path>;

    static auto ResolvePackagePaths(const std::filesystem::path& package_dir)
        -> NodeRuntimePackagePaths;

    auto BuildHostLaunchDesc() const
        -> DAS::Utils::Expected<NodeHostLaunchDesc>;

    auto LoadPlugin(const RuntimeLoadRequest& request)
        -> DAS::Utils::Expected<RuntimeLoadResult> override;

private:
    std::filesystem::path package_dir_;
    std::unique_ptr<IRemotePluginHost> remote_plugin_host_;
    std::chrono::milliseconds          timeout_{std::chrono::seconds{30}};
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_NODERUNTIME_H
