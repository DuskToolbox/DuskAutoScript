#include <das/Core/ForeignInterfaceHost/RuntimeProvider.h>

#include <das/Core/ForeignInterfaceHost/NativeIpcRuntime.h>
#include <das/Core/ForeignInterfaceHost/NodeRuntime.h>

#include <tl/expected.hpp>

#include <memory>
#include <utility>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace
{
    class LocalRuntimeProvider final : public IRuntimeProvider
    {
    public:
        explicit LocalRuntimeProvider(
            DAS::DasPtr<IForeignLanguageRuntime> runtime)
            : runtime_{std::move(runtime)}
        {
        }

        DasResult LoadPlugin(
            const RuntimeLoadRequest& request,
            RuntimeLoadResult*        out_result) override
        {
            if (!runtime_ || out_result == nullptr
                || request.runtime_path == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            // const char* UTF-8 → std::filesystem::path
            const std::filesystem::path runtime_path(
                reinterpret_cast<const char8_t*>(request.runtime_path));
            auto object = runtime_->LoadPlugin(runtime_path);
            if (!object)
            {
                return object.error();
            }
            // 转移所有权（COM out 约定：AddRef 后调用方 Release）
            out_result->object = object->Get();
            if (out_result->object != nullptr)
            {
                static_cast<void>(out_result->object->AddRef());
            }
            out_result->owner_session_id =
                request.main_process_owner_session_id;
            return DAS_S_OK;
        }

    private:
        DAS::DasPtr<IForeignLanguageRuntime> runtime_;
    };
} // namespace

auto CreateLocalRuntimeProvider(DAS::DasPtr<IForeignLanguageRuntime> runtime)
    -> DAS::Utils::Expected<std::unique_ptr<IRuntimeProvider>>
{
    if (!runtime)
    {
        return tl::make_unexpected(DAS_E_INVALID_POINTER);
    }

    return std::make_unique<LocalRuntimeProvider>(std::move(runtime));
}

auto CreateLocalRuntimeProvider(const ForeignLanguageRuntimeFactoryDesc& desc)
    -> DAS::Utils::Expected<std::unique_ptr<IRuntimeProvider>>
{
    auto runtime = CreateForeignLanguageRuntime(desc);
    if (!runtime)
    {
        return tl::make_unexpected(runtime.error());
    }

    return CreateLocalRuntimeProvider(std::move(runtime.value()));
}

auto CreateNativeIpcRuntimeProvider(
    std::filesystem::path              host_exe_path,
    std::unique_ptr<IRemotePluginHost> remote_plugin_host,
    RuntimeLifecycleCallbacks          callbacks)
    -> DAS::Utils::Expected<std::unique_ptr<IRuntimeProvider>>
{
    if (host_exe_path.empty())
    {
        return tl::make_unexpected(DAS_E_NO_IMPLEMENTATION);
    }
    if (!remote_plugin_host)
    {
        return tl::make_unexpected(DAS_E_INVALID_POINTER);
    }

    return std::make_unique<NativeIpcRuntime>(
        std::move(host_exe_path),
        std::move(remote_plugin_host),
        std::move(callbacks));
}

auto CreateNodeRuntimeProvider(
    std::unique_ptr<IRemotePluginHost> remote_plugin_host,
    RuntimeLifecycleCallbacks          callbacks)
    -> DAS::Utils::Expected<std::unique_ptr<IRuntimeProvider>>
{
    if (!remote_plugin_host)
    {
        return tl::make_unexpected(DAS_E_INVALID_POINTER);
    }

    return std::make_unique<NodeRuntime>(
        std::move(remote_plugin_host),
        std::move(callbacks));
}

auto CreateRuntimeProvider(
    RuntimeProviderFactoryDesc desc,
    RuntimeLifecycleCallbacks  callbacks)
    -> DAS::Utils::Expected<std::unique_ptr<IRuntimeProvider>>
{
    if (desc.language == ForeignInterfaceLanguage::Node)
    {
        return CreateNodeRuntimeProvider(
            std::move(desc.remote_plugin_host),
            std::move(callbacks));
    }

    if (desc.load_mode == LoadMode::Ipc)
    {
        return CreateNativeIpcRuntimeProvider(
            std::move(desc.native_host_exe_path),
            std::move(desc.remote_plugin_host),
            std::move(callbacks));
    }

    switch (desc.language)
    {
    case ForeignInterfaceLanguage::Cpp:
    case ForeignInterfaceLanguage::Python:
    {
        ForeignLanguageRuntimeFactoryDesc factory_desc{};
        factory_desc.language = desc.language;
        return CreateLocalRuntimeProvider(factory_desc);
    }
    default:
        return CreateNativeIpcRuntimeProvider(
            std::move(desc.native_host_exe_path),
            std::move(desc.remote_plugin_host),
            std::move(callbacks));
    }
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
