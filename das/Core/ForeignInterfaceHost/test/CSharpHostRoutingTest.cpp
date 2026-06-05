#include "../src/CSharpHost.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace DAS::Core::ForeignInterfaceHost;

namespace
{
    std::string ReadSourceFile(const std::filesystem::path& path)
    {
        std::ifstream stream{path};
        std::string   content(
            (std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
        return content;
    }

    std::filesystem::path RepositoryRoot()
    {
        auto path = std::filesystem::path{__FILE__};
        while (!path.empty())
        {
            if (std::filesystem::exists(
                    path / "das" / "Host" / "src" / "main.cpp"))
            {
                return path;
            }
            path = path.parent_path();
        }
        return {};
    }

    class TempCSharpManifestLayout
    {
    public:
        TempCSharpManifestLayout()
        {
            const auto stamp =
                std::chrono::steady_clock::now().time_since_epoch().count();
            root_ = std::filesystem::temp_directory_path()
                    / ("das-csharp-host-routing-" + std::to_string(stamp));
            std::filesystem::create_directories(root_);
        }

        TempCSharpManifestLayout(const TempCSharpManifestLayout&) = delete;
        TempCSharpManifestLayout& operator=(const TempCSharpManifestLayout&) =
            delete;

        ~TempCSharpManifestLayout()
        {
            std::error_code ec;
            std::filesystem::remove_all(root_, ec);
        }

        [[nodiscard]]
        const std::filesystem::path& Root() const
        {
            return root_;
        }

        std::filesystem::path WriteManifest(
            std::string_view target_framework) const
        {
            const auto    manifest_path = root_ / "CSharpRoutingPlugin.json";
            std::ofstream stream{manifest_path};
            stream << R"({
                "guid": "00000000-0000-0000-0000-000000780202",
                "name": "CSharpRoutingPlugin",
                "language": "CSharp",
                "description": "test",
                "author": "test",
                "version": "1.0",
                "supportedSystem": "win",
                "pluginFilenameExtension": "dll",
                "targetFramework": ")"
                   << target_framework << R"(",
                "entryPoint": "Das.TestPlugin.TestPluginEntrypoint.Create",
                "runtimeConfigPath": "CSharpRoutingPlugin.runtimeconfig.json"
            })";
            return manifest_path;
        }

    private:
        std::filesystem::path root_;
    };

    class RoutingPluginPackage final
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

    struct BackendCapture
    {
        int                   calls = 0;
        std::filesystem::path manifest_path;
        std::filesystem::path package_root;
        std::filesystem::path plugin_binary_path;
        std::string           target_framework;
        std::string           entry_type;
        std::string           entry_method;
        std::string           bootstrap_manifest_path;
        std::string           bootstrap_plugin_root;
        std::string           bootstrap_plugin_binary_path;
        uint32_t              bootstrap_size = 0;
        uint32_t              bootstrap_abi_version = 0;
    };

    class CapturingCSharpBackend final : public ICSharpRuntimeBackend
    {
    public:
        explicit CapturingCSharpBackend(
            BackendCapture& capture,
            DasResult       result = DAS_S_OK)
            : capture_{capture}, result_{result}
        {
        }

        DasResult LoadPlugin(
            const CSharpManifest&     manifest,
            DasCSharpBootstrapArgsV1& bootstrap_args) override
        {
            capture_.calls += 1;
            capture_.manifest_path = manifest.manifest_path;
            capture_.package_root = manifest.package_root;
            capture_.plugin_binary_path = manifest.plugin_binary_path;
            capture_.target_framework = manifest.target_framework;
            capture_.entry_type = manifest.entry_point.type_name;
            capture_.entry_method = manifest.entry_point.method_name;
            capture_.bootstrap_size = bootstrap_args.size;
            capture_.bootstrap_abi_version = bootstrap_args.abi_version;
            capture_.bootstrap_manifest_path = bootstrap_args.manifest_path;
            capture_.bootstrap_plugin_root = bootstrap_args.plugin_root;
            capture_.bootstrap_plugin_binary_path =
                bootstrap_args.plugin_binary_path;

            if (DAS::IsFailed(result_))
            {
                return result_;
            }

            auto* package = new RoutingPluginPackage();
            package->AddRef();
            *bootstrap_args.pp_package = package;
            return DAS_S_OK;
        }

    private:
        BackendCapture& capture_;
        DasResult       result_ = DAS_S_OK;
    };
} // namespace

TEST(CSharpHostRouting, FactoryCreatesCSharpRuntimeWhenExportEnabled)
{
    ForeignLanguageRuntimeFactoryDesc desc{};
    desc.language = ForeignInterfaceLanguage::CSharp;

    auto runtime = CreateForeignLanguageRuntime(desc);

#ifdef DAS_EXPORT_CSHARP
    ASSERT_TRUE(runtime);
    EXPECT_NE(runtime.value().Get(), nullptr);
#else
    ASSERT_FALSE(runtime);
    EXPECT_EQ(runtime.error(), DAS_E_NO_IMPLEMENTATION);
#endif
}

TEST(CSharpHostRouting, DasHostSourceContainsExplicitCSharpBranch)
{
    const auto source =
        ReadSourceFile(RepositoryRoot() / "das" / "Host" / "src" / "main.cpp");

    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("lang_lower == \"csharp\""), std::string::npos);
    EXPECT_NE(
        source.find("ForeignInterfaceLanguage::CSharp"),
        std::string::npos);
}

TEST(CSharpHostRouting, LoadPluginDispatchesModernTfmToModernBackend)
{
    TempCSharpManifestLayout layout;
    const auto               manifest_path = layout.WriteManifest("net8.0");
    BackendCapture           modern_capture;
    BackendCapture           netfx_capture;
    CSharpRuntimeBackendSet  backends{};
    backends.modern_dotnet =
        std::make_unique<CapturingCSharpBackend>(modern_capture);
    backends.netfx = std::make_unique<CapturingCSharpBackend>(netfx_capture);
    CSharpHost host{std::move(backends)};

    auto result = host.LoadPlugin(manifest_path);

    ASSERT_TRUE(result);
    EXPECT_NE(result->Get(), nullptr);
    EXPECT_EQ(modern_capture.calls, 1);
    EXPECT_EQ(netfx_capture.calls, 0);
    EXPECT_EQ(modern_capture.manifest_path, manifest_path);
    EXPECT_EQ(modern_capture.package_root, layout.Root());
    EXPECT_EQ(
        modern_capture.plugin_binary_path,
        layout.Root() / "CSharpRoutingPlugin.dll");
    EXPECT_EQ(modern_capture.target_framework, "net8.0");
    EXPECT_EQ(modern_capture.entry_type, "Das.TestPlugin.TestPluginEntrypoint");
    EXPECT_EQ(modern_capture.entry_method, "Create");
    EXPECT_EQ(modern_capture.bootstrap_manifest_path, manifest_path.string());
    EXPECT_EQ(modern_capture.bootstrap_plugin_root, layout.Root().string());
    EXPECT_EQ(
        modern_capture.bootstrap_plugin_binary_path,
        (layout.Root() / "CSharpRoutingPlugin.dll").string());
    EXPECT_EQ(modern_capture.bootstrap_size, DAS_CSHARP_BOOTSTRAP_ARGS_V1_SIZE);
    EXPECT_EQ(
        modern_capture.bootstrap_abi_version,
        DAS_CSHARP_BOOTSTRAP_ABI_VERSION_V1);
}

TEST(CSharpHostRouting, LoadPluginDispatchesNetFxTfmToNetFxBackend)
{
    TempCSharpManifestLayout layout;
    const auto               manifest_path = layout.WriteManifest("net48");
    BackendCapture           modern_capture;
    BackendCapture           netfx_capture;
    CSharpRuntimeBackendSet  backends{};
    backends.modern_dotnet =
        std::make_unique<CapturingCSharpBackend>(modern_capture);
    backends.netfx = std::make_unique<CapturingCSharpBackend>(netfx_capture);
    CSharpHost host{std::move(backends)};

    auto result = host.LoadPlugin(manifest_path);

    ASSERT_TRUE(result);
    EXPECT_EQ(modern_capture.calls, 0);
    EXPECT_EQ(netfx_capture.calls, 1);
    EXPECT_EQ(netfx_capture.target_framework, "net48");
}

TEST(CSharpHostRouting, LoadPluginMapsUnavailableModernBackend)
{
    TempCSharpManifestLayout layout;
    const auto               manifest_path = layout.WriteManifest("net8.0");
    CSharpHost               host;

    auto result = host.LoadPlugin(manifest_path);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), DAS_E_CSHARP_HOSTFXR_INIT_FAILED);
}

TEST(CSharpHostRouting, LoadPluginMapsUnavailableNetFxBackend)
{
    TempCSharpManifestLayout layout;
    const auto               manifest_path = layout.WriteManifest("net48");
    CSharpHost               host;

    auto result = host.LoadPlugin(manifest_path);

    ASSERT_FALSE(result);
#ifdef _WIN32
    EXPECT_EQ(result.error(), DAS_E_CSHARP_COM_CLR_INIT_FAILED);
#else
    EXPECT_EQ(result.error(), DAS_E_CSHARP_NETFX_UNSUPPORTED_PLATFORM);
#endif
}
