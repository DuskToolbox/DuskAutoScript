#ifndef DAS_CORE_FOREIGNINTERFACEHOST_CSHARPNETFXBACKEND_H
#define DAS_CORE_FOREIGNINTERFACEHOST_CSHARPNETFXBACKEND_H

#ifdef DAS_EXPORT_CSHARP

#include "CSharpHost.h"

#include <filesystem>
#include <memory>
#include <string_view>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class ICSharpNetFxRuntimeHost
{
public:
    virtual ~ICSharpNetFxRuntimeHost() = default;

    virtual int ExecuteInDefaultAppDomain(
        const std::filesystem::path& assembly_path,
        std::string_view             type_name,
        std::string_view             method_name,
        std::string_view             bootstrap_cookie,
        unsigned long*               return_value) = 0;
};

class ICSharpNetFxRuntimeLoader
{
public:
    virtual ~ICSharpNetFxRuntimeLoader() = default;

    virtual int StartRuntime(
        std::unique_ptr<ICSharpNetFxRuntimeHost>* runtime_host) = 0;
};

class CSharpNetFxBackend final : public ICSharpRuntimeBackend
{
public:
    CSharpNetFxBackend();
    explicit CSharpNetFxBackend(
        std::shared_ptr<ICSharpNetFxRuntimeLoader> runtime_loader);

    DasResult LoadPlugin(
        const CSharpManifest&     manifest,
        DasCSharpBootstrapArgsV1& bootstrap_args) override;

private:
    std::shared_ptr<ICSharpNetFxRuntimeLoader> runtime_loader_;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_EXPORT_CSHARP

#endif // DAS_CORE_FOREIGNINTERFACEHOST_CSHARPNETFXBACKEND_H
