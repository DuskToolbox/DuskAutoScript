#ifdef DAS_EXPORT_CSHARP

#include "CSharpHost.h"
#include "CSharpHostFxrBackend.h"
#include "CSharpNetFxBackend.h"

#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>

#include <fstream>
#include <iterator>
#include <string>
#include <tl/expected.hpp>
#include <utility>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace
{
    DasResult InvokeBackend(
        ICSharpRuntimeBackend*    backend,
        const CSharpManifest&     manifest,
        DasCSharpBootstrapArgsV1& bootstrap_args)
    {
        if (backend == nullptr)
        {
            return CSharpHost::BackendUnavailableError(
                manifest.target_framework_family);
        }

        return backend->LoadPlugin(manifest, bootstrap_args);
    }
} // namespace

CSharpHost::CSharpHost()
{
#ifdef DAS_CSHARP_HOSTFXR_BACKEND
    backends_.modern_dotnet = std::make_unique<CSharpHostFxrBackend>();
#endif
    backends_.netfx = std::make_unique<CSharpNetFxBackend>();
}

CSharpHost::CSharpHost(CSharpRuntimeBackendSet backends)
    : backends_{std::move(backends)}
{
}

auto CSharpHost::CreateForeignLanguageRuntime(
    const ForeignLanguageRuntimeFactoryDesc& desc)
    -> DAS::Utils::Expected<DasPtr<IForeignLanguageRuntime>>
{
    if (desc.language != ForeignInterfaceLanguage::CSharp)
    {
        return tl::make_unexpected(DAS_E_INVALID_ARGUMENT);
    }

    return Das::MakeDasPtr<IForeignLanguageRuntime, CSharpHost>();
}

DasResult CSharpHost::QueryInterface(const DasGuid& iid, void** pp_out_object)
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

auto CSharpHost::LoadPlugin(const std::filesystem::path& manifest_path)
    -> DAS::Utils::Expected<DasPtr<IDasBase>>
{
    std::ifstream manifest_file{manifest_path};
    if (!manifest_file.is_open())
    {
        DAS_CORE_LOG_ERROR(
            "Failed to open C# plugin manifest: {}",
            manifest_path.string());
        return tl::make_unexpected(DAS_E_FILE_NOT_FOUND);
    }

    const std::string manifest_json(
        (std::istreambuf_iterator<char>(manifest_file)),
        std::istreambuf_iterator<char>());

    auto manifest = ParseCSharpManifest(manifest_path, manifest_json);
    if (!manifest)
    {
        return tl::make_unexpected(manifest.error());
    }

    const auto manifest_path_storage = manifest->manifest_path.string();
    const auto plugin_root_storage = manifest->package_root.string();
    const auto plugin_binary_path_storage =
        manifest->plugin_binary_path.string();

    DasPtr<Das::PluginInterface::IDasPluginPackage> package;
    DasCSharpBootstrapArgsV1                        bootstrap_args{
        .size = DAS_CSHARP_BOOTSTRAP_ARGS_V1_SIZE,
        .abi_version = DAS_CSHARP_BOOTSTRAP_ABI_VERSION_V1,
        .manifest_path = manifest_path_storage.c_str(),
        .plugin_root = plugin_root_storage.c_str(),
        .plugin_binary_path = plugin_binary_path_storage.c_str(),
        .host_api = nullptr,
        .pp_package = package.Put(),
    };

    const auto args_result = ValidateDasCSharpBootstrapArgsV1(&bootstrap_args);
    if (DAS::IsFailed(args_result))
    {
        return tl::make_unexpected(args_result);
    }

    ICSharpRuntimeBackend* backend = nullptr;
    switch (manifest->target_framework_family)
    {
    case CSharpTargetFrameworkFamily::ModernDotNet:
        backend = backends_.modern_dotnet.get();
        break;
    case CSharpTargetFrameworkFamily::NetFx:
        backend = backends_.netfx.get();
        break;
    default:
        return tl::make_unexpected(DAS_E_CSHARP_UNSUPPORTED_TFM);
    }

    const auto backend_result =
        InvokeBackend(backend, manifest.value(), bootstrap_args);
    if (DAS::IsFailed(backend_result))
    {
        return tl::make_unexpected(backend_result);
    }

    if (!package)
    {
        return tl::make_unexpected(DAS_E_CSHARP_DIRECTOR_FACTORY_FAILED);
    }

    DasPtr<IDasBase> result;
    const auto       qi_result = package.As(result);
    if (DAS::IsFailed(qi_result))
    {
        return tl::make_unexpected(qi_result);
    }

    return result;
}

DasResult CSharpHost::BackendUnavailableError(
    CSharpTargetFrameworkFamily family) noexcept
{
    switch (family)
    {
    case CSharpTargetFrameworkFamily::ModernDotNet:
        return DAS_E_CSHARP_HOSTFXR_INIT_FAILED;
    case CSharpTargetFrameworkFamily::NetFx:
#ifdef _WIN32
        return DAS_E_CSHARP_COM_CLR_INIT_FAILED;
#else
        return DAS_E_CSHARP_NETFX_UNSUPPORTED_PLATFORM;
#endif
    default:
        return DAS_E_CSHARP_UNSUPPORTED_TFM;
    }
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_EXPORT_CSHARP
