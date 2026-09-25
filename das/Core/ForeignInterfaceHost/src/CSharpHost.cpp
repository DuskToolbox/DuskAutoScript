#ifdef DAS_EXPORT_CSHARP

#include "CSharpHost.h"
#include "CSharpHostFxrBackend.h"
#include "CSharpNetFxBackend.h"

#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/Utils/PathText.h>

#include <fstream>
#include <iterator>
#include <string>
#include <utility>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

auto ICSharpRuntimeBackend::LoadPlugin(const CSharpManifest& manifest)
    -> DAS::Utils::Expected<DasPtr<DAS::PluginInterface::IDasPluginPackage>>
{
    if (manifest.entry_point.type_name.empty()
        || manifest.entry_point.method_name.empty())
    {
        return DAS::Utils::MakeUnexpected(DAS_E_CSHARP_ENTRYPOINT_INVALID);
    }

    // string 存储必须是局部变量：bootstrap_args 只保留 c_str() 指针。
    const auto manifest_path_storage =
        DAS::Utils::PathToUtf8(manifest.manifest_path);
    const auto plugin_root_storage =
        DAS::Utils::PathToUtf8(manifest.package_root);
    const auto plugin_binary_path_storage =
        DAS::Utils::PathToUtf8(manifest.plugin_binary_path);

    // 包所有权集中在此：任何失败路径由 DasPtr 析构自动 Release。
    DasPtr<DAS::PluginInterface::IDasPluginPackage> package;
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
        return DAS::Utils::MakeUnexpected(args_result);
    }

    try
    {
        const auto invoke_result =
            InvokeBootstrapEntrypoint(manifest, bootstrap_args);
        if (!invoke_result)
        {
            return DAS::Utils::MakeUnexpected(invoke_result.error());
        }

        if (DAS::IsFailed(invoke_result.value()))
        {
            DAS_CORE_LOG_ERROR(
                "C# bootstrap entrypoint failed: result={}",
                invoke_result.value());
            return DAS::Utils::MakeUnexpected(DAS_E_CSHARP_PLUGIN_INIT_FAILED);
        }
    }
    catch (...)
    {
        return DAS::Utils::MakeUnexpected(DAS_E_CSHARP_PLUGIN_INIT_FAILED);
    }

    if (!package)
    {
        return DAS::Utils::MakeUnexpected(DAS_E_CSHARP_PLUGIN_INIT_FAILED);
    }

    return std::move(package);
}

CSharpHost::CSharpHost()
{
#ifdef DAS_CSHARP_HOSTFXR_BACKEND
    backends_.modern_dotnet = std::make_unique<CSharpHostFxrBackend>();
#endif
#if defined(_WIN32) && defined(DAS_CSHARP_NETFX_BACKEND)
    backends_.netfx = std::make_unique<CSharpNetFxBackend>();
#endif
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
        return DAS::Utils::MakeUnexpected(DAS_E_INVALID_ARGUMENT);
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
            DAS::Utils::PathToUtf8(manifest_path));
        return DAS::Utils::MakeUnexpected(DAS_E_FILE_NOT_FOUND);
    }

    const std::string manifest_json(
        (std::istreambuf_iterator<char>(manifest_file)),
        std::istreambuf_iterator<char>());

    auto manifest = ParseCSharpManifest(manifest_path, manifest_json);
    if (!manifest)
    {
        return DAS::Utils::MakeUnexpected(manifest.error());
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
        return DAS::Utils::MakeUnexpected(DAS_E_CSHARP_UNSUPPORTED_TFM);
    }

    if (backend == nullptr)
    {
        return DAS::Utils::MakeUnexpected(
            CSharpHost::BackendUnavailableError(
                manifest->target_framework_family));
    }

    auto package = backend->LoadPlugin(manifest.value());
    if (!package)
    {
        return DAS::Utils::MakeUnexpected(package.error());
    }

    DasPtr<IDasBase> result;
    const auto       qi_result = package.value().As(result);
    if (DAS::IsFailed(qi_result))
    {
        return DAS::Utils::MakeUnexpected(qi_result);
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
