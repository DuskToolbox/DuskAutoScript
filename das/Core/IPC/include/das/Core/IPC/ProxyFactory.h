#ifndef DAS_CORE_IPC_PROXY_FACTORY_H
#define DAS_CORE_IPC_PROXY_FACTORY_H

#include <atomic>
#include <das/Core/IPC/IPCProxyBase.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/ObjectManager.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
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
             * @param run_loop IPC运行循环
             * @return DasResult 初始化结果
             */
            DasResult Initialize(
                DistributedObjectManager* object_manager,
                RemoteObjectRegistry*     object_registry,
                IpcRunLoop*               run_loop = nullptr);

            /**
             * @brief 检查是否已初始化
             * @return 如果已初始化返回 true，否则返回 false
             */
            bool IsInitialized() const;

            /**
             * @brief 获取当前的 IPC 运行循环
             * @return IpcRunLoop* 运行循环指针，如果未初始化则返回 nullptr
             */
            IpcRunLoop* GetRunLoop() const { return run_loop_; }

            /**
             * @brief 设置 IPC 运行循环
             * @param run_loop 运行循环指针
             * @return DasResult 操作结果
             */
            DasResult SetRunLoop(IpcRunLoop* run_loop);

            /**
             * @brief 创建指定类型的 Proxy 实例
 *
             * @tparam T 要创建的 Proxy 类型
             * @param
             * object_id 对象 ID
             * @return DasPtr<T> Proxy
             * 实例的智能指针

             */
            template <typename T>
            DasPtr<T> CreateProxy(const ObjectId& object_id)
            {
                uint64_t encoded_id = EncodeObjectId(object_id);

                if (!object_registry_ || !IsInitialized())
                {
                    return nullptr;
                }

                {
                    std::lock_guard<std::mutex> lock(proxy_cache_mutex_);
                    auto it = proxy_cache_.find(encoded_id);
                    if (it != proxy_cache_.end())
                    {
                        auto* proxy = static_cast<Proxy<T>*>(it->second.proxy);
                        proxy->AddRef();
                        return DasPtr<T>(proxy);
                    }
                }

                RemoteObjectInfo info;
                DasResult        result = ValidateObject(object_id, info);
                if (result != DAS_S_OK)
                {
                    return nullptr;
                }

                try
                {
                    uint32_t interface_id = info.interface_id;

                    auto* proxy =
                        new Proxy<T>(interface_id, object_id, run_loop_);

                    {
                        std::lock_guard<std::mutex> lock(proxy_cache_mutex_);
                        ProxyEntry                  entry;
                        entry.proxy = proxy;
                        entry.object_id_encoded = encoded_id;
                        entry.interface_id = interface_id;
                        entry.session_id = object_id.session_id;

                        proxy_cache_[encoded_id] = entry;
                    }

                    if (object_manager_)
                    {
                        object_manager_->AddRef(object_id);
                    }

                    return DasPtr<T>(proxy);
                }
                catch (const std::exception&)
                {
                    return nullptr;
                }
            }

            /**
             * @brief 获取现有的 Proxy 实例
 *
             * @param object_id 对象 ID
             * @return 对应的 Proxy
             * 实例，如果不存在则返回 nullptr
             */
            IPCProxyBase* GetProxy(const ObjectId& object_id);

            /**
             * @brief 释放 Proxy 实例
             * @param
             * object_id 对象 ID
             * @return DasResult
             * 操作结果

             */
            DasResult ReleaseProxy(const ObjectId& object_id);

            /**
             * @brief 从缓存中移除
             * Proxy（内部方法，供
             * Proxy::Release 调用）
             * @param object_id 对象
             * ID

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

            // 内部数据结构：Proxy 缓存
            struct ProxyEntry
            {
                IPCProxyBase* proxy; // 使用原始指针，生命周期由引用计数管理
                uint64_t      object_id_encoded;
                uint32_t      interface_id;
                uint16_t      session_id;
            };

            // 缓存已创建的 Proxy
            std::unordered_map<uint64_t, ProxyEntry> proxy_cache_;

            // 互斥锁保护代理缓存
            mutable std::mutex proxy_cache_mutex_;

            // 分布式对象管理器
            DistributedObjectManager* object_manager_ = nullptr;

            // 远程对象注册表
            RemoteObjectRegistry* object_registry_ = nullptr;

            // IPC运行循环
            IpcRunLoop* run_loop_ = nullptr;
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
                : IPCProxyBase(interface_id, object_id, run_loop), ref_count_(1)
            {
            }

            ~Proxy() override = default;

            uint32_t AddRef() { return ++ref_count_; }

            uint32_t Release()
            {
                uint32_t new_count = --ref_count_;
                if (new_count == 0)
                {
                    ObjectId obj_id = GetObjectIdStruct();
                    ProxyFactory::GetInstance().RemoveFromCache(obj_id);
                    delete this;
                }
                return new_count;
            }

            // 禁止拷贝
            Proxy(const Proxy&) = delete;
            Proxy& operator=(const Proxy&) = delete;

            // 禁止移动（因为引用计数不支持移动语义）
            Proxy(Proxy&&) = delete;
            Proxy& operator=(Proxy&&) = delete;

            /**
             * @brief 获取原始接口指针
             *
             * @return T* 接口指针
             */
            T* Get() const noexcept
            {
                if (!GetRunLoop())
                {
                    return nullptr;
                }
                return nullptr;
            }

            /**
             * @brief 检查 Proxy 是否有效
             *
             * @return 如果有效返回 true，否则返回 false
 */
            bool IsValid() const noexcept
            {
                return GetObjectId() != 0 && GetRunLoop() != nullptr;
            }

            DasResult CallRemoteMethod(
                uint16_t                    method_id,
                const std::vector<uint8_t>& request_body,
                std::vector<uint8_t>&       response_body)
            {
                if (!GetRunLoop())
                {
                    return DAS_E_FAIL;
                }

                return SendRequest(
                    method_id,
                    request_body.data(),
                    request_body.size(),
                    response_body);
            }

        private:
            std::atomic<uint32_t> ref_count_;
        };

    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_PROXY_FACTORY_H