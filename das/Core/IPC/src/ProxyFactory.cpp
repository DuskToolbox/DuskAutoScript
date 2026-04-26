#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IPCProxyBase.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/ManualProxyRegistry.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/Logger/Logger.h>
#include <das/IDasBase.h>
#include <das/Utils/StringUtils.h>

DAS_CORE_IPC_NS_BEGIN

ProxyFactory::ProxyFactory() = default;

ProxyFactory::~ProxyFactory() = default;

DasPtr<IDasBase> ProxyFactory::GetOrCreateProxy(
    IpcRunLoop&                   run_loop,
    std::weak_ptr<BusinessThread> business_thread,
    const ObjectId&               object_id,
    uint32_t                      interface_id)
{
    std::lock_guard<std::mutex> lock(proxy_cache_mutex_);
    auto it = proxy_cache_.find(EncodeObjectId(object_id));
    if (it != proxy_cache_.end())
    {
        // Cache hit: copy DasPtr (AddRef) and return
        return it->second;
    }

    // Cache miss: create proxy
    IDasBase* proxy = CreateProxyByInterfaceIdWithFallback(
        interface_id,
        object_id,
        run_loop,
        std::move(business_thread),
        *this);

    if (proxy == nullptr)
    {
        return nullptr;
    }

    // Register remote object
    object_manager_.RegisterRemoteObject(object_id);

    // proxy refcount=1 from constructor
    // Attach: no AddRef, cache "owns" the initial ref -> refcount stays 1
    proxy_cache_[EncodeObjectId(object_id)] = DasPtr<IDasBase>::Attach(proxy);

    // Return copy from cache: DasPtr copy -> AddRef -> refcount=2
    // cache holds 1 ref, caller holds 1 ref
    return proxy_cache_[EncodeObjectId(object_id)];
}

bool ProxyFactory::HasProxy(const ObjectId& object_id) const
{
    std::lock_guard<std::mutex> lock(proxy_cache_mutex_);
    auto it = proxy_cache_.find(EncodeObjectId(object_id));
    return (it != proxy_cache_.end());
}

size_t ProxyFactory::GetProxyCount() const
{
    std::lock_guard<std::mutex> lock(proxy_cache_mutex_);
    return proxy_cache_.size();
}

void ProxyFactory::ClearAllProxies()
{
    std::lock_guard<std::mutex> lock(proxy_cache_mutex_);
    proxy_cache_.clear();
}
DAS_CORE_IPC_NS_END
