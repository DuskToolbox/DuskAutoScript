#ifndef DAS_CORE_FOREIGNINTERFACEHOST_CSHARPHOST_H
#define DAS_CORE_FOREIGNINTERFACEHOST_CSHARPHOST_H

#include <das/Core/ForeignInterfaceHost/CSharpBootstrap.h>
#include <das/Core/ForeignInterfaceHost/CSharpManifest.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/Utils/CommonUtils.hpp>

#include <memory>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class ICSharpRuntimeBackend
{
public:
    virtual ~ICSharpRuntimeBackend() = default;

    virtual DasResult LoadPlugin(
        const CSharpManifest&     manifest,
        DasCSharpBootstrapArgsV1& bootstrap_args) = 0;
};

struct CSharpRuntimeBackendSet
{
    std::unique_ptr<ICSharpRuntimeBackend> modern_dotnet;
    std::unique_ptr<ICSharpRuntimeBackend> netfx;
};

class CSharpHost final : public IForeignLanguageRuntime
{
public:
    CSharpHost();
    explicit CSharpHost(CSharpRuntimeBackendSet backends);

    static auto CreateForeignLanguageRuntime(
        const ForeignLanguageRuntimeFactoryDesc& desc)
        -> DAS::Utils::Expected<DasPtr<IForeignLanguageRuntime>>;

    auto LoadPlugin(const std::filesystem::path& manifest_path)
        -> DAS::Utils::Expected<DasPtr<IDasBase>> override;

    uint32_t  AddRef() override { return ref_counter_.AddRef(); }
    uint32_t  Release() override { return ref_counter_.Release(this); }
    DasResult QueryInterface(const DasGuid& iid, void** pp_out_object) override;

    static DasResult BackendUnavailableError(
        CSharpTargetFrameworkFamily family) noexcept;

private:
    CSharpRuntimeBackendSet            backends_;
    DAS::Utils::RefCounter<CSharpHost> ref_counter_{};
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_CSHARPHOST_H
