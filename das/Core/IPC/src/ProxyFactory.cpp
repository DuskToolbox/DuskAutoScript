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

IDasBase* ProxyFactory::GetOrCreateProxy(
    IpcRunLoop&                   run_loop,
    std::weak_ptr<BusinessThread> business_thread,
    const ObjectId&               object_id,
    uint32_t                      interface_id)
{
    std::lock_guard<std::mutex> lock(proxy_cache_mutex_);
    auto it = proxy_cache_.find(EncodeObjectId(object_id));
    if (it != proxy_cache_.end())
    {
        // 缓存命中：AddRef 返回（DasPtr 持有 1 ref，调用方再 AddRef 1 ref）
        it->second->AddRef();
        return it->second.Get();
    }

    // 缓存未命中：创建 proxy
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

    // 注册远程对象到 DistributedObjectManager
    object_manager_.RegisterRemoteObject(object_id);

    // 插入缓存：DasPtr(proxy) 构造时自动 AddRef
    // proxy 初始 refcount=1，DasPtr 构造 +1 = 2（缓存 1 + 调用方 1）
    proxy_cache_[EncodeObjectId(object_id)] = DasPtr<IDasBase>(proxy);

    return proxy;
}

DasResult ProxyFactory::RemoveFromCache(const ObjectId& object_id)
{
    std::lock_guard<std::mutex> lock(proxy_cache_mutex_);
    auto it = proxy_cache_.find(EncodeObjectId(object_id));
    if (it != proxy_cache_.end())
    {
        proxy_cache_.erase(it);
        // DasPtr 析构自动 Release
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
