#ifndef DAS_CORE_FOREIGNINTERFACEHOST_CSHARPHOSTFXRBACKEND_H
#define DAS_CORE_FOREIGNINTERFACEHOST_CSHARPHOSTFXRBACKEND_H

#include "CSharpHost.h"

#include <filesystem>
#include <memory>
#include <string_view>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

using CSharpHostFxrHandle = void*;

enum class CSharpHostFxrDelegateType
{
    LoadAssemblyAndGetFunctionPointer
};

struct CSharpHostFxrApi
{
    int (*initialize_for_runtime_config)(
        const std::filesystem::path& runtime_config_path,
        CSharpHostFxrHandle*         handle,
        void*                        user_data) = nullptr;
    int (*get_runtime_delegate)(
        CSharpHostFxrHandle       handle,
        CSharpHostFxrDelegateType type,
        void**                    runtime_delegate,
        void*                     user_data) = nullptr;
    void (*close)(CSharpHostFxrHandle handle, void* user_data) = nullptr;
    int (*load_assembly_and_get_function_pointer)(
        void*                        runtime_delegate,
        const std::filesystem::path& assembly_path,
        std::string_view             type_name,
        std::string_view             method_name,
        void**                       entrypoint,
        void*                        user_data) = nullptr;
    void* user_data = nullptr;
};

class CSharpHostFxrBackend final : public ICSharpRuntimeBackend
{
public:
    CSharpHostFxrBackend();
    explicit CSharpHostFxrBackend(CSharpHostFxrApi api);

    DasResult LoadPlugin(
        const CSharpManifest&     manifest,
        DasCSharpBootstrapArgsV1& bootstrap_args) override;

private:
    CSharpHostFxrBackend(CSharpHostFxrApi api, std::shared_ptr<void> state);

    CSharpHostFxrApi      api_{};
    std::shared_ptr<void> state_;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_CSHARPHOSTFXRBACKEND_H
