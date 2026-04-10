#ifndef DAS_CORE_IPC_PROXY_FACTORY_H
#define DAS_CORE_IPC_PROXY_FACTORY_H

#include <atomic>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IPCProxyBase.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <das/Utils/StringUtils.h>

#include <das/Core/IPC/Config.h>
#include <das/DasConfig.h>

DAS_CORE_IPC_NS_BEGIN
/**
 * @brief Proxy 工厂类，统一获取 Proxy 实例的入口
 *
 * 提供统一的 Proxy 实例创建、获取和释放接口
 * 使用单例模式，确保全局只有一个 Proxy 工厂实例
 * 复用 RemoteObjectRegistry 获取对象信息，复用 DistributedObjectManager
 * 管理对象生命周期
 */
class ProxyFactory
{
public:
    // 获取单例实例
    static ProxyFactory& GetInstance();

    /**
     * @brief 初始化 ProxyFactory
     * @param object_manager 分布式对象管理器
     * @param object_registry 远程对象注册表
     * @param run_loop IPC运行循环
     * @return DasResult 初始化结果
     */
    DasResult Initialize(
        DistributedObjectManager* object_manager,
        RemoteObjectRegistry*     object_registry,
        IpcRunLoop* run_loop      DAS_LIFETIMEBOUND = nullptr);

    /**
     * @brief 检查是否已初始化
     * @return 如果已初始化返回 true，否则返回 false
     */
    bool IsInitialized() const;

    /**
     * @brief 获取当前的 IPC 运行循环
     * @return IpcRunLoop* 运行循环指针，如果未初始化则返回 nullptr
     */
    IpcRunLoop* DAS_LIFETIMEBOUND GetRunLoop() const { return run_loop_; }

    /**
     * @brief 设置 IPC 运行循环
     * @param run_loop 运行循环指针
     * @return DasResult 操作结果
     */
    DasResult SetRunLoop(IpcRunLoop* run_loop);

    // 禁止拷贝和赋值
    ProxyFactory(const ProxyFactory&) = delete;
    ProxyFactory& operator=(const ProxyFactory&) = delete;

private:
    ProxyFactory() = default;
    ~ProxyFactory() = default;

    IPCProxyBase* CreateIPCProxy(const ObjectId& object_id);

    /**
     * @brief 验证对象是否存在且可访问
     * @param object_id 对象 ID
     * @param out_info 输出对象信息
     * @return DasResult 验证结果
     */
    DasResult ValidateObject(
        const ObjectId&   object_id,
        RemoteObjectInfo& out_info);

    /**
     * @brief 获取对象接口 ID
     * @param object_id 对象 ID
     * @return 对象的接口 ID
     * @throws DasException 当无法获取接口 ID 时抛出异常
     */
    uint32_t GetObjectInterfaceId(const ObjectId& object_id);

    // 分布式对象管理器
    DistributedObjectManager* object_manager_ = nullptr;

    // 远程对象注册表
    RemoteObjectRegistry* object_registry_ = nullptr;

    // IPC运行循环
    IpcRunLoop* run_loop_ = nullptr;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_PROXY_FACTORY_H
