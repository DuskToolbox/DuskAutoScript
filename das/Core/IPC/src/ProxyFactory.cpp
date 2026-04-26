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
    uint64_t                    key = EncodeObjectId(object_id);
    auto                        it = proxy_cache_.find(key);
    if (it != proxy_cache_.end() && it->second.Get() != nullptr)
    {
        // Cache hit: DasPtr 构造函数内部 AddRef
        return DasPtr<IDasBase>(it->second.Get());
    }

    // Cache miss: 工厂返回 DasPtr，存 ProxyCacheEntry，move 出去
    DasPtr<IDasBase> proxy = CreateProxyByInterfaceIdWithFallback(
        interface_id,
        object_id,
        run_loop,
        std::move(business_thread),
        *this);

    if (!proxy)
    {
        return nullptr;
    }

    // Register remote object
    object_manager_.RegisterRemoteObject(object_id);

    // 存非拥有引用，proxy refcount 不变 (=1)
    proxy_cache_[key] = ProxyCacheEntry(proxy.Get());
    // move 出去，调用方拥有唯一强引用
    return proxy;
}

bool ProxyFactory::HasProxy(const ObjectId& object_id) const
{
    std::lock_guard<std::mutex> lock(proxy_cache_mutex_);
    auto it = proxy_cache_.find(EncodeObjectId(object_id));
    return (it != proxy_cache_.end() && it->second.Get() != nullptr);
}

size_t ProxyFactory::GetProxyCount() const
{
    std::lock_guard<std::mutex> lock(proxy_cache_mutex_);
    return proxy_cache_.size();
}

// 只清查找表，proxy 由外部 DasPtr 自然释放
void ProxyFactory::ClearAllProxies()
{
    std::lock_guard<std::mutex> lock(proxy_cache_mutex_);
    proxy_cache_.clear();
}

void ProxyFactory::InvalidateCacheEntry(const ObjectId& object_id)
{
    std::lock_guard<std::mutex> lock(proxy_cache_mutex_);
    uint64_t                    key = EncodeObjectId(object_id);
    auto                        it = proxy_cache_.find(key);
    if (it != proxy_cache_.end())
    {
        it->second.ptr_ = nullptr;
        proxy_cache_.erase(it);
    }
}

DAS_CORE_IPC_NS_END
