#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ObjectManager.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/Logger/Logger.h>
#include <das/IDasBase.h>
#include <memory>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        ProxyFactory& ProxyFactory::GetInstance()
        {
            static ProxyFactory instance;
            return instance;
        }

        DasResult ProxyFactory::Initialize(
            DistributedObjectManager* object_manager,
            RemoteObjectRegistry*     object_registry,
            IpcRunLoop*               run_loop)
        {
            if (!object_manager || !object_registry)
            {
                return DAS_E_FAIL;
            }

            std::lock_guard<std::mutex> lock(proxy_cache_mutex_);

            // 设置对象管理器、注册表和运行循环
            object_manager_ = object_manager;
            object_registry_ = object_registry;
            run_loop_ = run_loop;

            return DAS_S_OK;
        }

        bool ProxyFactory::IsInitialized() const
        {
            return (object_manager_ != nullptr)
                   && (object_registry_ != nullptr);
        }

        DasResult ProxyFactory::SetRunLoop(IpcRunLoop* run_loop)
        {
            std::lock_guard<std::mutex> lock(proxy_cache_mutex_);

            // 如果已经存在运行循环，先清理现有代理
            if (run_loop_ && run_loop_ != run_loop)
            {
                ClearAllProxies();
            }

            run_loop_ = run_loop;
            return DAS_S_OK;
        }

        std::shared_ptr<IPCProxyBase> ProxyFactory::GetProxy(
            const ObjectId& object_id)
        {
            std::lock_guard<std::mutex> lock(proxy_cache_mutex_);
            auto it = proxy_cache_.find(EncodeObjectId(object_id));
            if (it != proxy_cache_.end())
            {
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
                // 从缓存中移除Proxy
                proxy_cache_.erase(it);

                // 减少对象的引用计数（即使失败也不影响缓存清理）
                if (object_manager_)
                {
                    object_manager_->Release(object_id);
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

            // 释放所有对象的引用
            for (const auto& entry : proxy_cache_)
            {
                if (object_manager_)
                {
                    object_manager_->Release(DecodeObjectId(entry.first));
                }
            }

            proxy_cache_.clear();
        }

        std::shared_ptr<IPCProxyBase> ProxyFactory::CreateIPCProxy(
            const ObjectId& object_id)
        {
            uint64_t encoded_id = EncodeObjectId(object_id);

            // 验证对象信息
            RemoteObjectInfo info;
            DasResult        result = ValidateObject(object_id, info);
            if (result != DAS_S_OK)
            {
                return nullptr;
            }

            // 检查是否已存在该对象的代理
            auto it = proxy_cache_.find(encoded_id);
            if (it != proxy_cache_.end())
            {
                // 增加引用计数
                if (object_manager_)
                {
                    object_manager_->AddRef(DecodeObjectId(encoded_id));
                }
                return it->second.proxy;
            }

            // 创建新的代理实例
            try
            {
                // 这里需要根据实际的接口类型创建相应的代理
                // 使用 FNV-1a hash 的 interface_id
                auto proxy = std::make_shared<Proxy<void>>(
                    info.interface_id,
                    object_id,
                    run_loop_);

                // 缓存代理信息
                ProxyEntry entry;
                entry.proxy = proxy;
                entry.object_id_encoded = encoded_id;
                entry.interface_id = info.interface_id;
                entry.session_id = object_id.session_id;

                proxy_cache_[encoded_id] = entry;

                return proxy;
            }
            catch (const std::exception&)
            {
                // 暂时注释掉日志，避免编译错误
                // DAS_LOG_ERROR("Failed to create IPC proxy for object {}: {}",
                //     encoded_id, e.what());
                return nullptr;
            }
        }

        DasResult ProxyFactory::ValidateObject(
            const ObjectId&   object_id,
            RemoteObjectInfo& out_info)
        {
            if (!object_registry_)
            {
                return DAS_E_IPC_INVALID_STATE;
            }

            // 获取对象信息
            DasResult result =
                object_registry_->GetObjectInfo(object_id, out_info);
            if (result != DAS_S_OK)
            {
                return result;
            }

            // 验证对象是否仍然存在
            if (!object_registry_->ObjectExists(object_id))
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

            // 返回接口ID (FNV-1a hash)
            return info.interface_id;
        }

    }
}
DAS_NS_END