#include "../src/CSharpHostFxrBackend.h"
#include "../src/CSharpNetFxBackend.h"

#include <gtest/gtest.h>

#include <das/DasPtr.hpp>
#include <das/DasTypes.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
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
    FakeNetFxState*   g_fake_netfx_state = nullptr;

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

    CSharpHostFxrApi MakeFakeApi(FakeHostFxrState& state)
    {
        state.entrypoint = reinterpret_cast<void*>(&FakeEntrypoint);
        g_fake_state = &state;

        CSharpHostFxrApi api{};
        api.initialize_for_runtime_config =
            [](const std::filesystem::path& runtime_config,
               CSharpHostFxrHandle*         handle,
               void*) -> int
        {
            g_fake_state->init_calls += 1;
            g_fake_state->runtime_config = runtime_config;
            *handle = reinterpret_cast<CSharpHostFxrHandle>(g_fake_state);
            return g_fake_state->init_result;
        };
        api.get_runtime_delegate = [](CSharpHostFxrHandle,
                                      CSharpHostFxrDelegateType type,
                                      void**                    delegate,
                                      void*) -> int
        {
            g_fake_state->delegate_calls += 1;
            if (type
                != CSharpHostFxrDelegateType::LoadAssemblyAndGetFunctionPointer)
            {
                return -1;
            }
            *delegate = g_fake_state;
            return g_fake_state->delegate_result;
        };
        api.close = [](CSharpHostFxrHandle, void*)
        { g_fake_state->close_calls += 1; };
        api.load_assembly_and_get_function_pointer =
            [](void*                        delegate,
               const std::filesystem::path& assembly_path,
               std::string_view             type_name,
               std::string_view             method_name,
               void**                       entrypoint,
               void*) -> int
        {
            auto* state = static_cast<FakeHostFxrState*>(delegate);
            state->assembly_path = assembly_path;
            state->type_name = std::string{type_name};
            state->method_name = std::string{method_name};
            *entrypoint = state->entrypoint;
            return 0;
        };
        return api;
    }

    CSharpNetFxApi MakeFakeApi(FakeNetFxState& state)
    {
        g_fake_netfx_state = &state;

        CSharpNetFxApi api{};
        api.start_clr = [](CSharpNetFxHostHandle* handle, void*) -> int
        {
            g_fake_netfx_state->start_calls += 1;
            *handle =
                reinterpret_cast<CSharpNetFxHostHandle>(g_fake_netfx_state);
            return g_fake_netfx_state->start_result;
        };
        api.execute_in_default_app_domain =
            [](CSharpNetFxHostHandle,
               const std::filesystem::path& assembly_path,
               std::string_view             type_name,
               std::string_view             method_name,
               std::string_view             bootstrap_cookie,
               unsigned long*               return_value,
               void*) -> int
        {
            g_fake_netfx_state->execute_calls += 1;
            g_fake_netfx_state->assembly_path = assembly_path;
            g_fake_netfx_state->type_name = std::string{type_name};
            g_fake_netfx_state->method_name = std::string{method_name};
            g_fake_netfx_state->bootstrap_cookie =
                std::string{bootstrap_cookie};
            *return_value = g_fake_netfx_state->managed_return;

            if (g_fake_netfx_state->set_package)
            {
                auto* package = new BackendPluginPackage();
                package->AddRef();
                auto* bootstrap_args =
                    reinterpret_cast<DasCSharpBootstrapArgsV1*>(
                        std::stoull(g_fake_netfx_state->bootstrap_cookie));
                *bootstrap_args->pp_package = package;
            }

            return g_fake_netfx_state->execute_result;
        };
        api.release = [](CSharpNetFxHostHandle, void*)
        { g_fake_netfx_state->release_calls += 1; };
        return api;
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
    auto                 api = MakeFakeApi(state);
    CSharpHostFxrBackend backend{api};

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
    auto                 api = MakeFakeApi(state);
    CSharpHostFxrBackend backend{api};

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
    auto                 api = MakeFakeApi(state);
    CSharpHostFxrBackend backend{api};

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
    auto             api = MakeFakeApi(state);
    state.entrypoint = nullptr;
    CSharpHostFxrBackend backend{api};

    EXPECT_EQ(
        InvokeBackend(backend, layout.Manifest()),
        DAS_E_CSHARP_ENTRYPOINT_MISSING);
}

TEST(CSharpHostBackend, InvalidManifestEntrypointMapsToEntrypointInvalid)
{
    TempBackendLayout layout;
    layout.WriteRuntimeConfig();
    FakeHostFxrState     state;
    auto                 api = MakeFakeApi(state);
    CSharpHostFxrBackend backend{api};
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
    auto                 api = MakeFakeApi(state);
    CSharpHostFxrBackend backend{api};

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
    auto                 api = MakeFakeApi(state);
    CSharpHostFxrBackend backend{api};

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
    auto                 api = MakeFakeApi(state);
    CSharpHostFxrBackend backend{api};

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
    auto               api = MakeFakeApi(state);
    CSharpNetFxBackend backend{api};

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
    auto               api = MakeFakeApi(state);
    CSharpNetFxBackend backend{api};

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
    auto               api = MakeFakeApi(state);
    CSharpNetFxBackend backend{api};

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
    auto               api = MakeFakeApi(state);
    CSharpNetFxBackend backend{api};

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
    auto               api = MakeFakeApi(state);
    CSharpNetFxBackend backend{api};

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
