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

        auto LoadPlugin(const RuntimeLoadRequest& request)
            -> DAS::Utils::Expected<RuntimeLoadResult> override
        {
            if (!runtime_)
            {
                return tl::make_unexpected(DAS_E_OBJECT_NOT_INIT);
            }

            auto object = runtime_->LoadPlugin(request.runtime_path);
            if (!object)
            {
                return tl::make_unexpected(object.error());
            }

            RuntimeLoadResult result{};
            result.object = std::move(object.value());
            result.owner_session_id = request.main_process_owner_session_id;
            return result;
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
    std::unique_ptr<IRemotePluginHost> remote_plugin_host)
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
        std::move(remote_plugin_host));
}

auto CreateNodeRuntimeProvider(
    std::unique_ptr<IRemotePluginHost> remote_plugin_host)
    -> DAS::Utils::Expected<std::unique_ptr<IRuntimeProvider>>
{
    if (!remote_plugin_host)
    {
        return tl::make_unexpected(DAS_E_INVALID_POINTER);
    }

    return std::make_unique<NodeRuntime>(std::move(remote_plugin_host));
}

auto CreateRuntimeProvider(RuntimeProviderFactoryDesc desc)
    -> DAS::Utils::Expected<std::unique_ptr<IRuntimeProvider>>
{
    if (desc.language == ForeignInterfaceLanguage::Node)
    {
        return CreateNodeRuntimeProvider(std::move(desc.remote_plugin_host));
    }

    if (desc.load_mode == LoadMode::Ipc)
    {
        return CreateNativeIpcRuntimeProvider(
            std::move(desc.native_host_exe_path),
            std::move(desc.remote_plugin_host));
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
            std::move(desc.remote_plugin_host));
    }
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
