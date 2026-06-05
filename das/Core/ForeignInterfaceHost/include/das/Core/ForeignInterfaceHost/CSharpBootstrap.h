#ifndef DAS_CORE_FOREIGNINTERFACEHOST_CSHARPBOOTSTRAP_H
#define DAS_CORE_FOREIGNINTERFACEHOST_CSHARPBOOTSTRAP_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/DasApi.h>
#include <das/DasTypes.hpp>

#include <cstddef>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

inline constexpr std::uint32_t DAS_CSHARP_BOOTSTRAP_ABI_VERSION_V1 = 1;

struct DasCSharpHostApiV1;

// DAS C# entrypoint contract:
// - manifest entryPoint is custom and resolves to Namespace.Type.Method;
//   type name comes first, method name comes last.
// - the resolved C# method must be public static. The manifest customizes
//   the name/location, not instance-method dispatch.
// - split entryPoint at the last '.', using the left side as full type name
//   and the right side as method name. Do not accept method-only entrypoints.
// - namespace segments are part of the full type name; do not add a separate
//   namespace manifest field.
// - managed assembly path follows the existing DAS package convention:
//   manifest_dir / (name + "." + pluginFilenameExtension). Do not add
//   a separate managed-assembly path field to the C# manifest contract.
// - modern .NET runtimeconfig resolves from optional runtimeConfigPath, or
//   defaults to manifest_dir / (name + ".runtimeconfig.json").
// - modern .NET uses hostfxr load_assembly_and_get_function_pointer with
//   the default ComponentEntryPoint(IntPtr args, int sizeBytes) signature;
//   do not use a custom delegate type in the first version.
// - net48 uses ICLRRuntimeHost::ExecuteInDefaultAppDomain, whose direct
//   callable shape is static int Method(string); the string is an opaque
//   pointer/cookie to DasCSharpBootstrapArgsV1.
// - Both paths write IDasPluginPackage* into the bootstrap args. Do not
//   replace this with reflection scan, attribute scan, or global COM registry.
struct DasCSharpBootstrapArgsV1
{
    std::uint32_t                             size;
    std::uint32_t                             abi_version;
    const char*                               manifest_path;
    const char*                               plugin_root;
    const char*                               plugin_binary_path;
    DasCSharpHostApiV1*                       host_api;
    Das::PluginInterface::IDasPluginPackage** pp_package;
};

inline constexpr std::uint32_t DAS_CSHARP_BOOTSTRAP_ARGS_V1_SIZE =
    static_cast<std::uint32_t>(sizeof(DasCSharpBootstrapArgsV1));

// The args pointer is valid only during one in-process entrypoint invocation.
// C# bootstrap code must copy any strings it needs and must not retain args.
inline DasResult ValidateDasCSharpBootstrapArgsV1(
    const DasCSharpBootstrapArgsV1* args)
{
    if (args == nullptr)
    {
        return DAS_E_CSHARP_BOOTSTRAP_INVALID;
    }

    if (args->size != DAS_CSHARP_BOOTSTRAP_ARGS_V1_SIZE)
    {
        return DAS_E_CSHARP_BOOTSTRAP_INVALID;
    }

    if (args->abi_version != DAS_CSHARP_BOOTSTRAP_ABI_VERSION_V1)
    {
        return DAS_E_CSHARP_BOOTSTRAP_INVALID;
    }

    if (args->pp_package == nullptr)
    {
        return DAS_E_CSHARP_BOOTSTRAP_INVALID;
    }

    return DAS_S_OK;
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_CSHARPBOOTSTRAP_H
