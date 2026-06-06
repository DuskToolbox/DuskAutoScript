#ifndef DAS_CORE_FOREIGNINTERFACEHOST_CSHARPHOSTFXRBACKEND_H
#define DAS_CORE_FOREIGNINTERFACEHOST_CSHARPHOSTFXRBACKEND_H

#ifdef DAS_EXPORT_CSHARP

#include "CSharpHost.h"

#include <filesystem>
#include <memory>
#include <string_view>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class ICSharpHostFxrAssemblyResolver
{
public:
    virtual ~ICSharpHostFxrAssemblyResolver() = default;

    virtual int LoadAssemblyAndGetFunctionPointer(
        const std::filesystem::path& assembly_path,
        std::string_view             type_name,
        std::string_view             method_name,
        void**                       entrypoint) = 0;
};

class ICSharpHostFxrRuntime
{
public:
    virtual ~ICSharpHostFxrRuntime() = default;

    virtual int GetAssemblyResolver(
        std::unique_ptr<ICSharpHostFxrAssemblyResolver>* resolver) = 0;
};

class ICSharpHostFxrRuntimeLoader
{
public:
    virtual ~ICSharpHostFxrRuntimeLoader() = default;

    virtual int InitializeForRuntimeConfig(
        const std::filesystem::path&            runtime_config_path,
        std::unique_ptr<ICSharpHostFxrRuntime>* runtime) = 0;
};

class CSharpHostFxrBackend final : public ICSharpRuntimeBackend
{
public:
    CSharpHostFxrBackend();
    explicit CSharpHostFxrBackend(
        std::shared_ptr<ICSharpHostFxrRuntimeLoader> runtime_loader);

    DasResult LoadPlugin(
        const CSharpManifest&     manifest,
        DasCSharpBootstrapArgsV1& bootstrap_args) override;

private:
    std::shared_ptr<ICSharpHostFxrRuntimeLoader> runtime_loader_;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_EXPORT_CSHARP

#endif // DAS_CORE_FOREIGNINTERFACEHOST_CSHARPHOSTFXRBACKEND_H
