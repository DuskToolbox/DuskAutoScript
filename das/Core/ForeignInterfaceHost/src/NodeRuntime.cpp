#define _CRT_SECURE_NO_WARNINGS

#include <das/Core/ForeignInterfaceHost/NodeRuntime.h>

#include <das/Utils/StringUtils.h>

#include <das/DasApi.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <tl/expected.hpp>
#include <utility>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_BOOST_PROCESS_WARNING

#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/pid.hpp>

DAS_DISABLE_WARNING_END

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace
{
    constexpr auto NODE_HOST_EXECUTABLE_ENV = "DAS_NODE_HOST_EXE_PATH";
    constexpr auto NODE_EXECUTABLE_NAME = "node";
    constexpr auto RUNTIME_PACKAGE_NAME = "das-core-node";
    constexpr auto PACKAGE_JSON_FILE_NAME = "package.json";
    constexpr auto PACKAGE_ENTRY_FILE_NAME = "index.cjs";
    constexpr auto HOST_SCRIPT_RELATIVE = "bin/das-node-host.cjs";
    constexpr auto WRAPPER_FILE_NAME = "das_core_napi_export.js";
    constexpr auto ADDON_RELATIVE = "native/das_core_napi.node";

#ifdef _WIN32
    constexpr auto DYNAMIC_LIBRARY_PATH_ENV = "PATH";
#elif defined(__APPLE__)
    constexpr auto DYNAMIC_LIBRARY_PATH_ENV = "DYLD_LIBRARY_PATH";
#else
    constexpr auto DYNAMIC_LIBRARY_PATH_ENV = "LD_LIBRARY_PATH";
#endif

    bool IsPresentFile(const std::filesystem::path& path)
    {
        std::error_code ec;
        return std::filesystem::is_regular_file(path, ec) && !ec;
    }

    auto MakeReadOnlyString(const std::string& value)
        -> DAS::Utils::Expected<DAS::DasPtr<IDasReadOnlyString>>
    {
        DAS::DasPtr<IDasReadOnlyString> result;
        const auto                      create_result =
            CreateIDasReadOnlyStringFromUtf8(value.c_str(), result.Put());
        if (DAS::IsFailed(create_result))
        {
            return tl::make_unexpected(create_result);
        }
        return result;
    }

    std::string EscapeJsonString(const std::string& value)
    {
        std::string result;
        for (const char ch : value)
        {
            switch (ch)
            {
            case '\\':
                result += "\\\\";
                break;
            case '"':
                result += "\\\"";
                break;
            case '\b':
                result += "\\b";
                break;
            case '\f':
                result += "\\f";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                result += ch;
                break;
            }
        }
        return result;
    }

    auto MakeEnvironmentConfig(const std::filesystem::path& app_root)
        -> DAS::Utils::Expected<DAS::DasPtr<IDasReadOnlyString>>
    {
        const auto value =
            std::string{DAS::Utils::U8AsString(app_root.u8string())};
        if (value.empty())
        {
            return tl::make_unexpected(DAS_E_INVALID_ARGUMENT);
        }

        const std::string config =
            std::string{R"({"version":1,"prepend":[{"name":")"}
            + DYNAMIC_LIBRARY_PATH_ENV + R"(","value":")"
            + EscapeJsonString(value) + R"("}]})";
        return MakeReadOnlyString(config);
    }

    auto ValidatePackagePaths(const NodeRuntimePackagePaths& paths) -> DasResult
    {
        std::error_code ec;
        if (paths.package_root.empty() || paths.node_modules_root.empty()
            || paths.runtime_root.empty() || paths.app_root.empty())
        {
            return DAS_E_INVALID_ARGUMENT;
        }

        if (!std::filesystem::is_directory(paths.runtime_root, ec) || ec
            || !IsPresentFile(paths.package_json_path)
            || !IsPresentFile(paths.public_entry_path)
            || !IsPresentFile(paths.host_script_path)
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
      working_directory{std::move(other.working_directory)},
      environment_config{std::move(other.environment_config)}
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
        environment_config = std::move(other.environment_config);
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
    launch_desc.p_environment_config = environment_config.Get();
}

NodeRuntime::NodeRuntime(
    std::filesystem::path package_root,
    std::filesystem::path node_modules_root)
    : package_root_{std::move(package_root)},
      node_modules_root_{std::move(node_modules_root)}
{
}

NodeRuntime::NodeRuntime(
    std::unique_ptr<IRemotePluginHost> remote_plugin_host,
    std::chrono::milliseconds          timeout)
    : remote_plugin_host_{std::move(remote_plugin_host)}, timeout_{timeout}
{
}

auto NodeRuntime::ResolveNodeExecutable()
    -> DAS::Utils::Expected<std::filesystem::path>
{
    const char* env_value = std::getenv(NODE_HOST_EXECUTABLE_ENV);
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
        boost::process::v2::environment::find_executable(NODE_EXECUTABLE_NAME);
    if (resolved.empty())
    {
        return tl::make_unexpected(DAS_E_FILE_NOT_FOUND);
    }
    return std::filesystem::path{resolved.string()};
}

auto NodeRuntime::ResolvePackagePaths(
    const std::filesystem::path& package_root,
    const std::filesystem::path& node_modules_root) -> NodeRuntimePackagePaths
{
    const auto runtime_root = node_modules_root / RUNTIME_PACKAGE_NAME;
    return NodeRuntimePackagePaths{
        .package_root = package_root,
        .node_modules_root = node_modules_root,
        .runtime_root = runtime_root,
        .package_json_path = runtime_root / PACKAGE_JSON_FILE_NAME,
        .public_entry_path = runtime_root / PACKAGE_ENTRY_FILE_NAME,
        .host_script_path =
            runtime_root / std::filesystem::path{HOST_SCRIPT_RELATIVE},
        .wrapper_path = runtime_root / WRAPPER_FILE_NAME,
        .addon_path = runtime_root / std::filesystem::path{ADDON_RELATIVE},
        .app_root = node_modules_root.parent_path().parent_path()};
}

auto NodeRuntime::BuildHostLaunchDesc() const
    -> DAS::Utils::Expected<NodeHostLaunchDesc>
{
    if (package_root_.empty() || node_modules_root_.empty())
    {
        return tl::make_unexpected(DAS_E_INVALID_ARGUMENT);
    }

    auto executable_path = ResolveNodeExecutable();
    if (!executable_path)
    {
        return tl::make_unexpected(executable_path.error());
    }

    auto       paths = ResolvePackagePaths(package_root_, node_modules_root_);
    const auto package_result = ValidatePackagePaths(paths);
    if (DAS::IsFailed(package_result))
    {
        return tl::make_unexpected(package_result);
    }

    NodeHostLaunchDesc result{};
    result.resolved_node_executable = executable_path.value();
    result.package_paths = std::move(paths);

    auto executable = MakeReadOnlyString(
        std::string{DAS::Utils::U8AsString(
            result.resolved_node_executable.u8string())});
    if (!executable)
    {
        return tl::make_unexpected(executable.error());
    }
    result.executable = std::move(executable.value());

    const std::vector<std::string> args = {
        std::string{DAS::Utils::U8AsString(
            result.package_paths.host_script_path.u8string())},
        "--main-pid",
        std::to_string(
            static_cast<uint32_t>(boost::process::v2::current_pid())),
        "--package-root",
        std::string{DAS::Utils::U8AsString(
            result.package_paths.package_root.u8string())},
        "--node-modules-root",
        std::string{DAS::Utils::U8AsString(
            result.package_paths.node_modules_root.u8string())}};

    result.arg_storage.reserve(args.size());
    for (const auto& arg : args)
    {
        auto stored_arg = MakeReadOnlyString(arg);
        if (!stored_arg)
        {
            return tl::make_unexpected(stored_arg.error());
        }
        result.arg_storage.push_back(std::move(stored_arg.value()));
    }

    auto working_directory = MakeReadOnlyString(
        std::string{DAS::Utils::U8AsString(
            result.package_paths.package_root.u8string())});
    if (!working_directory)
    {
        return tl::make_unexpected(working_directory.error());
    }
    auto environment_config =
        MakeEnvironmentConfig(result.package_paths.app_root);
    if (!environment_config)
    {
        return tl::make_unexpected(environment_config.error());
    }

    result.arg_ptrs.reserve(result.arg_storage.size());
    for (const auto& arg : result.arg_storage)
    {
        result.arg_ptrs.push_back(arg.Get());
    }
    result.working_directory = std::move(working_directory.value());
    result.environment_config = std::move(environment_config.value());
    result.RefreshPointers();
    return result;
}

auto NodeRuntime::LoadPlugin(const RuntimeLoadRequest& request)
    -> DAS::Utils::Expected<RuntimeLoadResult>
{
    if (!remote_plugin_host_)
    {
        return tl::make_unexpected(DAS_E_OBJECT_NOT_INIT);
    }

    if (request.manifest_path.empty() || request.node_modules_root.empty())
    {
        return tl::make_unexpected(DAS_E_INVALID_ARGUMENT);
    }

    const auto package_root = request.manifest_path.parent_path();
    if (package_root.empty())
    {
        return tl::make_unexpected(DAS_E_INVALID_ARGUMENT);
    }

    NodeRuntime resolver{package_root, request.node_modules_root};
    auto        launch = resolver.BuildHostLaunchDesc();
    if (!launch)
    {
        return tl::make_unexpected(launch.error());
    }

    RemotePluginLoadRequest remote_request{};
    remote_request.launch_desc = launch->launch_desc;
    remote_request.manifest_path = request.manifest_path;
    remote_request.plugin_guid = request.plugin_guid;
    remote_request.timeout = timeout_;
    remote_request.on_process_exit = request.on_process_exit;
    remote_request.on_heartbeat_timeout = request.on_heartbeat_timeout;

    return remote_plugin_host_->LoadPlugin(remote_request);
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
