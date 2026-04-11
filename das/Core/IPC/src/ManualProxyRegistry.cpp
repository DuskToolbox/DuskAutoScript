#include <das/Core/IPC/ManualProxyRegistry.h>

#include <das/Core/IPC/DasProxyBase.h>
#include <das/Core/IPC/DasReadOnlyStringProxy.h>
#include <das/Core/IPC/DasVariantVectorByValueProxy.h>
#include <das/_autogen/idl/ipc/IpcProxyFactory.h>

DAS_CORE_IPC_NS_BEGIN

namespace
{
    std::unordered_map<uint32_t, ManualProxyFactory>& GetManualRegistry()
    {
        static std::unordered_map<uint32_t, ManualProxyFactory> registry;
        return registry;
    }
} // namespace

IDasBase* CreateProxyByInterfaceIdWithFallback(
    uint32_t                      interface_id,
    const ObjectId&               object_id,
    IpcRunLoop&                   run_loop,
    std::weak_ptr<BusinessThread> business_thread,
    ProxyFactory&                 proxy_factory)
{
    // Try autogen factory first
    IDasBase* proxy = DasIpcProxy::CreateProxyByInterfaceId(
        interface_id,
        object_id,
        run_loop,
        business_thread,
        proxy_factory);

    if (proxy != nullptr)
    {
        return proxy;
    }

    // Fall back to manually registered factory
    auto& registry = GetManualRegistry();
    auto  it = registry.find(interface_id);
    if (it != registry.end())
    {
        return it->second(
            interface_id,
            object_id,
            run_loop,
            business_thread,
            proxy_factory);
    }

    return nullptr;
}

void RegisterManualProxyFactory(
    uint32_t           interface_id,
    ManualProxyFactory factory)
{
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
