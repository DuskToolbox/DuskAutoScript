#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IPCProxyBase.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ManualProxyRegistry.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/Logger/Logger.h>
#include <das/IDasBase.h>
#include <das/Utils/StringUtils.h>

DAS_CORE_IPC_NS_BEGIN

ProxyFactory::ProxyFactory(
    IpcRunLoop&                   run_loop,
    std::weak_ptr<BusinessThread> business_thread)
    : run_loop_(run_loop), business_thread_(std::move(business_thread))
{
}

ProxyFactory::~ProxyFactory() = default;

IDasBase* ProxyFactory::GetOrCreateProxy(
    const ObjectId& object_id,
    uint32_t        interface_id)
{
    std::lock_guard<std::mutex> lock(proxy_cache_mutex_);
    auto it = proxy_cache_.find(EncodeObjectId(object_id));
    if (it != proxy_cache_.end())
    {
        // 缓存命中：增加本地计数 + AddRef
        it->second.local_refcount++;
        it->second.proxy->AddRef();
        return it->second.proxy;
    }

    // 缓存未命中：创建 proxy
    IDasBase* proxy = CreateProxyByInterfaceIdWithFallback(
        interface_id,
        object_id,
        run_loop_,
        business_thread_,
        *this);

    if (proxy == nullptr)
    {
        return nullptr;
    }

    // 注册远程对象到 DistributedObjectManager
    object_manager_.RegisterRemoteObject(object_id);

    // 插入缓存
    proxy_cache_[EncodeObjectId(object_id)] = ProxyEntry{
        .proxy = proxy,
        .object_id_encoded = EncodeObjectId(object_id),
        .local_refcount = 1};

    return proxy;
}

DasResult ProxyFactory::RemoveFromCache(const ObjectId& object_id)
{
    std::lock_guard<std::mutex> lock(proxy_cache_mutex_);
    auto it = proxy_cache_.find(EncodeObjectId(object_id));
    if (it != proxy_cache_.end())
    {
        it->second.local_refcount--;
        if (it->second.local_refcount == 0)
        {
            proxy_cache_.erase(it);
        }

        return DAS_S_OK;
    }

    return DAS_E_IPC_OBJECT_NOT_FOUND;
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
