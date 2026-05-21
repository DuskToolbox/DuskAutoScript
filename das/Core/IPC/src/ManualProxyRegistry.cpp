#include <das/Core/IPC/ManualProxyRegistry.h>

#include <das/Core/IPC/DasProxyBase.h>
#include <das/Core/IPC/DasReadOnlyStringProxy.h>
#include <das/Core/IPC/DasVariantVectorByValueProxy.h>
#include <das/_autogen/idl/ipc/IpcProxyFactory.h>

#include <mutex>

DAS_CORE_IPC_NS_BEGIN

namespace
{
    std::unordered_map<uint32_t, ManualProxyFactory>& GetManualRegistry()
    {
        static std::unordered_map<uint32_t, ManualProxyFactory> registry;
        return registry;
    }

    std::mutex& GetManualRegistryMutex()
    {
        static std::mutex mutex;
        return mutex;
    }
} // namespace

std::pair<DasResult, DasPtr<IDasBase>> CreateProxyByInterfaceIdWithFallback(
    uint32_t                      interface_id,
    const ObjectId&               object_id,
    IpcRunLoop&                   run_loop,
    std::weak_ptr<BusinessThread> business_thread,
    ProxyFactory&                 proxy_factory)
{
    ManualProxyFactory manual_factory = nullptr;
    {
        std::lock_guard lock{GetManualRegistryMutex()};
        auto&           registry = GetManualRegistry();
        auto            it = registry.find(interface_id);
        if (it != registry.end())
        {
            manual_factory = it->second;
        }
    }

    if (manual_factory != nullptr)
    {
        IDasBase* manual_proxy = manual_factory(
            interface_id,
            object_id,
            run_loop,
            business_thread,
            proxy_factory);
        if (manual_proxy != nullptr)
        {
            return {DAS_S_OK, DasPtr<IDasBase>::Attach(manual_proxy)};
        }
    }

    IDasBase* proxy = DasIpcProxy::CreateProxyByInterfaceId(
        interface_id,
        object_id,
        run_loop,
        business_thread,
        proxy_factory);

    if (proxy != nullptr)
    {
        return {DAS_S_OK, DasPtr<IDasBase>::Attach(proxy)};
    }

    return {DAS_E_NO_INTERFACE, nullptr};
}

void RegisterManualProxyFactory(
    uint32_t           interface_id,
    ManualProxyFactory factory)
{
    std::lock_guard lock{GetManualRegistryMutex()};
    GetManualRegistry()[interface_id] = factory;
}

namespace
{
    struct ManualProxyRegistrar
    {
        ManualProxyRegistrar()
        {
            RegisterManualProxyFactory(
                DasReadOnlyStringProxy::InterfaceId,
                [](uint32_t                      interface_id,
                   const ObjectId&               object_id,
                   IpcRunLoop&                   run_loop,
                   std::weak_ptr<BusinessThread> business_thread,
                   ProxyFactory&                 proxy_factory) -> IDasBase*
                {
                    return new DasReadOnlyStringProxy(
                        interface_id,
                        object_id,
                        run_loop,
                        std::move(business_thread),
                        proxy_factory);
                });

            RegisterManualProxyFactory(
                DasVariantVectorByValueProxy::InterfaceId,
                [](uint32_t                      interface_id,
                   const ObjectId&               object_id,
                   IpcRunLoop&                   run_loop,
                   std::weak_ptr<BusinessThread> business_thread,
                   ProxyFactory&                 proxy_factory) -> IDasBase*
                {
                    return new DasVariantVectorByValueProxy(
                        interface_id,
                        object_id,
                        run_loop,
                        std::move(business_thread),
                        proxy_factory);
                });
        }
    };

    static ManualProxyRegistrar s_registrar;
} // namespace

DAS_CORE_IPC_NS_END
