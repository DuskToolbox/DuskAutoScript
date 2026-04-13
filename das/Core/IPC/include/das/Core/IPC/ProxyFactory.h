#ifndef DAS_CORE_IPC_PROXY_FACTORY_H
#define DAS_CORE_IPC_PROXY_FACTORY_H

#include <atomic>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <das/Utils/StringUtils.h>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <das/Core/IPC/Config.h>
#include <das/DasConfig.h>

DAS_CORE_IPC_NS_BEGIN

class BusinessThread; // forward declaration
class IpcRunLoop;     // forward declaration
class IPCProxyBase;   // forward declaration
/**
 * @brief Proxy 工厂类，统一获取 Proxy 实例的入口
 *
 * 提供统一的 Proxy 实例创建、获取和释放接口
 * 零依赖默认构造，作为 IpcContext 的值成员存在
 * 值持有 DistributedObjectManager
 */
class ProxyFactory
{
public:
    ProxyFactory();
    ~ProxyFactory();

    /**
     * @brief 获取分布式对象管理器
     * @return DistributedObjectManager& 对象管理器引用
     */
    [[nodiscard]]
    DistributedObjectManager& DAS_LIFETIMEBOUND GetObjectManager()
    {
        return object_manager_;
    }

    /**
     * @brief 获取或创建 Proxy 实例（统一入口）
     *
     * 缓存命中时 AddRef 返回现有实例，未命中时创建新 proxy 并注册到缓存
     * @param run_loop IpcRunLoop 引用（传递给 proxy 构造函数）
     * @param business_thread BusinessThread weak_ptr（传递给 proxy 构造函数）
     * @param object_id 对象 ID
     * @param interface_id 接口 ID
     * @return Proxy 实例指针（引用计数已+1），失败返回 nullptr
     */
    IDasBase* GetOrCreateProxy(
        IpcRunLoop&                   run_loop,
        std::weak_ptr<BusinessThread> business_thread,
        const ObjectId&               object_id,
        uint32_t                      interface_id);

    /**
     * @brief 从缓存中移除
     Proxy（内部方法，供
     Proxy::Release 调用）
     * @param object_id 对象
     ID

     * * @return DasResult 操作结果
     */
    DasResult RemoveFromCache(const ObjectId& object_id);

    /**
     * @brief 检查 Proxy 实例是否存在
     * @param object_id 对象 ID
     * @return 如果存在返回 true，否则返回 false
     */
    bool HasProxy(const ObjectId& object_id) const;

    /**
     * @brief 获取所有已创建的 Proxy 对象数量
     * @return Proxy 对象数量
     */
    size_t GetProxyCount() const;

    /**
     * @brief 清空所有 Proxy 实例
     */
    void ClearAllProxies();

    // 禁止拷贝和赋值
    ProxyFactory(const ProxyFactory&) = delete;
    ProxyFactory& operator=(const ProxyFactory&) = delete;

private:
    // 缓存已创建的 Proxy（DasPtr 自动管理 AddRef/Release）
    std::unordered_map<uint64_t, DasPtr<IDasBase>> proxy_cache_;

    // 互斥锁保护代理缓存
    mutable std::mutex proxy_cache_mutex_;

    // 分布式对象管理器（值持有）
    DistributedObjectManager object_manager_;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_PROXY_FACTORY_H
