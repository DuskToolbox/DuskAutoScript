#ifndef DAS_CORE_FOREIGNINTERFACEHOST_CSHARPHOSTFXRBACKEND_H
#define DAS_CORE_FOREIGNINTERFACEHOST_CSHARPHOSTFXRBACKEND_H

#ifdef DAS_EXPORT_CSHARP

#include "CSharpHost.h"

#include <das/IDasBase.h>
#include <das/Utils/Expected.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <type_traits>
#include <utility>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

// hostfxr load_assembly_and_get_function_pointer 落点的组件入口签名：
// ComponentEntryPoint(void* args, int32_t sizeBytes)。
using CSharpComponentEntrypointFn =
    std::add_pointer_t<int DAS_STD_CALL(void* args, std::int32_t size_bytes)>;

class ICSharpHostFxrRuntime
{
public:
    virtual ~ICSharpHostFxrRuntime() = default;

    virtual auto LoadAssemblyAndGetFunctionPointer(
        const std::filesystem::path& assembly_path,
        std::string_view             type_name,
        std::string_view             method_name) const
        -> DAS::Utils::Expected<CSharpComponentEntrypointFn> = 0;
};

class ICSharpHostFxrRuntimeLoader
{
public:
    virtual ~ICSharpHostFxrRuntimeLoader() = default;

    virtual auto InitializeForRuntimeConfig(
        const std::filesystem::path& runtime_config_path) const
        -> DAS::Utils::Expected<std::unique_ptr<ICSharpHostFxrRuntime>> = 0;
};

class CSharpHostFxrBackend final : public ICSharpRuntimeBackend
{
public:
    CSharpHostFxrBackend();
    explicit CSharpHostFxrBackend(
        std::shared_ptr<ICSharpHostFxrRuntimeLoader> runtime_loader);

protected:
    auto InvokeBootstrapEntrypoint(
        const CSharpManifest&     manifest,
        DasCSharpBootstrapArgsV1& bootstrap_args)
        -> DAS::Utils::Expected<DasResult> override;

private:
    std::shared_ptr<ICSharpHostFxrRuntimeLoader> runtime_loader_;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_EXPORT_CSHARP

#endif // DAS_CORE_FOREIGNINTERFACEHOST_CSHARPHOSTFXRBACKEND_H
