#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ManualProxyRegistry.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/Logger/Logger.h>
#include <das/IDasBase.h>
#include <das/Utils/StringUtils.h>

DAS_CORE_IPC_NS_BEGIN

ProxyFactory::ProxyFactory(
    RemoteObjectRegistry&         object_registry,
    IpcRunLoop&                   run_loop,
    std::weak_ptr<BusinessThread> business_thread)
    : object_registry_(object_registry), run_loop_(run_loop),
      business_thread_(std::move(business_thread))
{
}

ProxyFactory::~ProxyFactory() = default;

IPCProxyBase* ProxyFactory::GetOrCreateProxy(
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
    IDasBase* proxy_raw = CreateProxyByInterfaceIdWithFallback(
        interface_id,
        object_id,
        run_loop_,
        business_thread_,
        *this);

    if (proxy_raw == nullptr)
    {
        return nullptr;
    }

    auto* proxy = static_cast<IPCProxyBase*>(proxy_raw);

    // 注册远程对象到 DistributedObjectManager
    object_manager_.RegisterRemoteObject(object_id);

    // 插入缓存
    proxy_cache_[EncodeObjectId(object_id)] = ProxyEntry{
        .proxy = proxy,
        .object_id_encoded = EncodeObjectId(object_id),
        .interface_id = interface_id,
        .session_id = object_id.session_id,
        .local_refcount = 1};

    return proxy;
}

IPCProxyBase* ProxyFactory::GetProxy(const ObjectId& object_id)
{
    std::lock_guard<std::mutex> lock(proxy_cache_mutex_);
    auto it = proxy_cache_.find(EncodeObjectId(object_id));
    if (it != proxy_cache_.end())
    {
        // 缓存命中：只增加本地计数，不发送 IPC
        it->second.local_refcount++;
        it->second.proxy->AddRef();
        return it->second.proxy;
    }
    return nullptr;
}

DasResult ProxyFactory::ReleaseProxy(const ObjectId& object_id)
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

IPCProxyBase* ProxyFactory::CreateIPCProxy(const ObjectId& object_id)
{
    // Generic Proxy 不再支持，需要使用 IDL 生成的具体 Proxy 类
    (void)object_id; // 未使用的参数
    DAS_CORE_LOG_ERROR(
        "CreateIPCProxy: Generic proxy not supported. Use IDL-generated proxy instead.");
    return nullptr;
}

DasResult ProxyFactory::ValidateObject(
    const ObjectId&   object_id,
    RemoteObjectInfo& out_info)
{
    DasResult result = object_registry_.GetObjectInfo(object_id, out_info);
    if (result != DAS_S_OK)
    {
        return result;
    }

    if (!object_registry_.ObjectExists(object_id))
    {
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    return DAS_S_OK;
}

uint32_t ProxyFactory::GetObjectInterfaceId(const ObjectId& object_id)
{
    RemoteObjectInfo info;
    DasResult        result = ValidateObject(object_id, info);
    if (result != DAS_S_OK)
    {
        throw std::runtime_error("Cannot get object interface id");
    }

    return info.interface_id;
}
DAS_CORE_IPC_NS_END
