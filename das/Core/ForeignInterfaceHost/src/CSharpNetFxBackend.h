#ifndef DAS_CORE_FOREIGNINTERFACEHOST_CSHARPNETFXBACKEND_H
#define DAS_CORE_FOREIGNINTERFACEHOST_CSHARPNETFXBACKEND_H

#include "CSharpHost.h"

#include <filesystem>
#include <memory>
#include <string_view>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

using CSharpNetFxHostHandle = void*;

struct CSharpNetFxApi
{
    int (*start_clr)(CSharpNetFxHostHandle* handle, void* user_data) = nullptr;
    int (*execute_in_default_app_domain)(
        CSharpNetFxHostHandle        handle,
        const std::filesystem::path& assembly_path,
        std::string_view             type_name,
        std::string_view             method_name,
        std::string_view             bootstrap_cookie,
        unsigned long*               return_value,
        void*                        user_data) = nullptr;
    void (*release)(CSharpNetFxHostHandle handle, void* user_data) = nullptr;
    void* user_data = nullptr;
};

class CSharpNetFxBackend final : public ICSharpRuntimeBackend
{
public:
    CSharpNetFxBackend();
    explicit CSharpNetFxBackend(CSharpNetFxApi api);

    DasResult LoadPlugin(
        const CSharpManifest&     manifest,
        DasCSharpBootstrapArgsV1& bootstrap_args) override;

private:
    CSharpNetFxBackend(CSharpNetFxApi api, std::shared_ptr<void> state);

    CSharpNetFxApi        api_{};
    std::shared_ptr<void> state_;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_CSHARPNETFXBACKEND_H
