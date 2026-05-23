#include <das/Core/ForeignInterfaceHost/NodeRuntime.h>

#include <das/DasApi.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <tl/expected.hpp>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define DAS_NODE_RUNTIME_CURRENT_PROCESS_ID() GetCurrentProcessId()
#else
#include <unistd.h>
#define DAS_NODE_RUNTIME_CURRENT_PROCESS_ID() getpid()
#endif

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_BOOST_PROCESS_WARNING

#include <boost/process/v2/environment.hpp>

DAS_DISABLE_WARNING_END

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace
{
    constexpr auto kNodeHostExecutableEnv = "DAS_NODE_HOST_EXE_PATH";
    constexpr auto kNodeExecutableName = "node";
    constexpr auto kHostScriptFileName = "das-node-host.cjs";
    constexpr auto kWrapperFileName = "das_core_napi_export.js";
    constexpr auto kAddonFileName = "das_core_napi.node";

    bool IsPresentFile(const std::filesystem::path& path)
    {
        std::error_code ec;
        return std::filesystem::is_regular_file(path, ec) && !ec;
    }

    auto MakeReadOnlyString(const std::string& value)
        -> DAS::Utils::Expected<DAS::DasPtr<IDasReadOnlyString>>
    {
        DAS::DasPtr<IDasReadOnlyString> result;
        const auto create_result =
            CreateIDasReadOnlyStringFromUtf8(value.c_str(), result.Put());
        if (DAS::IsFailed(create_result))
        {
            return tl::make_unexpected(create_result);
        }
        return result;
    }

    auto ValidatePackagePaths(const NodeRuntimePackagePaths& paths)
        -> DasResult
    {
        if (!IsPresentFile(paths.host_script_path)
            || !IsPresentFile(paths.wrapper_path)
            || !IsPresentFile(paths.addon_path))
        {
            return DAS_E_FILE_NOT_FOUND;
        }
        return DAS_S_OK;
    }
} // namespace

NodeHostLaunchDesc::NodeHostLaunchDesc(NodeHostLaunchDesc&& other) noexcept
    : launch_desc{other.launch_desc},
      resolved_node_executable{std::move(other.resolved_node_executable)},
      package_paths{std::move(other.package_paths)},
      executable{std::move(other.executable)},
      arg_storage{std::move(other.arg_storage)},
      arg_ptrs{std::move(other.arg_ptrs)},
      working_directory{std::move(other.working_directory)}
{
    RefreshPointers();
    other.RefreshPointers();
}

NodeHostLaunchDesc& NodeHostLaunchDesc::operator=(
    NodeHostLaunchDesc&& other) noexcept
{
    if (this != &other)
    {
        launch_desc = other.launch_desc;
        resolved_node_executable = std::move(other.resolved_node_executable);
        package_paths = std::move(other.package_paths);
        executable = std::move(other.executable);
        arg_storage = std::move(other.arg_storage);
        arg_ptrs = std::move(other.arg_ptrs);
        working_directory = std::move(other.working_directory);
        RefreshPointers();
        other.RefreshPointers();
    }
    return *this;
}

void NodeHostLaunchDesc::RefreshPointers() noexcept
{
    launch_desc.p_executable_path = executable.Get();
    launch_desc.pp_args = arg_ptrs.empty() ? nullptr : arg_ptrs.data();
    launch_desc.arg_count = arg_ptrs.size();
    launch_desc.p_working_directory = working_directory.Get();
}

NodeRuntime::NodeRuntime(std::filesystem::path package_dir)
    : package_dir_{std::move(package_dir)}
{
}

auto NodeRuntime::ResolveNodeExecutable()
    -> DAS::Utils::Expected<std::filesystem::path>
{
    const char* env_value = std::getenv(kNodeHostExecutableEnv);
    if (env_value && env_value[0] != '\0')
    {
        std::filesystem::path strict_path{env_value};
        if (!IsPresentFile(strict_path))
        {
            return tl::make_unexpected(DAS_E_FILE_NOT_FOUND);
        }
        return strict_path;
    }

    const auto resolved =
        boost::process::v2::environment::find_executable(kNodeExecutableName);
    if (resolved.empty())
    {
        return tl::make_unexpected(DAS_E_FILE_NOT_FOUND);
    }
    return std::filesystem::path{resolved.string()};
}

auto NodeRuntime::ResolvePackagePaths(const std::filesystem::path& package_dir)
    -> NodeRuntimePackagePaths
{
    return NodeRuntimePackagePaths{
        .host_script_path = package_dir / kHostScriptFileName,
        .wrapper_path = package_dir / kWrapperFileName,
        .addon_path = package_dir / kAddonFileName,
        .host_script_relative = std::string{"./"} + kHostScriptFileName,
        .wrapper_relative = std::string{"./"} + kWrapperFileName,
        .addon_relative = std::string{"./"} + kAddonFileName};
}

auto NodeRuntime::BuildHostLaunchDesc() const
    -> DAS::Utils::Expected<NodeHostLaunchDesc>
{
    auto executable_path = ResolveNodeExecutable();
    if (!executable_path)
    {
        return tl::make_unexpected(executable_path.error());
    }

    auto paths = ResolvePackagePaths(package_dir_);
    const auto package_result = ValidatePackagePaths(paths);
    if (DAS::IsFailed(package_result))
    {
        return tl::make_unexpected(package_result);
    }

    NodeHostLaunchDesc result{};
    result.resolved_node_executable = executable_path.value();
    result.package_paths = std::move(paths);

    auto executable = MakeReadOnlyString(result.resolved_node_executable.string());
    if (!executable)
    {
        return tl::make_unexpected(executable.error());
    }
    result.executable = std::move(executable.value());

    auto script_arg =
        MakeReadOnlyString(result.package_paths.host_script_path.string());
    if (!script_arg)
    {
        return tl::make_unexpected(script_arg.error());
    }
    auto main_pid_arg = MakeReadOnlyString("--main-pid");
    if (!main_pid_arg)
    {
        return tl::make_unexpected(main_pid_arg.error());
    }
    auto pid_arg = MakeReadOnlyString(std::to_string(
        static_cast<uint32_t>(DAS_NODE_RUNTIME_CURRENT_PROCESS_ID())));
    if (!pid_arg)
    {
        return tl::make_unexpected(pid_arg.error());
    }
    auto working_directory = MakeReadOnlyString(package_dir_.string());
    if (!working_directory)
    {
        return tl::make_unexpected(working_directory.error());
    }

    result.arg_storage.push_back(std::move(script_arg.value()));
    result.arg_storage.push_back(std::move(main_pid_arg.value()));
    result.arg_storage.push_back(std::move(pid_arg.value()));
    result.arg_ptrs.reserve(result.arg_storage.size());
    for (const auto& arg : result.arg_storage)
    {
        result.arg_ptrs.push_back(arg.Get());
    }
    result.working_directory = std::move(working_directory.value());
    result.RefreshPointers();
    return result;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
