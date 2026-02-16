#ifndef DAS_CORE_IPC_PROXY_FACTORY_H
#define DAS_CORE_IPC_PROXY_FACTORY_H

#include <das/Core/IPC/IPCProxyBase.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/ObjectManager.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/Logger/Logger.h>
#include <das/IDasBase.h>
#include <memory>
#include <mutex>
#include <unordered_map>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        // 前置声明
        template <typename T>
        class Proxy;

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
             * @return DasResult 初始化结果
             */
            DasResult Initialize(
                DistributedObjectManager* object_manager,
                RemoteObjectRegistry*     object_registry);

            /**
             * @brief 检查是否已初始化
             * @return 如果已初始化返回 true，否则返回 false
             */
            bool IsInitialized() const;

            /**
             * @brief 创建指定类型的 Proxy 实例
             * @tparam T 要创建的 Proxy 类型
             * @param object_id 对象 ID
             * @return Proxy 实例的共享指针
             * @throws DasException 当无法创建 Proxy 时抛出异常
             */
            template <typename T>
            std::shared_ptr<Proxy<T>> CreateProxy(const ObjectId& object_id)
            {
                uint64_t encoded_id = EncodeObjectId(object_id);

                // 验证对象是否存在
                if (!object_registry_
                    || !object_registry_->ObjectExists(object_id))
                {
                    return nullptr;
                }

                // 检查是否已存在该对象的代理
                {
                    std::lock_guard<std::mutex> lock(proxy_cache_mutex_);
                    auto it = proxy_cache_.find(encoded_id);
                    if (it != proxy_cache_.end())
                    {
                        // 已存在，增加引用计数并返回类型转换后的代理
                        if (object_manager_)
                        {
                            object_manager_->AddRef(encoded_id);
                        }
                        return std::static_pointer_cast<Proxy<T>>(
                            it->second.proxy);
                    }
                }

                // 创建新的代理
                try
                {
                    // 获取对象的接口 ID
                    uint32_t interface_id = GetObjectInterfaceId(object_id);

                    // 创建代理实例（这里需要实际的 IpcRunLoop）
                    auto proxy = std::make_shared<Proxy<T>>(
                        interface_id,
                        object_id,
                        nullptr);

                    // 缓存代理信息
                    std::lock_guard<std::mutex> lock(proxy_cache_mutex_);
                    ProxyEntry                  entry;
                    entry.proxy = proxy;
                    entry.object_id_encoded = encoded_id;
                    entry.interface_id = interface_id;
                    entry.session_id = object_id.session_id;

                    proxy_cache_[encoded_id] = entry;

                    // 增加对象引用计数
                    if (object_manager_)
                    {
                        object_manager_->AddRef(encoded_id);
                    }

                    return proxy;
                }
                catch (const std::exception&)
                {
                    return nullptr;
                }
            }

            /**
             * @brief 获取现有的 Proxy 实例
             * @param object_id 对象 ID
             * @return 对应的 Proxy 实例，如果不存在则返回 nullptr
             */
            std::shared_ptr<IPCProxyBase> GetProxy(const ObjectId& object_id);

            /**
             * @brief 释放 Proxy 实例
             * @param object_id 对象 ID
             * @return DasResult 操作结果
             */
            DasResult ReleaseProxy(const ObjectId& object_id);

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
            // 私有构造函数（单例模式）
            ProxyFactory() = default;
            ~ProxyFactory() = default;

            /**
             * @brief 创建 IPCProxyBase 实例
             * @param object_id 对象 ID
             * @return IPCProxyBase 实例的共享指针
             * @throws DasException 当无法创建 Proxy 时抛出异常
             */
            std::shared_ptr<IPCProxyBase> CreateIPCProxy(
                const ObjectId& object_id);

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

            // 内部数据结构：Proxy 缓存
            struct ProxyEntry
            {
                std::shared_ptr<IPCProxyBase> proxy;
                uint64_t                      object_id_encoded;
                uint32_t                      interface_id;
                uint16_t                      session_id;
            };

            // 缓存已创建的 Proxy
            std::unordered_map<uint64_t, ProxyEntry> proxy_cache_;

            // 互斥锁保护代理缓存
            mutable std::mutex proxy_cache_mutex_;

            // 分布式对象管理器
            DistributedObjectManager* object_manager_ = nullptr;

            // 远程对象注册表
            RemoteObjectRegistry* object_registry_ = nullptr;
        };

        /**
         * @brief Proxy 模板类
         * @tparam T 接口类型
         */
        template <typename T>
        class Proxy : public IPCProxyBase
        {
        public:
            explicit Proxy(
                uint32_t        interface_id,
                const ObjectId& object_id,
                IpcRunLoop*     run_loop)
                : IPCProxyBase(interface_id, object_id, run_loop)
            {
            }

            ~Proxy() override = default;

            // 禁止拷贝
            Proxy(const Proxy&) = delete;
            Proxy& operator=(const Proxy&) = delete;

            // 允许移动
            Proxy(Proxy&&) = default;
            Proxy& operator=(Proxy&&) = default;

            /**
             * @brief 获取原始接口指针
             * @return T* 接口指针
             */
            T* Get() const noexcept
            {
                // 这里需要根据实际的 IPC 机制来实现
                // 暂时返回 nullptr，实际实现需要根据 IPC 协议来调用远程方法
                return nullptr;
            }

            /**
             * @brief 转换为智能指针
             * @return std::shared_ptr<T>
             */
            std::shared_ptr<T> AsSharedPtr() const
            {
                return std::shared_ptr<T>(Get(), [](T*) {}); // 不拥有所有权
            }

            /**
             * @brief 检查 Proxy 是否有效
             * @return 如果有效返回 true，否则返回 false
             */
            bool IsValid() const noexcept
            {
                return GetObjectId() != 0; // 检查对象 ID 是否为空
            }
        };

    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_PROXY_FACTORY_H