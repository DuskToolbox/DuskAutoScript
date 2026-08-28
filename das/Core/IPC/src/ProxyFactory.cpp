#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IPCProxyBase.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcRuntimeState.h>
#include <das/Core/IPC/ManualProxyRegistry.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/Logger/Logger.h>
#include <das/IDasBase.h>
#include <das/Utils/StringUtils.h>

DAS_CORE_IPC_NS_BEGIN

ProxyFactory::ProxyFactory()
    : runtime_state_(std::make_shared<IpcRuntimeState>())
{
}

ProxyFactory::~ProxyFactory() { BeginShutdown(); }

std::weak_ptr<IpcRuntimeState> ProxyFactory::GetRuntimeState() const noexcept
{
    return runtime_state_;
}

void ProxyFactory::BeginShutdown() noexcept
{
    if (runtime_state_)
    {
        runtime_state_->BeginShutdown();
    }
    ClearAllProxies();
}

bool ProxyFactory::IsShuttingDown() const noexcept
{
    return runtime_state_ && runtime_state_->IsShuttingDown();
}

std::pair<DasResult, DasPtr<IDasBase>> ProxyFactory::GetOrCreateProxy(
    IpcRunLoop&                   run_loop,
    std::weak_ptr<BusinessThread> business_thread,
    const ObjectId&               object_id,
    uint32_t                      interface_id)
{
    if (IsShuttingDown())
    {
        return {DAS_E_IPC_DISCONNECTED, nullptr};
    }

    std::unique_lock<std::mutex> lock(proxy_cache_mutex_);
    if (IsShuttingDown())
    {
        return {DAS_E_IPC_DISCONNECTED, nullptr};
    }

    const uint64_t key = EncodeObjectId(object_id);
    auto           it = proxy_cache_.find(key);
    if (it != proxy_cache_.end())
    {
        auto view_it = it->second.interfaces.find(interface_id);
        if (view_it != it->second.interfaces.end())
        {
            auto& entry = view_it->second;
            if (entry.runtime_ptr != nullptr && entry.interface_ptr != nullptr
                && entry.runtime_ptr->TryAddRefForCache())
            {
                return {
                    DAS_S_OK,
                    DasPtr<IDasBase>::Attach(entry.interface_ptr)};
            }

            it->second.interfaces.erase(view_it);
            if (it->second.interfaces.empty())
            {
                proxy_cache_.erase(it);
            }
        }
    }

    // Cache miss: 工厂返回 DasPtr，存 ProxyCacheEntry，move 出去
    auto [create_result, proxy] = CreateProxyByInterfaceIdWithFallback(
        interface_id,
        object_id,
        run_loop,
        std::move(business_thread),
        *this);

    if (!proxy)
    {
        DAS_CORE_LOG_ERROR(
            "CreateProxyByInterfaceIdWithFallback failed. "
            "result={}, interface_id=0x{:08X}",
            create_result,
            interface_id);
        return {create_result, nullptr};
    }

    if (IsShuttingDown())
    {
        return {DAS_E_IPC_DISCONNECTED, nullptr};
    }

    IPCProxyBase* runtime_ptr = nullptr;
    DasResult     tag_result = proxy->QueryInterface(
        DasIidOf<IPCProxyBase>(),
        reinterpret_cast<void**>(&runtime_ptr));
    if (DAS::IsFailed(tag_result) || runtime_ptr == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "QueryInterface(IPCProxyBase) failed. "
            "tag_result={}, interface_id=0x{:08X}",
            tag_result,
            interface_id);
        lock.unlock();
        return {tag_result, nullptr};
    }
    static_cast<void>(runtime_ptr->ReleaseRuntimeTagRef());

    bool      newly_created = false;
    DasResult register_result =
        object_manager_.RegisterRemoteObject(object_id, newly_created);
    if (DAS::IsFailed(register_result))
    {
        DAS_CORE_LOG_ERROR(
            "RegisterRemoteObject failed. "
            "result={}, interface_id=0x{:08X}",
            register_result,
            interface_id);
        lock.unlock();
        return {register_result, nullptr};
    }

    // 首次物化申报：DOM remote 条目 0→1 时向 owner 申报一条 CONTROL_PLANE
    // RemoteAddRef（fire-and-forget），与条目移除时的 RemoteRelease 严格
    // 配对——每 (对象×对端) 一对消息，后续接口视图物化（dedup）不再发。
    // D-19 分叉（线程判别已并除）：piggyback 总闸开 → 入队候车（外部
    // 线程同步调用稳态下下一帧几十微秒后必到，drain 全队顺路带走——
    // 立即触发会让批永远只剩 1 帧，合写/拼段稳态零执行）；总闸关闭 →
    // 当场直发（零延迟零窗口回退语义）。帧构造单一事实源在 IpcRunLoop
    if (newly_created)
    {
        if (run_loop.IsPiggybackEnabled())
        {
            run_loop.EnqueueAddRef(object_id);
        }
        else
        {
            static_cast<void>(run_loop.PostAddRefStandalone(object_id));
        }
    }

    proxy_cache_[key].interfaces[interface_id] =
        ProxyCacheEntry(runtime_ptr, proxy.Get());
    // move 出去，调用方拥有唯一强引用
    return {DAS_S_OK, std::move(proxy)};
}

bool ProxyFactory::HasProxy(const ObjectId& object_id) const
{
    if (IsShuttingDown())
    {
        return false;
    }

    const std::scoped_lock lock(proxy_cache_mutex_);
    auto                   it = proxy_cache_.find(EncodeObjectId(object_id));
    if (it == proxy_cache_.end())
    {
        return false;
    }
    for (const auto& [interface_id, entry] : it->second.interfaces)
    {
        (void)interface_id;
        if (entry.Get() != nullptr)
        {
            return true;
        }
    }
    return false;
}

size_t ProxyFactory::GetProxyCount() const
{
    const std::scoped_lock lock(proxy_cache_mutex_);
    size_t                 count = 0;
    for (const auto& [encoded_id, identity] : proxy_cache_)
    {
        (void)encoded_id;
        count += identity.interfaces.size();
    }
    return count;
}

// 只清查找表，proxy 由外部 DasPtr 自然释放
void ProxyFactory::ClearAllProxies()
{
    const std::scoped_lock lock(proxy_cache_mutex_);
    proxy_cache_.clear();
}

void ProxyFactory::InvalidateCacheEntry(const ObjectId& object_id)
{
    const std::scoped_lock lock(proxy_cache_mutex_);
    const uint64_t         key = EncodeObjectId(object_id);
    auto                   it = proxy_cache_.find(key);
    if (it != proxy_cache_.end())
    {
        proxy_cache_.erase(it);
    }
}

void ProxyFactory::OnProxyFinalRelease(
    const ObjectId& object_id,
    uint32_t        interface_id,
    IPCProxyBase*   runtime_ptr) noexcept
{
    const std::scoped_lock lock(proxy_cache_mutex_);
    auto identity_it = proxy_cache_.find(EncodeObjectId(object_id));
    if (identity_it == proxy_cache_.end())
    {
        return;
    }

    auto view_it = identity_it->second.interfaces.find(interface_id);
    if (view_it == identity_it->second.interfaces.end())
    {
        return;
    }

    if (view_it->second.runtime_ptr != runtime_ptr)
    {
        return;
    }

    identity_it->second.interfaces.erase(view_it);
    if (identity_it->second.interfaces.empty())
    {
        proxy_cache_.erase(identity_it);
    }
}

DAS_CORE_IPC_NS_END
