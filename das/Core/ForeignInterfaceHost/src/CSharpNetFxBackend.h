#ifndef DAS_CORE_FOREIGNINTERFACEHOST_CSHARPNETFXBACKEND_H
#define DAS_CORE_FOREIGNINTERFACEHOST_CSHARPNETFXBACKEND_H

#ifdef DAS_EXPORT_CSHARP

#include "CSharpHost.h"

#include <das/Utils/Expected.h>

#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class ICSharpNetFxRuntimeHost
{
public:
    virtual ~ICSharpNetFxRuntimeHost() = default;

    // 成功时返回值即托管入口（static int Method(string)）的返回码。
    virtual auto ExecuteInDefaultAppDomain(
        const std::filesystem::path& assembly_path,
        std::string_view             type_name,
        std::string_view             method_name,
        std::string_view             bootstrap_cookie) const
        -> DAS::Utils::Expected<DasResult> = 0;
};

class ICSharpNetFxRuntimeLoader
{
public:
    virtual ~ICSharpNetFxRuntimeLoader() = default;

    virtual auto StartRuntime() const
        -> DAS::Utils::Expected<std::unique_ptr<ICSharpNetFxRuntimeHost>> = 0;
};

class CSharpNetFxBackend final : public ICSharpRuntimeBackend
{
public:
    CSharpNetFxBackend();
    explicit CSharpNetFxBackend(
        std::shared_ptr<ICSharpNetFxRuntimeLoader> runtime_loader);

protected:
    auto InvokeBootstrapEntrypoint(
        const CSharpManifest&     manifest,
        DasCSharpBootstrapArgsV1& bootstrap_args)
        -> DAS::Utils::Expected<DasResult> override;

private:
    std::shared_ptr<ICSharpNetFxRuntimeLoader> runtime_loader_;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_EXPORT_CSHARP

#endif // DAS_CORE_FOREIGNINTERFACEHOST_CSHARPNETFXBACKEND_H
