#include <cstdint>
#include <das/Core/IPC/ObjectManager.h>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        struct DistributedObjectManager::Impl
        {
            std::unordered_map<ObjectId, RemoteObjectHandle> objects_;
            mutable std::shared_mutex                        objects_mutex_;
            uint16_t                               local_session_id_{0};
            uint32_t                               next_local_id_{1};
            std::unordered_map<uint32_t, uint16_t> local_id_generations_;
        };

        DistributedObjectManager::DistributedObjectManager()
            : impl_(std::make_unique<Impl>())
        {
        }

        DistributedObjectManager::~DistributedObjectManager() { Shutdown(); }

        DasResult DistributedObjectManager::Initialize(
            uint16_t local_session_id)
        {
            impl_->local_session_id_ = local_session_id;
            return DAS_S_OK;
        }

        DasResult DistributedObjectManager::Shutdown()
        {
            std::unique_lock<std::shared_mutex> lock(impl_->objects_mutex_);
            impl_->objects_.clear();
            return DAS_S_OK;
        }

        DasResult DistributedObjectManager::ValidateObjectId(
            const ObjectId& object_id)
        {
            if (IsNullObjectId(object_id))
            {
                return DAS_E_IPC_INVALID_OBJECT_ID;
            }

            return DAS_S_OK;
        }

        DasResult DistributedObjectManager::RegisterLocalObject(
            void*     object_ptr,
            ObjectId& out_object_id)
        {
            if (object_ptr == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }

            uint32_t local_id = impl_->next_local_id_++;

            uint16_t generation = 1;
            {
                std::unique_lock<std::shared_mutex> lock(impl_->objects_mutex_);
                auto it = impl_->local_id_generations_.find(local_id);
                if (it != impl_->local_id_generations_.end())
                {
                    generation = it->second;
                }
                else
                {
                    impl_->local_id_generations_[local_id] = generation;
                }
            }

            ObjectId obj_id{
                .session_id = impl_->local_session_id_,
                .generation = generation,
                .local_id = local_id};

            RemoteObjectHandle handle{
                .object_id = obj_id,
                .refcount = 1,
                .object_ptr = object_ptr,
                .is_local = true};

            {
                std::unique_lock<std::shared_mutex> lock(impl_->objects_mutex_);
                impl_->objects_[obj_id] = handle;
            }

            out_object_id = obj_id;
            return DAS_S_OK;
        }

        DasResult DistributedObjectManager::RegisterRemoteObject(
            const ObjectId& object_id)
        {
            auto result = ValidateObjectId(object_id);
            if (result != DAS_S_OK)
            {
                return result;
            }

            RemoteObjectHandle handle{
                .object_id = object_id,
                .refcount = 1,
                .object_ptr = nullptr,
                .is_local = false};

            {
                std::unique_lock<std::shared_mutex> lock(impl_->objects_mutex_);
                impl_->objects_[object_id] = handle;
            }

            return DAS_S_OK;
        }

        DasResult DistributedObjectManager::UnregisterObject(
            const ObjectId& object_id)
        {
            auto result = ValidateObjectId(object_id);
            if (result != DAS_S_OK)
            {
                return result;
            }

            std::unique_lock<std::shared_mutex> lock(impl_->objects_mutex_);
            auto it = impl_->objects_.find(object_id);
            if (it == impl_->objects_.end())
            {
                return DAS_E_IPC_OBJECT_NOT_FOUND;
            }

            if (it->second.is_local)
            {
                impl_->local_id_generations_[object_id.local_id] =
                    IncrementGeneration(object_id.generation);
            }

            impl_->objects_.erase(it);
            return DAS_S_OK;
        }

        DasResult DistributedObjectManager::AddRef(const ObjectId& object_id)
        {
            auto result = ValidateObjectId(object_id);
            if (result != DAS_S_OK)
            {
                return result;
            }

            std::unique_lock<std::shared_mutex> lock(impl_->objects_mutex_);
            auto it = impl_->objects_.find(object_id);
            if (it == impl_->objects_.end())
            {
                if (object_id.session_id == impl_->local_session_id_)
                {
                    auto gen_it =
                        impl_->local_id_generations_.find(object_id.local_id);
                    if (gen_it != impl_->local_id_generations_.end()
                        && gen_it->second != object_id.generation)
                    {
                        return DAS_E_IPC_STALE_OBJECT_HANDLE;
                    }
                }
                return DAS_E_IPC_OBJECT_NOT_FOUND;
            }

            it->second.refcount++;
            return DAS_S_OK;
        }

        DasResult DistributedObjectManager::Release(const ObjectId& object_id)
        {
            auto result = ValidateObjectId(object_id);
            if (result != DAS_S_OK)
            {
                return result;
            }

            std::unique_lock<std::shared_mutex> lock(impl_->objects_mutex_);
            auto it = impl_->objects_.find(object_id);
            if (it == impl_->objects_.end())
            {
                if (object_id.session_id == impl_->local_session_id_)
                {
                    auto gen_it =
                        impl_->local_id_generations_.find(object_id.local_id);
                    if (gen_it != impl_->local_id_generations_.end()
                        && gen_it->second != object_id.generation)
                    {
                        return DAS_E_IPC_STALE_OBJECT_HANDLE;
                    }
                }
                return DAS_E_IPC_OBJECT_NOT_FOUND;
            }

            it->second.refcount--;
            if (it->second.refcount == 0)
            {
                if (it->second.is_local)
                {
                    impl_->local_id_generations_[object_id.local_id] =
                        IncrementGeneration(object_id.generation);
                }
                impl_->objects_.erase(it);
            }

            return DAS_S_OK;
        }

        DasResult DistributedObjectManager::LookupObject(
            const ObjectId& object_id,
            void**          object_ptr)
        {
            if (object_ptr == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }

            auto result = ValidateObjectId(object_id);
            if (result != DAS_S_OK)
            {
                return result;
            }

            std::shared_lock<std::shared_mutex> lock(impl_->objects_mutex_);
            auto it = impl_->objects_.find(object_id);
            if (it == impl_->objects_.end())
            {
                if (object_id.session_id == impl_->local_session_id_)
                {
                    auto gen_it =
                        impl_->local_id_generations_.find(object_id.local_id);
                    if (gen_it != impl_->local_id_generations_.end()
                        && gen_it->second != object_id.generation)
                    {
                        return DAS_E_IPC_STALE_OBJECT_HANDLE;
                    }
                }
                return DAS_E_IPC_OBJECT_NOT_FOUND;
            }

            if (!it->second.is_local)
            {
                return DAS_E_IPC_INVALID_OBJECT_ID;
            }

            *object_ptr = it->second.object_ptr;
            return DAS_S_OK;
        }

        bool DistributedObjectManager::IsValidObject(
            const ObjectId& object_id) const
        {
            auto result = ValidateObjectId(object_id);
            if (result != DAS_S_OK)
            {
                return false;
            }

            std::shared_lock<std::shared_mutex> lock(impl_->objects_mutex_);
            if (impl_->objects_.find(object_id) != impl_->objects_.end())
            {
                return true;
            }

            if (object_id.session_id == impl_->local_session_id_)
            {
                auto gen_it =
                    impl_->local_id_generations_.find(object_id.local_id);
                if (gen_it != impl_->local_id_generations_.end()
                    && gen_it->second != object_id.generation)
                {
                    return false;
                }
            }

            return false;
        }

        bool DistributedObjectManager::IsLocalObject(
            const ObjectId& object_id) const
        {
            auto result = ValidateObjectId(object_id);
            if (result != DAS_S_OK)
            {
                return false;
            }

            if (object_id.session_id != impl_->local_session_id_)
            {
                return false;
            }

            std::shared_lock<std::shared_mutex> lock(impl_->objects_mutex_);
            auto it = impl_->objects_.find(object_id);
            if (it == impl_->objects_.end())
            {
                auto gen_it =
                    impl_->local_id_generations_.find(object_id.local_id);
                if (gen_it != impl_->local_id_generations_.end()
                    && gen_it->second != object_id.generation)
                {
                    return false;
                }
                return false;
            }

            return it->second.is_local;
        }
    }
}
DAS_NS_END
