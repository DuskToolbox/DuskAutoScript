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
            std::unordered_map<uint64_t, RemoteObjectHandle> objects_;
            mutable std::shared_mutex                        objects_mutex_;
            uint16_t local_session_id_{0};
            uint32_t next_local_id_{1};
            // Track current generation for each local_id (for stale handle
            // detection)
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
            uint64_t  object_id,
            ObjectId& out_id) const
        {
            out_id = DecodeObjectId(object_id);

            if (IsNullObjectId(out_id))
            {
                return DAS_E_IPC_INVALID_OBJECT_ID;
            }

            return DAS_S_OK;
        }

        DasResult DistributedObjectManager::RegisterLocalObject(
            void*     object_ptr,
            uint64_t& out_object_id)
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

            uint64_t encoded_id = EncodeObjectId(obj_id);

            RemoteObjectHandle handle{
                .object_id = encoded_id,
                .refcount = 1,
                .object_ptr = object_ptr,
                .is_local = true};

            {
                std::unique_lock<std::shared_mutex> lock(impl_->objects_mutex_);
                impl_->objects_[encoded_id] = handle;
            }

            out_object_id = encoded_id;
            return DAS_S_OK;
        }

        DasResult DistributedObjectManager::RegisterRemoteObject(
            uint64_t object_id)
        {
            ObjectId obj_id = DecodeObjectId(object_id);

            if (IsNullObjectId(obj_id))
            {
                return DAS_E_IPC_INVALID_OBJECT_ID;
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

        DasResult DistributedObjectManager::UnregisterObject(uint64_t object_id)
        {
            ObjectId obj_id;
            auto     result = ValidateObjectId(object_id, obj_id);
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
                impl_->local_id_generations_[obj_id.local_id] =
                    IncrementGeneration(obj_id.generation);
            }

            impl_->objects_.erase(it);
            return DAS_S_OK;
        }

        DasResult DistributedObjectManager::AddRef(uint64_t object_id)
        {
            ObjectId obj_id;
            auto     result = ValidateObjectId(object_id, obj_id);
            if (result != DAS_S_OK)
            {
                return result;
            }

            std::unique_lock<std::shared_mutex> lock(impl_->objects_mutex_);
            auto it = impl_->objects_.find(object_id);
            if (it == impl_->objects_.end())
            {
                if (obj_id.session_id == impl_->local_session_id_)
                {
                    auto gen_it =
                        impl_->local_id_generations_.find(obj_id.local_id);
                    if (gen_it != impl_->local_id_generations_.end()
                        && gen_it->second != obj_id.generation)
                    {
                        return DAS_E_IPC_STALE_OBJECT_HANDLE;
                    }
                }
                return DAS_E_IPC_OBJECT_NOT_FOUND;
            }

            it->second.refcount++;
            return DAS_S_OK;
        }

        DasResult DistributedObjectManager::Release(uint64_t object_id)
        {
            ObjectId obj_id;
            auto     result = ValidateObjectId(object_id, obj_id);
            if (result != DAS_S_OK)
            {
                return result;
            }

            std::unique_lock<std::shared_mutex> lock(impl_->objects_mutex_);
            auto it = impl_->objects_.find(object_id);
            if (it == impl_->objects_.end())
            {
                if (obj_id.session_id == impl_->local_session_id_)
                {
                    auto gen_it =
                        impl_->local_id_generations_.find(obj_id.local_id);
                    if (gen_it != impl_->local_id_generations_.end()
                        && gen_it->second != obj_id.generation)
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
                    impl_->local_id_generations_[obj_id.local_id] =
                        IncrementGeneration(obj_id.generation);
                }
                impl_->objects_.erase(it);
            }

            return DAS_S_OK;
        }

        DasResult DistributedObjectManager::LookupObject(
            uint64_t object_id,
            void**   object_ptr)
        {
            if (object_ptr == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }

            ObjectId obj_id;
            auto     result = ValidateObjectId(object_id, obj_id);
            if (result != DAS_S_OK)
            {
                return result;
            }

            std::shared_lock<std::shared_mutex> lock(impl_->objects_mutex_);
            auto it = impl_->objects_.find(object_id);
            if (it == impl_->objects_.end())
            {
                if (obj_id.session_id == impl_->local_session_id_)
                {
                    auto gen_it =
                        impl_->local_id_generations_.find(obj_id.local_id);
                    if (gen_it != impl_->local_id_generations_.end()
                        && gen_it->second != obj_id.generation)
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

        bool DistributedObjectManager::IsValidObject(uint64_t object_id) const
        {
            ObjectId obj_id;
            auto     result = ValidateObjectId(object_id, obj_id);
            if (result != DAS_S_OK)
            {
                return false;
            }

            std::shared_lock<std::shared_mutex> lock(impl_->objects_mutex_);
            if (impl_->objects_.find(object_id) != impl_->objects_.end())
            {
                return true;
            }

            if (obj_id.session_id == impl_->local_session_id_)
            {
                auto gen_it =
                    impl_->local_id_generations_.find(obj_id.local_id);
                if (gen_it != impl_->local_id_generations_.end()
                    && gen_it->second != obj_id.generation)
                {
                    return false;
                }
            }

            return false;
        }

        bool DistributedObjectManager::IsLocalObject(uint64_t object_id) const
        {
            ObjectId obj_id;
            auto     result = ValidateObjectId(object_id, obj_id);
            if (result != DAS_S_OK)
            {
                return false;
            }

            if (obj_id.session_id != impl_->local_session_id_)
            {
                return false;
            }

            std::shared_lock<std::shared_mutex> lock(impl_->objects_mutex_);
            auto it = impl_->objects_.find(object_id);
            if (it == impl_->objects_.end())
            {
                auto gen_it =
                    impl_->local_id_generations_.find(obj_id.local_id);
                if (gen_it != impl_->local_id_generations_.end()
                    && gen_it->second != obj_id.generation)
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
