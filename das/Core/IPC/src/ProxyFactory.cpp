#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ObjectManager.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/Logger/Logger.h>
#include <das/IDasBase.h>

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

            if (run_loop_ && run_loop_ != run_loop)
            {
                ClearAllProxies();
            }

            run_loop_ = run_loop;
            return DAS_S_OK;
        }

        IPCProxyBase* ProxyFactory::GetProxy(const ObjectId& object_id)
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
                IPCProxyBase* proxy = it->second.proxy;
                proxy_cache_.erase(it);
                proxy->Release();
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
                proxy_cache_.erase(it);

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

            for (const auto& entry : proxy_cache_)
            {
                if (object_manager_)
                {
                    object_manager_->Release(DecodeObjectId(entry.first));
                }
                if (entry.second.proxy)
                {
                    entry.second.proxy->Release();
                }
            }

            proxy_cache_.clear();
        }

        IPCProxyBase* ProxyFactory::CreateIPCProxy(const ObjectId& object_id)
        {
            uint64_t encoded_id = EncodeObjectId(object_id);

            RemoteObjectInfo info;
            DasResult        result = ValidateObject(object_id, info);
            if (result != DAS_S_OK)
            {
                return nullptr;
            }

            auto it = proxy_cache_.find(encoded_id);
            if (it != proxy_cache_.end())
            {
                if (object_manager_)
                {
                    object_manager_->AddRef(DecodeObjectId(encoded_id));
                }
                return it->second.proxy;
            }

            try
            {
                auto* proxy =
                    new GenericProxy(info.interface_id, object_id, run_loop_);

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

            DasResult result =
                object_registry_->GetObjectInfo(object_id, out_info);
            if (result != DAS_S_OK)
            {
                return result;
            }

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

            return info.interface_id;
        }

    }
}
DAS_NS_END