#include <das/Core/ForeignInterfaceHost/NodeRuntime.h>

#include <das/Core/IPC/MainProcess/IHostLauncher.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <stdlib.h>
#endif

using namespace DAS::Core::ForeignInterfaceHost;

namespace
{
    constexpr auto kEnvName = "DAS_NODE_HOST_EXE_PATH";

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

    class TempNodePackage
    {
    public:
        TempNodePackage()
        {
            const auto stamp =
                std::chrono::steady_clock::now().time_since_epoch().count();
            root_ = std::filesystem::temp_directory_path()
                  / ("das-node-runtime-test-" + std::to_string(stamp));
            std::filesystem::create_directories(root_);
        }

        TempNodePackage(const TempNodePackage&) = delete;
        TempNodePackage& operator=(const TempNodePackage&) = delete;

        ~TempNodePackage()
        {
            std::error_code ec;
            std::filesystem::remove_all(root_, ec);
        }

        [[nodiscard]]
        const std::filesystem::path& Root() const
        {
            return root_;
        }

        std::filesystem::path CreateFile(std::string_view name)
        {
            const auto path = root_ / std::filesystem::path{name};
            std::ofstream stream{path};
            stream << "\n";
            return path;
        }

        std::filesystem::path CreatePackageFiles()
        {
            auto executable = CreateFile("fake-node.exe");
            CreateFile("das-node-host.cjs");
            CreateFile("das_core_napi_export.js");
            CreateFile("das_core_napi.node");
            return executable;
        }

    private:
        std::filesystem::path root_;
    };

    std::string ReadUtf8(IDasReadOnlyString* value)
    {
        const char* raw = nullptr;
        EXPECT_NE(value, nullptr);
        EXPECT_EQ(value->GetUtf8(&raw), DAS_S_OK);
        return raw ? std::string{raw} : std::string{};
    }

    std::string Lower(std::string value)
    {
        std::ranges::transform(
            value,
            value.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }
} // namespace

TEST(NodeRuntimeResolver, UnsetEnvironmentUsesBareNodeLookup)
{
    ScopedEnvVar env{kEnvName, std::nullopt};

    auto resolved = NodeRuntime::ResolveNodeExecutable();

    ASSERT_TRUE(resolved);
    EXPECT_TRUE(std::filesystem::exists(resolved.value()));
    const auto filename = Lower(resolved.value().filename().string());
    EXPECT_TRUE(filename == "node" || filename == "node.exe") << filename;
}

TEST(NodeRuntimeResolver, EmptyEnvironmentUsesBareNodeLookup)
{
    ScopedEnvVar env{kEnvName, std::string{}};

    auto resolved = NodeRuntime::ResolveNodeExecutable();

    ASSERT_TRUE(resolved);
    EXPECT_TRUE(std::filesystem::exists(resolved.value()));
}

TEST(NodeRuntimeResolver, StrictEnvironmentExecutableHasNoFallback)
{
    TempNodePackage package;
    package.CreatePackageFiles();
    ScopedEnvVar env{kEnvName, (package.Root() / "missing-node.exe").string()};

    NodeRuntime runtime{package.Root()};
    auto        launch = runtime.BuildHostLaunchDesc();

    ASSERT_FALSE(launch);
    EXPECT_EQ(launch.error(), DAS_E_FILE_NOT_FOUND);
}

TEST(NodeRuntimeResolver, StrictEnvironmentExecutableIsUsedWhenPresent)
{
    TempNodePackage package;
    const auto      executable = package.CreatePackageFiles();
    ScopedEnvVar    env{kEnvName, executable.string()};

    NodeRuntime runtime{package.Root()};
    auto        launch = runtime.BuildHostLaunchDesc();

    ASSERT_TRUE(launch);
    EXPECT_EQ(
        ReadUtf8(launch->launch_desc.p_executable_path),
        executable.string());
}

TEST(NodeRuntimePackage, ResolvesPinnedPackageRelativePaths)
{
    TempNodePackage package;

    const auto paths = NodeRuntime::ResolvePackagePaths(package.Root());

    EXPECT_EQ(paths.host_script_path, package.Root() / "das-node-host.cjs");
    EXPECT_EQ(paths.wrapper_path, package.Root() / "das_core_napi_export.js");
    EXPECT_EQ(paths.addon_path, package.Root() / "das_core_napi.node");
    EXPECT_EQ(paths.host_script_relative, "./das-node-host.cjs");
    EXPECT_EQ(paths.wrapper_relative, "./das_core_napi_export.js");
    EXPECT_EQ(paths.addon_relative, "./das_core_napi.node");
}

TEST(NodeRuntimePackage, MissingHostScriptFailsDeterministically)
{
    TempNodePackage package;
    const auto      executable = package.CreateFile("fake-node.exe");
    package.CreateFile("das_core_napi_export.js");
    package.CreateFile("das_core_napi.node");
    ScopedEnvVar env{kEnvName, executable.string()};

    NodeRuntime runtime{package.Root()};
    auto        launch = runtime.BuildHostLaunchDesc();

    ASSERT_FALSE(launch);
    EXPECT_EQ(launch.error(), DAS_E_FILE_NOT_FOUND);
}

TEST(NodeRuntimePackage, MissingWrapperFailsDeterministically)
{
    TempNodePackage package;
    const auto      executable = package.CreateFile("fake-node.exe");
    package.CreateFile("das-node-host.cjs");
    package.CreateFile("das_core_napi.node");
    ScopedEnvVar env{kEnvName, executable.string()};

    NodeRuntime runtime{package.Root()};
    auto        launch = runtime.BuildHostLaunchDesc();

    ASSERT_FALSE(launch);
    EXPECT_EQ(launch.error(), DAS_E_FILE_NOT_FOUND);
}

TEST(NodeRuntimePackage, MissingAddonFailsDeterministically)
{
    TempNodePackage package;
    const auto      executable = package.CreateFile("fake-node.exe");
    package.CreateFile("das-node-host.cjs");
    package.CreateFile("das_core_napi_export.js");
    ScopedEnvVar env{kEnvName, executable.string()};

    NodeRuntime runtime{package.Root()};
    auto        launch = runtime.BuildHostLaunchDesc();

    ASSERT_FALSE(launch);
    EXPECT_EQ(launch.error(), DAS_E_FILE_NOT_FOUND);
}

TEST(NodeRuntimePackage, BuildHostLaunchDescUsesPackageLocalScriptAndHostArgs)
{
    TempNodePackage package;
    const auto      executable = package.CreatePackageFiles();
    ScopedEnvVar    env{kEnvName, executable.string()};

    NodeRuntime runtime{package.Root()};
    auto        launch = runtime.BuildHostLaunchDesc();

    ASSERT_TRUE(launch);
    const auto& desc = launch->launch_desc;
    EXPECT_EQ(ReadUtf8(desc.p_executable_path), executable.string());
    ASSERT_NE(desc.pp_args, nullptr);
    ASSERT_EQ(desc.arg_count, 3u);
    EXPECT_EQ(
        ReadUtf8(desc.pp_args[0]),
        (package.Root() / "das-node-host.cjs").string());
    EXPECT_EQ(ReadUtf8(desc.pp_args[1]), "--main-pid");
    EXPECT_FALSE(ReadUtf8(desc.pp_args[2]).empty());
    EXPECT_EQ(ReadUtf8(desc.p_working_directory), package.Root().string());
}
