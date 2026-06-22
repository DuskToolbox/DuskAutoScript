#include <das/Core/ForeignInterfaceHost/NodeRuntime.h>

#include <das/Core/ForeignInterfaceHost/RemotePluginHost.h>
#include <das/Core/IPC/MainProcess/IHostLauncher.h>
#include <das/Utils/StringUtils.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <stdlib.h>
#endif

using namespace DAS::Core::ForeignInterfaceHost;

namespace
{
    constexpr auto NODE_HOST_EXECUTABLE_ENV = "DAS_NODE_HOST_EXE_PATH";
#ifndef DAS_NODE_PACKAGE_NAME
#define DAS_NODE_PACKAGE_NAME "das-core-node"
#endif
#ifndef DAS_NODE_ADDON_NAME
#define DAS_NODE_ADDON_NAME "das_core_napi"
#endif
    constexpr auto RUNTIME_PACKAGE_NAME = DAS_NODE_PACKAGE_NAME;
    constexpr auto HOST_SCRIPT_RELATIVE = "bin/das-node-host.cjs";
    constexpr auto PACKAGE_ENTRY_RELATIVE = "index.cjs";
    constexpr auto WRAPPER_RELATIVE = DAS_NODE_ADDON_NAME "_export.js";
    constexpr auto ADDON_RELATIVE = "native/" DAS_NODE_ADDON_NAME ".node";

#ifdef _WIN32
    constexpr auto DYNAMIC_LIBRARY_PATH_ENV = "PATH";
#elif defined(__APPLE__)
    constexpr auto DYNAMIC_LIBRARY_PATH_ENV = "DYLD_LIBRARY_PATH";
#else
    constexpr auto DYNAMIC_LIBRARY_PATH_ENV = "LD_LIBRARY_PATH";
#endif

    class ScopedEnvVar
    {
    public:
        ScopedEnvVar(const char* name, std::optional<std::string> value)
            : name_{name}
        {
            const char* current = std::getenv(name_.c_str());
            if (current)
            {
                original_ = std::string{current};
            }
            Set(std::move(value));
        }

        ScopedEnvVar(const ScopedEnvVar&) = delete;
        ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

        ~ScopedEnvVar() { Set(std::move(original_)); }

    private:
        void Set(std::optional<std::string> value)
        {
#ifdef _WIN32
            _putenv_s(name_.c_str(), value ? value->c_str() : "");
#else
            if (value)
            {
                setenv(name_.c_str(), value->c_str(), 1);
            }
            else
            {
                unsetenv(name_.c_str());
            }
#endif
        }

        std::string                name_;
        std::optional<std::string> original_;
    };

    class TempNodeRuntimeLayout
    {
    public:
        TempNodeRuntimeLayout()
        {
            const auto stamp =
                std::chrono::steady_clock::now().time_since_epoch().count();
            root_ = std::filesystem::temp_directory_path()
                    / ("das-node-runtime-test-" + std::to_string(stamp));
            app_root_ = root_ / "app";
            plugins_root_ = app_root_ / "plugins";
            node_modules_root_ = plugins_root_ / "node_modules";
            runtime_root_ = node_modules_root_ / RUNTIME_PACKAGE_NAME;
            package_root_ = plugins_root_ / "NodePlugin";
            manifest_path_ = package_root_ / "NodePlugin.json";
            fake_node_ = root_ / "fake-node.exe";
            std::filesystem::create_directories(package_root_);
        }

        TempNodeRuntimeLayout(const TempNodeRuntimeLayout&) = delete;
        TempNodeRuntimeLayout& operator=(const TempNodeRuntimeLayout&) = delete;

        ~TempNodeRuntimeLayout()
        {
            std::error_code ec;
            std::filesystem::remove_all(root_, ec);
        }

        [[nodiscard]]
        const std::filesystem::path& AppRoot() const
        {
            return app_root_;
        }

        [[nodiscard]]
        const std::filesystem::path& NodeModulesRoot() const
        {
            return node_modules_root_;
        }

        [[nodiscard]]
        const std::filesystem::path& RuntimeRoot() const
        {
            return runtime_root_;
        }

        [[nodiscard]]
        const std::filesystem::path& PackageRoot() const
        {
            return package_root_;
        }

        [[nodiscard]]
        const std::filesystem::path& ManifestPath() const
        {
            return manifest_path_;
        }

        [[nodiscard]]
        const std::filesystem::path& FakeNode() const
        {
            return fake_node_;
        }

        std::filesystem::path CreateFile(const std::filesystem::path& path)
        {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream stream{path};
            stream << "\n";
            return path;
        }

        std::filesystem::path CreateRuntimeFile(std::string_view relative)
        {
            return CreateFile(runtime_root_ / std::filesystem::path{relative});
        }

        std::filesystem::path CreatePackageFile(std::string_view relative)
        {
            return CreateFile(package_root_ / std::filesystem::path{relative});
        }

        void CreateFakeNode() { CreateFile(fake_node_); }

        void CreatePluginManifest() { CreateFile(manifest_path_); }

        void CreateRuntimePackageFiles()
        {
            CreateFakeNode();
            CreatePluginManifest();
            CreateRuntimeFile("package.json");
            CreateRuntimeFile(PACKAGE_ENTRY_RELATIVE);
            CreateRuntimeFile(WRAPPER_RELATIVE);
            CreateRuntimeFile(HOST_SCRIPT_RELATIVE);
            CreateRuntimeFile(ADDON_RELATIVE);
        }

        void CreateLegacyPluginRootRuntimeFiles()
        {
            CreateFakeNode();
            CreatePluginManifest();
            CreatePackageFile("package.json");
            CreatePackageFile(PACKAGE_ENTRY_RELATIVE);
            CreatePackageFile(WRAPPER_RELATIVE);
            CreatePackageFile("das-node-host.cjs");
            CreatePackageFile(DAS_NODE_ADDON_NAME ".node");
        }

        void RemoveRuntimeFile(std::string_view relative)
        {
            std::error_code ec;
            std::filesystem::remove(
                runtime_root_ / std::filesystem::path{relative},
                ec);
        }

    private:
        std::filesystem::path root_;
        std::filesystem::path app_root_;
        std::filesystem::path plugins_root_;
        std::filesystem::path node_modules_root_;
        std::filesystem::path runtime_root_;
        std::filesystem::path package_root_;
        std::filesystem::path manifest_path_;
        std::filesystem::path fake_node_;
    };

    std::string ReadUtf8(IDasReadOnlyString* value)
    {
        const char* raw = nullptr;
        EXPECT_NE(value, nullptr);
        EXPECT_EQ(value->GetUtf8(&raw), DAS_S_OK);
        return raw ? std::string{raw} : std::string{};
    }

    std::vector<std::string> ReadArgs(
        const DAS::Core::IPC::HostLaunchDesc& desc)
    {
        std::vector<std::string> result;
        result.reserve(desc.arg_count);
        for (size_t i = 0; i < desc.arg_count; ++i)
        {
            result.push_back(ReadUtf8(desc.pp_args[i]));
        }
        return result;
    }

    std::string Lower(std::string value)
    {
        std::ranges::transform(
            value,
            value.begin(),
            [](unsigned char ch)
            { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

    std::string JsonEscaped(std::string_view value)
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
            default:
                result += ch;
                break;
            }
        }
        return result;
    }

    class RemoteHostBaseObject final : public IDasBase
    {
    public:
        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out_object) override
        {
            if (pp_out_object == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DAS_IID_BASE)
            {
                *pp_out_object = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

    private:
        std::atomic<uint32_t> ref_count_{0};
    };

    struct RemoteHostCapture
    {
        int                      load_count = 0;
        std::filesystem::path    manifest_path;
        DasGuid                  plugin_guid{};
        std::string              executable;
        std::vector<std::string> args;
        std::string              working_directory;
        std::string              environment_config;
        bool                     has_on_process_exit = false;
        bool                     has_on_heartbeat_timeout = false;
    };

    class CapturingRemotePluginHost final : public IRemotePluginHost
    {
    public:
        CapturingRemotePluginHost(
            RemoteHostCapture& capture,
            uint16_t           owner_session_id)
            : capture_{capture}, owner_session_id_{owner_session_id}
        {
        }

        auto LoadPlugin(const RemotePluginLoadRequest& request)
            -> DAS::Utils::Expected<RuntimeLoadResult> override
        {
            capture_.load_count += 1;
            capture_.manifest_path = request.manifest_path;
            capture_.plugin_guid = request.plugin_guid;
            capture_.executable =
                ReadUtf8(request.launch_desc.p_executable_path);
            capture_.working_directory =
                ReadUtf8(request.launch_desc.p_working_directory);
            capture_.environment_config =
                ReadUtf8(request.launch_desc.p_environment_config);
            capture_.args = ReadArgs(request.launch_desc);
            capture_.has_on_process_exit =
                static_cast<bool>(request.on_process_exit);
            capture_.has_on_heartbeat_timeout =
                static_cast<bool>(request.on_heartbeat_timeout);

            auto* object = new RemoteHostBaseObject();
            object->AddRef();

            RuntimeLoadResult result{};
            result.object = static_cast<IDasBase*>(object);
            result.owner_session_id = owner_session_id_;
            return result;
        }

    private:
        RemoteHostCapture& capture_;
        uint16_t           owner_session_id_ = 0;
    };
} // namespace

TEST(NodeRuntimeResolver, UnsetEnvironmentUsesBareNodeLookup)
{
    ScopedEnvVar env{NODE_HOST_EXECUTABLE_ENV, std::nullopt};

    auto resolved = NodeRuntime::ResolveNodeExecutable();

    ASSERT_TRUE(resolved);
    EXPECT_TRUE(std::filesystem::exists(resolved.value()));
    const auto filename = Lower(resolved.value().filename().string());
    EXPECT_TRUE(filename == "node" || filename == "node.exe") << filename;
}

TEST(NodeRuntimeResolver, EmptyEnvironmentUsesBareNodeLookup)
{
    ScopedEnvVar env{NODE_HOST_EXECUTABLE_ENV, std::string{}};

    auto resolved = NodeRuntime::ResolveNodeExecutable();

    ASSERT_TRUE(resolved);
    EXPECT_TRUE(std::filesystem::exists(resolved.value()));
}

TEST(NodeRuntimeResolver, StrictEnvironmentExecutableHasNoFallback)
{
    TempNodeRuntimeLayout layout;
    layout.CreateRuntimePackageFiles();
    ScopedEnvVar env{
        NODE_HOST_EXECUTABLE_ENV,
        (layout.FakeNode().parent_path() / "missing-node.exe").string()};

    NodeRuntime runtime{layout.PackageRoot(), layout.NodeModulesRoot()};
    auto        launch = runtime.BuildHostLaunchDesc();

    ASSERT_FALSE(launch);
    EXPECT_EQ(launch.error(), DAS_E_FILE_NOT_FOUND);
}

TEST(NodeRuntimeResolver, StrictEnvironmentExecutableIsUsedWhenPresent)
{
    TempNodeRuntimeLayout layout;
    layout.CreateRuntimePackageFiles();
    ScopedEnvVar env{NODE_HOST_EXECUTABLE_ENV, layout.FakeNode().string()};

    NodeRuntime runtime{layout.PackageRoot(), layout.NodeModulesRoot()};
    auto        launch = runtime.BuildHostLaunchDesc();

    ASSERT_TRUE(launch);
    EXPECT_EQ(
        ReadUtf8(launch->launch_desc.p_executable_path),
        layout.FakeNode().string());
}

TEST(NodeRuntimePackage, ResolvesExplicitRuntimePackagePaths)
{
    TempNodeRuntimeLayout layout;

    const auto paths = NodeRuntime::ResolvePackagePaths(
        layout.PackageRoot(),
        layout.NodeModulesRoot());

    EXPECT_EQ(paths.package_root, layout.PackageRoot());
    EXPECT_EQ(paths.node_modules_root, layout.NodeModulesRoot());
    EXPECT_EQ(paths.runtime_root, layout.RuntimeRoot());
    EXPECT_EQ(paths.app_root, layout.AppRoot());
    EXPECT_EQ(paths.package_json_path, layout.RuntimeRoot() / "package.json");
    EXPECT_EQ(paths.public_entry_path, layout.RuntimeRoot() / "index.cjs");
    EXPECT_EQ(
        paths.host_script_path,
        layout.RuntimeRoot() / "bin" / "das-node-host.cjs");
    EXPECT_EQ(
        paths.wrapper_path,
        layout.RuntimeRoot() / (DAS_NODE_ADDON_NAME "_export.js"));
    EXPECT_EQ(
        paths.addon_path,
        layout.RuntimeRoot() / "native" / (DAS_NODE_ADDON_NAME ".node"));
    EXPECT_NE(paths.host_script_path.parent_path(), layout.PackageRoot());
}

TEST(NodeRuntimePackage, LegacyPluginRootRuntimeArtifactsAreIgnored)
{
    TempNodeRuntimeLayout layout;
    layout.CreateLegacyPluginRootRuntimeFiles();
    ScopedEnvVar env{NODE_HOST_EXECUTABLE_ENV, layout.FakeNode().string()};

    NodeRuntime runtime{layout.PackageRoot(), layout.NodeModulesRoot()};
    auto        launch = runtime.BuildHostLaunchDesc();

    ASSERT_FALSE(launch);
    EXPECT_EQ(launch.error(), DAS_E_FILE_NOT_FOUND);
}

TEST(NodeRuntimePackage, MissingRuntimeRootFailsDeterministically)
{
    TempNodeRuntimeLayout layout;
    layout.CreateFakeNode();
    layout.CreatePluginManifest();
    ScopedEnvVar env{NODE_HOST_EXECUTABLE_ENV, layout.FakeNode().string()};

    NodeRuntime runtime{layout.PackageRoot(), layout.NodeModulesRoot()};
    auto        launch = runtime.BuildHostLaunchDesc();

    ASSERT_FALSE(launch);
    EXPECT_EQ(launch.error(), DAS_E_FILE_NOT_FOUND);
}

TEST(NodeRuntimePackage, MissingPackageJsonFailsDeterministically)
{
    TempNodeRuntimeLayout layout;
    layout.CreateRuntimePackageFiles();
    layout.RemoveRuntimeFile("package.json");
    ScopedEnvVar env{NODE_HOST_EXECUTABLE_ENV, layout.FakeNode().string()};

    NodeRuntime runtime{layout.PackageRoot(), layout.NodeModulesRoot()};
    auto        launch = runtime.BuildHostLaunchDesc();

    ASSERT_FALSE(launch);
    EXPECT_EQ(launch.error(), DAS_E_FILE_NOT_FOUND);
}

TEST(NodeRuntimePackage, MissingPackageEntryFailsDeterministically)
{
    TempNodeRuntimeLayout layout;
    layout.CreateRuntimePackageFiles();
    layout.RemoveRuntimeFile(PACKAGE_ENTRY_RELATIVE);
    ScopedEnvVar env{NODE_HOST_EXECUTABLE_ENV, layout.FakeNode().string()};

    NodeRuntime runtime{layout.PackageRoot(), layout.NodeModulesRoot()};
    auto        launch = runtime.BuildHostLaunchDesc();

    ASSERT_FALSE(launch);
    EXPECT_EQ(launch.error(), DAS_E_FILE_NOT_FOUND);
}

TEST(NodeRuntimePackage, MissingWrapperFailsDeterministically)
{
    TempNodeRuntimeLayout layout;
    layout.CreateRuntimePackageFiles();
    layout.RemoveRuntimeFile(WRAPPER_RELATIVE);
    ScopedEnvVar env{NODE_HOST_EXECUTABLE_ENV, layout.FakeNode().string()};

    NodeRuntime runtime{layout.PackageRoot(), layout.NodeModulesRoot()};
    auto        launch = runtime.BuildHostLaunchDesc();

    ASSERT_FALSE(launch);
    EXPECT_EQ(launch.error(), DAS_E_FILE_NOT_FOUND);
}

TEST(NodeRuntimePackage, MissingHostScriptFailsDeterministically)
{
    TempNodeRuntimeLayout layout;
    layout.CreateRuntimePackageFiles();
    layout.RemoveRuntimeFile(HOST_SCRIPT_RELATIVE);
    ScopedEnvVar env{NODE_HOST_EXECUTABLE_ENV, layout.FakeNode().string()};

    NodeRuntime runtime{layout.PackageRoot(), layout.NodeModulesRoot()};
    auto        launch = runtime.BuildHostLaunchDesc();

    ASSERT_FALSE(launch);
    EXPECT_EQ(launch.error(), DAS_E_FILE_NOT_FOUND);
}

TEST(NodeRuntimePackage, MissingAddonFailsDeterministically)
{
    TempNodeRuntimeLayout layout;
    layout.CreateRuntimePackageFiles();
    layout.RemoveRuntimeFile(ADDON_RELATIVE);
    ScopedEnvVar env{NODE_HOST_EXECUTABLE_ENV, layout.FakeNode().string()};

    NodeRuntime runtime{layout.PackageRoot(), layout.NodeModulesRoot()};
    auto        launch = runtime.BuildHostLaunchDesc();

    ASSERT_FALSE(launch);
    EXPECT_EQ(launch.error(), DAS_E_FILE_NOT_FOUND);
}

TEST(NodeRuntimePackage, BuildHostLaunchDescUsesRuntimeScriptAndExplicitRoots)
{
    TempNodeRuntimeLayout layout;
    layout.CreateRuntimePackageFiles();
    ScopedEnvVar env{NODE_HOST_EXECUTABLE_ENV, layout.FakeNode().string()};

    NodeRuntime runtime{layout.PackageRoot(), layout.NodeModulesRoot()};
    auto        launch = runtime.BuildHostLaunchDesc();

    ASSERT_TRUE(launch);
    const auto& desc = launch->launch_desc;
    EXPECT_EQ(ReadUtf8(desc.p_executable_path), layout.FakeNode().string());
    ASSERT_NE(desc.pp_args, nullptr);

    const auto args = ReadArgs(desc);
    ASSERT_EQ(args.size(), 7u);
    EXPECT_EQ(args[0], (layout.RuntimeRoot() / HOST_SCRIPT_RELATIVE).string());
    EXPECT_EQ(args[1], "--main-pid");
    EXPECT_FALSE(args[2].empty());
    EXPECT_EQ(args[3], "--package-root");
    EXPECT_EQ(args[4], layout.PackageRoot().string());
    EXPECT_EQ(args[5], "--node-modules-root");
    EXPECT_EQ(args[6], layout.NodeModulesRoot().string());
    EXPECT_EQ(
        ReadUtf8(desc.p_working_directory),
        layout.PackageRoot().string());

    const auto environment_config = ReadUtf8(desc.p_environment_config);
    EXPECT_NE(environment_config.find(R"("version":1)"), std::string::npos);
    EXPECT_NE(
        environment_config.find(
            std::string{R"("name":")"} + DYNAMIC_LIBRARY_PATH_ENV + R"(")"),
        std::string::npos);
    EXPECT_NE(
        environment_config.find(JsonEscaped(layout.AppRoot().string())),
        std::string::npos);
}

TEST(NodeRuntimeRemoteHost, LoadPluginRejectsMissingManifestPath)
{
    RemoteHostCapture capture;
    NodeRuntime       runtime{
        std::make_unique<CapturingRemotePluginHost>(capture, 82),
        RuntimeLifecycleCallbacks{}};

    RuntimeLoadRequest request{};
    request.node_modules_root = "plugins/node_modules";

    RuntimeLoadResult result{};
    const auto        hr = runtime.LoadPlugin(request, &result);

    ASSERT_TRUE(DAS::IsFailed(hr));
    EXPECT_EQ(hr, DAS_E_INVALID_ARGUMENT);
    EXPECT_EQ(capture.load_count, 0);
}

TEST(NodeRuntimeRemoteHost, LoadPluginRejectsMissingNodeModulesRoot)
{
    TempNodeRuntimeLayout layout;
    layout.CreateRuntimePackageFiles();
    ScopedEnvVar      env{NODE_HOST_EXECUTABLE_ENV, layout.FakeNode().string()};
    RemoteHostCapture capture;
    NodeRuntime       runtime{
        std::make_unique<CapturingRemotePluginHost>(capture, 82),
        RuntimeLifecycleCallbacks{}};

    const std::string manifest_u8{
        DAS::Utils::U8AsString(layout.ManifestPath().u8string())};
    RuntimeLoadRequest request{};
    request.manifest_path = manifest_u8.c_str();

    RuntimeLoadResult result{};
    const auto        hr = runtime.LoadPlugin(request, &result);

    ASSERT_TRUE(DAS::IsFailed(hr));
    EXPECT_EQ(hr, DAS_E_INVALID_ARGUMENT);
    EXPECT_EQ(capture.load_count, 0);
}

TEST(NodeRuntimeRemoteHost, LoadDelegatesToRemoteHostWithExplicitLaunchDesc)
{
    TempNodeRuntimeLayout layout;
    layout.CreateRuntimePackageFiles();
    ScopedEnvVar      env{NODE_HOST_EXECUTABLE_ENV, layout.FakeNode().string()};
    RemoteHostCapture capture;

    NodeRuntime runtime{
        std::make_unique<CapturingRemotePluginHost>(capture, 82),
        RuntimeLifecycleCallbacks{}};

    const std::string manifest_u8{
        DAS::Utils::U8AsString(layout.ManifestPath().u8string())};
    const std::string node_modules_u8{
        DAS::Utils::U8AsString(layout.NodeModulesRoot().u8string())};
    RuntimeLoadRequest request{};
    request.manifest_path = manifest_u8.c_str();
    request.runtime_path = manifest_u8.c_str();
    request.node_modules_root = node_modules_u8.c_str();
    request.plugin_guid.data1 = 0x75070003;
    request.main_process_owner_session_id = 1;

    RuntimeLoadResult result{};
    const auto        hr = runtime.LoadPlugin(request, &result);

    ASSERT_TRUE(DAS::IsOk(hr));
    EXPECT_EQ(result.owner_session_id, 82);
    EXPECT_NE(result.object, nullptr);
    if (result.object != nullptr)
    {
        result.object->Release();
    }
    EXPECT_EQ(capture.load_count, 1);
    EXPECT_EQ(capture.manifest_path, layout.ManifestPath());
    EXPECT_EQ(capture.plugin_guid.data1, 0x75070003u);
    EXPECT_EQ(capture.executable, layout.FakeNode().string());
    ASSERT_EQ(capture.args.size(), 7u);
    EXPECT_EQ(
        capture.args.front(),
        (layout.RuntimeRoot() / HOST_SCRIPT_RELATIVE).string());
    EXPECT_EQ(capture.args[1], "--main-pid");
    EXPECT_FALSE(capture.args[2].empty());
    EXPECT_EQ(capture.args[3], "--package-root");
    EXPECT_EQ(capture.args[4], layout.PackageRoot().string());
    EXPECT_EQ(capture.args[5], "--node-modules-root");
    EXPECT_EQ(capture.args[6], layout.NodeModulesRoot().string());
    EXPECT_EQ(capture.working_directory, layout.PackageRoot().string());
    EXPECT_NE(
        capture.environment_config.find(DYNAMIC_LIBRARY_PATH_ENV),
        std::string::npos);
}

TEST(NodeRuntimeRemoteHost, LoadForwardsIpcLifecycleCallbacks)
{
    TempNodeRuntimeLayout layout;
    layout.CreateRuntimePackageFiles();
    ScopedEnvVar      env{NODE_HOST_EXECUTABLE_ENV, layout.FakeNode().string()};
    RemoteHostCapture capture;

    RuntimeLifecycleCallbacks callbacks{};
    callbacks.on_process_exit = [](uint16_t, int) {};
    callbacks.on_heartbeat_timeout = [](DasGuid) {};
    NodeRuntime runtime{
        std::make_unique<CapturingRemotePluginHost>(capture, 82),
        std::move(callbacks)};

    const std::string manifest_u8{
        DAS::Utils::U8AsString(layout.ManifestPath().u8string())};
    const std::string node_modules_u8{
        DAS::Utils::U8AsString(layout.NodeModulesRoot().u8string())};
    RuntimeLoadRequest request{};
    request.manifest_path = manifest_u8.c_str();
    request.runtime_path = manifest_u8.c_str();
    request.node_modules_root = node_modules_u8.c_str();
    request.plugin_guid.data1 = 0x75070004;
    request.main_process_owner_session_id = 1;

    RuntimeLoadResult result{};
    const auto        hr = runtime.LoadPlugin(request, &result);

    ASSERT_TRUE(DAS::IsOk(hr));
    if (result.object != nullptr)
    {
        result.object->Release();
    }
    EXPECT_EQ(capture.load_count, 1);
    EXPECT_TRUE(capture.has_on_process_exit);
    EXPECT_TRUE(capture.has_on_heartbeat_timeout);
}
