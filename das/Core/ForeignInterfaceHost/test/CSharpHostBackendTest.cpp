#include "../src/CSharpHostFxrBackend.h"
#include "../src/CSharpNetFxBackend.h"

#include <gtest/gtest.h>

#include <das/DasPtr.hpp>
#include <das/DasTypes.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

using namespace DAS::Core::ForeignInterfaceHost;

namespace
{
    class BackendPluginPackage final
        : public Das::PluginInterface::IDasPluginPackage
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
            if (iid == DAS_IID_PLUGIN_PACKAGE)
            {
                *pp_out_object =
                    static_cast<Das::PluginInterface::IDasPluginPackage*>(this);
                AddRef();
                return DAS_S_OK;
            }

            *pp_out_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL
        EnumFeature(uint64_t, Das::PluginInterface::DasPluginFeature*) override
        {
            return DAS_S_FALSE;
        }

        DasResult DAS_STD_CALL
        CreateFeatureInterface(uint64_t, IDasBase**) override
        {
            return DAS_E_NOT_FOUND;
        }

        DasResult DAS_STD_CALL CanUnloadNow(bool* can_unload_now) override
        {
            if (can_unload_now == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *can_unload_now = true;
            return DAS_S_OK;
        }

    private:
        std::atomic<uint32_t> ref_count_{0};
    };

    class TempBackendLayout
    {
    public:
        TempBackendLayout()
        {
            const auto stamp =
                std::chrono::steady_clock::now().time_since_epoch().count();
            root_ = std::filesystem::temp_directory_path()
                    / ("das-csharp-backend-" + std::to_string(stamp));
            std::filesystem::create_directories(root_);
            WriteFile(root_ / "DasCSharpTestPlugin.dll", "");
        }

        TempBackendLayout(const TempBackendLayout&) = delete;
        TempBackendLayout& operator=(const TempBackendLayout&) = delete;

        ~TempBackendLayout()
        {
            std::error_code ec;
            std::filesystem::remove_all(root_, ec);
        }

        [[nodiscard]]
        CSharpManifest Manifest() const
        {
            CSharpManifest manifest{};
            manifest.manifest_path = root_ / "DasCSharpTestPlugin.json";
            manifest.package_root = root_;
            manifest.plugin_binary_path = root_ / "DasCSharpTestPlugin.dll";
            manifest.name = "DasCSharpTestPlugin";
            manifest.plugin_filename_extension = "dll";
            manifest.target_framework = "net8.0";
            manifest.target_framework_family =
                CSharpTargetFrameworkFamily::ModernDotNet;
            manifest.entry_point = {
                .type_name = "Das.TestPlugin.TestPluginEntrypoint",
                .method_name = "Create",
            };
            manifest.runtime_config_path =
                root_ / "DasCSharpTestPlugin.runtimeconfig.json";
            return manifest;
        }

        void WriteRuntimeConfig() const
        {
            WriteFile(
                root_ / "DasCSharpTestPlugin.runtimeconfig.json",
                R"({"runtimeOptions":{"tfm":"net8.0","framework":{"name":"Microsoft.NETCore.App","version":"8.0.0"}}})");
        }

    private:
        static void WriteFile(
            const std::filesystem::path& path,
            std::string_view             content)
        {
            std::ofstream stream{path};
            stream << content;
        }

        std::filesystem::path root_;
    };

    struct FakeHostFxrState
    {
        int                   init_result = 0;
        int                   delegate_result = 0;
        int                   entrypoint_lookup_result = 0;
        void*                 entrypoint = nullptr;
        int                   entrypoint_result = DAS_S_OK;
        bool                  set_package = true;
        int                   init_calls = 0;
        int                   delegate_calls = 0;
        int                   close_calls = 0;
        int                   entrypoint_calls = 0;
        std::filesystem::path runtime_config;
        std::filesystem::path assembly_path;
        std::string           type_name;
        std::string           method_name;
        int                   args_size = 0;
    };

    struct FakeNetFxState
    {
        int                   start_result = 0;
        int                   execute_result = 0;
        unsigned long         managed_return = DAS_S_OK;
        bool                  set_package = true;
        int                   start_calls = 0;
        int                   execute_calls = 0;
        int                   release_calls = 0;
        std::filesystem::path assembly_path;
        std::string           type_name;
        std::string           method_name;
        std::string           bootstrap_cookie;
    };

    FakeHostFxrState* g_fake_state = nullptr;

    int FakeEntrypoint(void* args, int size_bytes)
    {
        g_fake_state->entrypoint_calls += 1;
        g_fake_state->args_size = size_bytes;

        if (g_fake_state->set_package)
        {
            auto* package = new BackendPluginPackage();
            package->AddRef();
            auto* bootstrap_args = static_cast<DasCSharpBootstrapArgsV1*>(args);
            *bootstrap_args->pp_package = package;
        }

        return g_fake_state->entrypoint_result;
    }

    class FakeHostFxrAssemblyResolver final
        : public ICSharpHostFxrAssemblyResolver
    {
    public:
        explicit FakeHostFxrAssemblyResolver(FakeHostFxrState& state)
            : state_{state}
        {
        }

        int LoadAssemblyAndGetFunctionPointer(
            const std::filesystem::path& assembly_path,
            std::string_view             type_name,
            std::string_view             method_name,
            void**                       entrypoint) override
        {
            state_.assembly_path = assembly_path;
            state_.type_name = std::string{type_name};
            state_.method_name = std::string{method_name};
            if (entrypoint != nullptr)
            {
                *entrypoint = state_.entrypoint;
            }
            return state_.entrypoint_lookup_result;
        }

    private:
        FakeHostFxrState& state_;
    };

    class FakeHostFxrRuntime final : public ICSharpHostFxrRuntime
    {
    public:
        explicit FakeHostFxrRuntime(FakeHostFxrState& state) : state_{state} {}

        ~FakeHostFxrRuntime() override { state_.close_calls += 1; }

        int GetAssemblyResolver(
            std::unique_ptr<ICSharpHostFxrAssemblyResolver>* resolver) override
        {
            if (resolver == nullptr)
            {
                return -1;
            }

            state_.delegate_calls += 1;
            *resolver = nullptr;
            if (state_.delegate_result != 0)
            {
                return state_.delegate_result;
            }

            *resolver = std::make_unique<FakeHostFxrAssemblyResolver>(state_);
            return 0;
        }

    private:
        FakeHostFxrState& state_;
    };

    class FakeHostFxrRuntimeLoader final : public ICSharpHostFxrRuntimeLoader
    {
    public:
        explicit FakeHostFxrRuntimeLoader(FakeHostFxrState& state)
            : state_{state}
        {
            state_.entrypoint = reinterpret_cast<void*>(&FakeEntrypoint);
            g_fake_state = &state_;
        }

        int InitializeForRuntimeConfig(
            const std::filesystem::path&            runtime_config_path,
            std::unique_ptr<ICSharpHostFxrRuntime>* runtime) override
        {
            if (runtime == nullptr)
            {
                return -1;
            }

            state_.init_calls += 1;
            state_.runtime_config = runtime_config_path;
            *runtime = nullptr;
            if (state_.init_result < 0)
            {
                return state_.init_result;
            }

            *runtime = std::make_unique<FakeHostFxrRuntime>(state_);
            return state_.init_result;
        }

    private:
        FakeHostFxrState& state_;
    };

    std::shared_ptr<ICSharpHostFxrRuntimeLoader> MakeFakeHostFxrLoader(
        FakeHostFxrState& state)
    {
        return std::make_shared<FakeHostFxrRuntimeLoader>(state);
    }

    class FakeNetFxRuntimeHost final : public ICSharpNetFxRuntimeHost
    {
    public:
        explicit FakeNetFxRuntimeHost(FakeNetFxState& state) : state_{state} {}

        ~FakeNetFxRuntimeHost() override { state_.release_calls += 1; }

        int ExecuteInDefaultAppDomain(
            const std::filesystem::path& assembly_path,
            std::string_view             type_name,
            std::string_view             method_name,
            std::string_view             bootstrap_cookie,
            unsigned long*               return_value) override
        {
            if (return_value == nullptr)
            {
                return -1;
            }

            state_.execute_calls += 1;
            state_.assembly_path = assembly_path;
            state_.type_name = std::string{type_name};
            state_.method_name = std::string{method_name};
            state_.bootstrap_cookie = std::string{bootstrap_cookie};
            *return_value = state_.managed_return;

            if (state_.set_package)
            {
                auto* package = new BackendPluginPackage();
                package->AddRef();
                auto* bootstrap_args =
                    reinterpret_cast<DasCSharpBootstrapArgsV1*>(
                        std::stoull(state_.bootstrap_cookie));
                *bootstrap_args->pp_package = package;
            }

            return state_.execute_result;
        }

    private:
        FakeNetFxState& state_;
    };

    class FakeNetFxRuntimeLoader final : public ICSharpNetFxRuntimeLoader
    {
    public:
        explicit FakeNetFxRuntimeLoader(FakeNetFxState& state) : state_{state}
        {
        }

        int StartRuntime(
            std::unique_ptr<ICSharpNetFxRuntimeHost>* runtime_host) override
        {
            if (runtime_host == nullptr)
            {
                return -1;
            }

            state_.start_calls += 1;
            *runtime_host = nullptr;
            if (state_.start_result < 0)
            {
                return state_.start_result;
            }

            *runtime_host = std::make_unique<FakeNetFxRuntimeHost>(state_);
            return state_.start_result;
        }

    private:
        FakeNetFxState& state_;
    };

    std::shared_ptr<ICSharpNetFxRuntimeLoader> MakeFakeNetFxLoader(
        FakeNetFxState& state)
    {
        return std::make_shared<FakeNetFxRuntimeLoader>(state);
    }

    DasResult InvokeBackend(
        CSharpHostFxrBackend& backend,
        const CSharpManifest& manifest)
    {
        Das::PluginInterface::IDasPluginPackage* package = nullptr;
        const auto manifest_path_storage = manifest.manifest_path.string();
        const auto plugin_root_storage = manifest.package_root.string();
        const auto plugin_binary_path_storage =
            manifest.plugin_binary_path.string();
        DasCSharpBootstrapArgsV1 args{
            .size = DAS_CSHARP_BOOTSTRAP_ARGS_V1_SIZE,
            .abi_version = DAS_CSHARP_BOOTSTRAP_ABI_VERSION_V1,
            .manifest_path = manifest_path_storage.c_str(),
            .plugin_root = plugin_root_storage.c_str(),
            .plugin_binary_path = plugin_binary_path_storage.c_str(),
            .host_api = nullptr,
            .pp_package = &package,
        };

        const auto result = backend.LoadPlugin(manifest, args);
        if (package != nullptr)
        {
            package->Release();
        }
        return result;
    }

    DasResult InvokeBackend(
        CSharpNetFxBackend&   backend,
        const CSharpManifest& manifest)
    {
        Das::PluginInterface::IDasPluginPackage* package = nullptr;
        const auto manifest_path_storage = manifest.manifest_path.string();
        const auto plugin_root_storage = manifest.package_root.string();
        const auto plugin_binary_path_storage =
            manifest.plugin_binary_path.string();
        DasCSharpBootstrapArgsV1 args{
            .size = DAS_CSHARP_BOOTSTRAP_ARGS_V1_SIZE,
            .abi_version = DAS_CSHARP_BOOTSTRAP_ABI_VERSION_V1,
            .manifest_path = manifest_path_storage.c_str(),
            .plugin_root = plugin_root_storage.c_str(),
            .plugin_binary_path = plugin_binary_path_storage.c_str(),
            .host_api = nullptr,
            .pp_package = &package,
        };

        const auto result = backend.LoadPlugin(manifest, args);
        if (package != nullptr)
        {
            package->Release();
        }
        return result;
    }
} // namespace

TEST(CSharpHostBackend, MissingRuntimeConfigMapsToSpecificError)
{
    TempBackendLayout    layout;
    FakeHostFxrState     state;
    auto                 loader = MakeFakeHostFxrLoader(state);
    CSharpHostFxrBackend backend{loader};

    EXPECT_EQ(
        InvokeBackend(backend, layout.Manifest()),
        DAS_E_CSHARP_MISSING_RUNTIMECONFIG);
    EXPECT_EQ(state.init_calls, 0);
}

TEST(CSharpHostBackend, HostFxrInitFailureMapsToHostFxrInitFailed)
{
    TempBackendLayout layout;
    layout.WriteRuntimeConfig();
    FakeHostFxrState state;
    state.init_result = -1;
    auto                 loader = MakeFakeHostFxrLoader(state);
    CSharpHostFxrBackend backend{loader};

    EXPECT_EQ(
        InvokeBackend(backend, layout.Manifest()),
        DAS_E_CSHARP_HOSTFXR_INIT_FAILED);
    EXPECT_EQ(state.init_calls, 1);
    EXPECT_EQ(state.delegate_calls, 0);
}

TEST(CSharpHostBackend, HostFxrDelegateFailureMapsToHostFxrInitFailed)
{
    TempBackendLayout layout;
    layout.WriteRuntimeConfig();
    FakeHostFxrState state;
    state.delegate_result = -1;
    auto                 loader = MakeFakeHostFxrLoader(state);
    CSharpHostFxrBackend backend{loader};

    EXPECT_EQ(
        InvokeBackend(backend, layout.Manifest()),
        DAS_E_CSHARP_HOSTFXR_INIT_FAILED);
    EXPECT_EQ(state.close_calls, 1);
}

TEST(CSharpHostBackend, MissingEntrypointPointerMapsToEntrypointMissing)
{
    TempBackendLayout layout;
    layout.WriteRuntimeConfig();
    FakeHostFxrState state;
    auto             loader = MakeFakeHostFxrLoader(state);
    state.entrypoint = nullptr;
    CSharpHostFxrBackend backend{loader};

    EXPECT_EQ(
        InvokeBackend(backend, layout.Manifest()),
        DAS_E_CSHARP_ENTRYPOINT_MISSING);
}

TEST(CSharpHostBackend, EntrypointLookupFailureMapsToEntrypointMissing)
{
    TempBackendLayout layout;
    layout.WriteRuntimeConfig();
    FakeHostFxrState state;
    state.entrypoint_lookup_result = -1;
    auto                 loader = MakeFakeHostFxrLoader(state);
    CSharpHostFxrBackend backend{loader};

    EXPECT_EQ(
        InvokeBackend(backend, layout.Manifest()),
        DAS_E_CSHARP_ENTRYPOINT_MISSING);
}

TEST(CSharpHostBackend, InvalidManifestEntrypointMapsToEntrypointInvalid)
{
    TempBackendLayout layout;
    layout.WriteRuntimeConfig();
    FakeHostFxrState     state;
    auto                 loader = MakeFakeHostFxrLoader(state);
    CSharpHostFxrBackend backend{loader};
    auto                 manifest = layout.Manifest();
    manifest.entry_point = {};

    EXPECT_EQ(
        InvokeBackend(backend, manifest),
        DAS_E_CSHARP_ENTRYPOINT_INVALID);
}

TEST(CSharpHostBackend, EntrypointFailureMapsToPluginInitFailed)
{
    TempBackendLayout layout;
    layout.WriteRuntimeConfig();
    FakeHostFxrState state;
    state.entrypoint_result = DAS_E_CSHARP_ERROR;
    auto                 loader = MakeFakeHostFxrLoader(state);
    CSharpHostFxrBackend backend{loader};

    EXPECT_EQ(
        InvokeBackend(backend, layout.Manifest()),
        DAS_E_CSHARP_PLUGIN_INIT_FAILED);
}

TEST(CSharpHostBackend, NullPackageOutMapsToPluginInitFailed)
{
    TempBackendLayout layout;
    layout.WriteRuntimeConfig();
    FakeHostFxrState state;
    state.set_package = false;
    auto                 loader = MakeFakeHostFxrLoader(state);
    CSharpHostFxrBackend backend{loader};

    EXPECT_EQ(
        InvokeBackend(backend, layout.Manifest()),
        DAS_E_CSHARP_PLUGIN_INIT_FAILED);
}

TEST(
    CSharpHostBackend,
    ModernEntrypointUsesAssemblyQualifiedTypeAndDefaultSignature)
{
    TempBackendLayout layout;
    layout.WriteRuntimeConfig();
    FakeHostFxrState     state;
    auto                 loader = MakeFakeHostFxrLoader(state);
    CSharpHostFxrBackend backend{loader};

    EXPECT_EQ(InvokeBackend(backend, layout.Manifest()), DAS_S_OK);
    EXPECT_EQ(
        state.assembly_path,
        layout.Manifest().package_root / "DasCSharpTestPlugin.dll");
    EXPECT_EQ(
        state.type_name,
        "Das.TestPlugin.TestPluginEntrypoint, DasCSharpTestPlugin");
    EXPECT_EQ(state.method_name, "Create");
    EXPECT_EQ(state.entrypoint_calls, 1);
    EXPECT_EQ(state.args_size, DAS_CSHARP_BOOTSTRAP_ARGS_V1_SIZE);
}

TEST(CSharpHostBackend, FindDotNetDefinesNativeHostingDiscoveryContract)
{
    const auto    source_path = std::filesystem::path{__FILE__}
                                    .parent_path()
                                    .parent_path()
                                    .parent_path()
                                    .parent_path()
                                    .parent_path()
                                / "cmake" / "FindDotNet.cmake";
    std::ifstream source{source_path};
    ASSERT_TRUE(source.is_open());
    const std::string content(
        (std::istreambuf_iterator<char>(source)),
        std::istreambuf_iterator<char>());

    EXPECT_NE(content.find("DotNet_NATIVE_HOSTING_FOUND"), std::string::npos);
    EXPECT_NE(content.find("DotNet::nethost"), std::string::npos);
    EXPECT_NE(content.find("find_path"), std::string::npos);
    EXPECT_NE(content.find("find_library"), std::string::npos);
    EXPECT_NE(content.find("FindPackageHandleStandardArgs"), std::string::npos);
    EXPECT_EQ(content.find("Program Files"), std::string::npos);
    EXPECT_EQ(
        content.find("Microsoft.NETCore.App.Host.win-x64"),
        std::string::npos);
}

TEST(CSharpHostBackendNetFx, WindowsSeamUsesDefaultAppDomainStringContract)
{
    TempBackendLayout layout;
    auto              manifest = layout.Manifest();
    manifest.target_framework = "net48";
    manifest.target_framework_family = CSharpTargetFrameworkFamily::NetFx;
    manifest.runtime_config_path = std::nullopt;

    FakeNetFxState     state;
    auto               loader = MakeFakeNetFxLoader(state);
    CSharpNetFxBackend backend{loader};

    EXPECT_EQ(InvokeBackend(backend, manifest), DAS_S_OK);
    EXPECT_EQ(state.start_calls, 1);
    EXPECT_EQ(state.execute_calls, 1);
    EXPECT_EQ(state.release_calls, 1);
    EXPECT_EQ(
        state.assembly_path,
        layout.Manifest().package_root / "DasCSharpTestPlugin.dll");
    EXPECT_EQ(state.type_name, "Das.TestPlugin.TestPluginEntrypoint");
    EXPECT_EQ(state.method_name, "Create");
    ASSERT_FALSE(state.bootstrap_cookie.empty());
}

TEST(CSharpHostBackendNetFx, ClrInitFailureMapsToClrInitFailed)
{
    TempBackendLayout layout;
    auto              manifest = layout.Manifest();
    manifest.target_framework = "net48";
    manifest.target_framework_family = CSharpTargetFrameworkFamily::NetFx;
    manifest.runtime_config_path = std::nullopt;

    FakeNetFxState state;
    state.start_result = -1;
    auto               loader = MakeFakeNetFxLoader(state);
    CSharpNetFxBackend backend{loader};

    EXPECT_EQ(
        InvokeBackend(backend, manifest),
        DAS_E_CSHARP_COM_CLR_INIT_FAILED);
    EXPECT_EQ(state.execute_calls, 0);
}

TEST(CSharpHostBackendNetFx, EntrypointFailureMapsToEntrypointMissing)
{
    TempBackendLayout layout;
    auto              manifest = layout.Manifest();
    manifest.target_framework = "net48";
    manifest.target_framework_family = CSharpTargetFrameworkFamily::NetFx;
    manifest.runtime_config_path = std::nullopt;

    FakeNetFxState state;
    state.execute_result = -1;
    auto               loader = MakeFakeNetFxLoader(state);
    CSharpNetFxBackend backend{loader};

    EXPECT_EQ(
        InvokeBackend(backend, manifest),
        DAS_E_CSHARP_ENTRYPOINT_MISSING);
}

TEST(CSharpHostBackendNetFx, ManagedFailureMapsToPluginInitFailed)
{
    TempBackendLayout layout;
    auto              manifest = layout.Manifest();
    manifest.target_framework = "net48";
    manifest.target_framework_family = CSharpTargetFrameworkFamily::NetFx;
    manifest.runtime_config_path = std::nullopt;

    FakeNetFxState state;
    state.managed_return = DAS_E_CSHARP_ERROR;
    auto               loader = MakeFakeNetFxLoader(state);
    CSharpNetFxBackend backend{loader};

    EXPECT_EQ(
        InvokeBackend(backend, manifest),
        DAS_E_CSHARP_PLUGIN_INIT_FAILED);
}

TEST(CSharpHostBackendNetFx, NullPackageOutMapsToPluginInitFailed)
{
    TempBackendLayout layout;
    auto              manifest = layout.Manifest();
    manifest.target_framework = "net48";
    manifest.target_framework_family = CSharpTargetFrameworkFamily::NetFx;
    manifest.runtime_config_path = std::nullopt;

    FakeNetFxState state;
    state.set_package = false;
    auto               loader = MakeFakeNetFxLoader(state);
    CSharpNetFxBackend backend{loader};

    EXPECT_EQ(
        InvokeBackend(backend, manifest),
        DAS_E_CSHARP_PLUGIN_INIT_FAILED);
}

TEST(CSharpHostBackendNetFx, DefaultBackendIsDeterministicWithoutClrSeam)
{
    TempBackendLayout layout;
    auto              manifest = layout.Manifest();
    manifest.target_framework = "net48";
    manifest.target_framework_family = CSharpTargetFrameworkFamily::NetFx;
    manifest.runtime_config_path = std::nullopt;

    CSharpNetFxBackend backend;

#ifdef _WIN32
    EXPECT_EQ(
        InvokeBackend(backend, manifest),
        DAS_E_CSHARP_COM_CLR_INIT_FAILED);
#else
    EXPECT_EQ(
        InvokeBackend(backend, manifest),
        DAS_E_CSHARP_NETFX_UNSUPPORTED_PLATFORM);
#endif
}

TEST(CSharpHostBackendNetFx, FindDotNetFrameworkDefinesNet48GatingContract)
{
    const auto    source_path = std::filesystem::path{__FILE__}
                                    .parent_path()
                                    .parent_path()
                                    .parent_path()
                                    .parent_path()
                                    .parent_path()
                                / "cmake" / "FindDotNetFramework.cmake";
    std::ifstream source{source_path};
    ASSERT_TRUE(source.is_open());
    const std::string content(
        (std::istreambuf_iterator<char>(source)),
        std::istreambuf_iterator<char>());

    EXPECT_NE(
        content.find("DotNetFramework_NET48_REFERENCE_DIR"),
        std::string::npos);
    EXPECT_NE(
        content.find("DotNetFramework_CSC_EXECUTABLE"),
        std::string::npos);
    EXPECT_NE(
        content.find("DotNetFramework_CLR_HOSTING_FOUND"),
        std::string::npos);
    EXPECT_NE(content.find("FindPackageHandleStandardArgs"), std::string::npos);
    EXPECT_EQ(content.find("Program Files"), std::string::npos);
    EXPECT_EQ(content.find("C:/"), std::string::npos);
    EXPECT_EQ(content.find("C:\\"), std::string::npos);
}

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream source{path};
    if (!source.is_open())
    {
        return {};
    }

    return {
        std::istreambuf_iterator<char>{source},
        std::istreambuf_iterator<char>{},
    };
}

TEST(
    CSharpHostBackend,
    BackendShims_use_behavior_interfaces_not_function_pointer_structs)
{
    const auto root = std::filesystem::path{__FILE__}
                          .parent_path()
                          .parent_path()
                          .parent_path()
                          .parent_path()
                          .parent_path();
    const auto host_fxr_header_path = root / "das" / "Core"
                                      / "ForeignInterfaceHost" / "src"
                                      / "CSharpHostFxrBackend.h";
    const auto net_fx_header_path = root / "das" / "Core"
                                    / "ForeignInterfaceHost" / "src"
                                    / "CSharpNetFxBackend.h";

    const auto host_fxr_header = ReadTextFile(host_fxr_header_path);
    const auto net_fx_header = ReadTextFile(net_fx_header_path);

    ASSERT_FALSE(host_fxr_header.empty());
    ASSERT_FALSE(net_fx_header.empty());

    EXPECT_EQ(
        host_fxr_header.find("struct CSharpHostFxrApi"),
        std::string::npos);
    EXPECT_EQ(net_fx_header.find("struct CSharpNetFxApi"), std::string::npos);
    EXPECT_NE(
        host_fxr_header.find("ICSharpHostFxrRuntimeLoader"),
        std::string::npos);
    EXPECT_NE(host_fxr_header.find("ICSharpHostFxrRuntime"), std::string::npos);
    EXPECT_NE(
        host_fxr_header.find("ICSharpHostFxrAssemblyResolver"),
        std::string::npos);
    EXPECT_NE(net_fx_header.find("ICSharpNetFxRuntimeHost"), std::string::npos);
    EXPECT_NE(
        net_fx_header.find("ICSharpNetFxRuntimeLoader"),
        std::string::npos);
    EXPECT_EQ(host_fxr_header.find("(*)"), std::string::npos);
    EXPECT_EQ(net_fx_header.find("(*)"), std::string::npos);
}
