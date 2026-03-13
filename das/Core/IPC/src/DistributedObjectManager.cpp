#include <cstdint>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <memory>
#include <unordered_map>

DAS_CORE_IPC_NS_BEGIN

DistributedObjectManager::DistributedObjectManager() = default;

DistributedObjectManager::~DistributedObjectManager() { objects_.clear(); }

DasResult DistributedObjectManager::RegisterLocalObject(
    void*     object_ptr,
    ObjectId& out_object_id)
{
    if (object_ptr == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    uint32_t local_id = next_local_id_++;

    uint16_t generation = 1;
    auto     it = local_id_generations_.find(local_id);
    if (it != local_id_generations_.end())
    {
        generation = it->second;
    }
    else
    {
        local_id_generations_[local_id] = generation;
    }

    ObjectId obj_id{
        .session_id = *SessionCoordinator::GetInstance()
                           .GetLocalSessionId(), // 直接解引用，未初始化会崩溃
        .generation = generation,
        .local_id = local_id};

    RemoteObjectHandle handle{
        .object_id = obj_id,
        .local_refcount = 1,
        .remote_refcount = 0,
        .object_ptr = object_ptr,
        .is_local = true};

    objects_[obj_id] = handle;

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
        .local_refcount = 0,
        .remote_refcount = 1,
        .object_ptr = nullptr,
        .is_local = false};

    objects_[object_id] = handle;

    return DAS_S_OK;
}

DasResult DistributedObjectManager::UnregisterObject(const ObjectId& object_id)
{
    auto result = ValidateObjectId(object_id);
    if (result != DAS_S_OK)
    {
        return result;
    }

    auto it = objects_.find(object_id);
    if (it == objects_.end())
    {
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    if (it->second.is_local)
    {
        local_id_generations_[object_id.local_id] =
            IncrementGeneration(object_id.generation);
    }

    objects_.erase(it);
    return DAS_S_OK;
}

DasResult DistributedObjectManager::AddRef(const ObjectId& object_id)
{
    auto result = ValidateObjectId(object_id);
    if (result != DAS_S_OK)
    {
        return result;
    }

    auto it = objects_.find(object_id);
    if (it == objects_.end())
    {
        if (object_id.session_id
            == *SessionCoordinator::GetInstance().GetLocalSessionId())
        {
            auto gen_it = local_id_generations_.find(object_id.local_id);
            if (gen_it != local_id_generations_.end()
                && gen_it->second != object_id.generation)
            {
                return DAS_E_IPC_STALE_OBJECT_HANDLE;
            }
        }
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    it->second.local_refcount++;
    return DAS_S_OK;
}

DasResult DistributedObjectManager::Release(const ObjectId& object_id)
{
    auto result = ValidateObjectId(object_id);
    if (result != DAS_S_OK)
    {
        return result;
    }

    auto it = objects_.find(object_id);
    if (it == objects_.end())
    {
        if (object_id.session_id
            == *SessionCoordinator::GetInstance().GetLocalSessionId())
        {
            auto gen_it = local_id_generations_.find(object_id.local_id);
            if (gen_it != local_id_generations_.end()
                && gen_it->second != object_id.generation)
            {
                return DAS_E_IPC_STALE_OBJECT_HANDLE;
            }
        }
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    it->second.local_refcount--;
    // 只有当 local + remote 都为 0 时才销毁
    if (it->second.local_refcount == 0 && it->second.remote_refcount == 0)
    {
        if (it->second.is_local)
        {
            local_id_generations_[object_id.local_id] =
                IncrementGeneration(object_id.generation);
        }
        objects_.erase(it);
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

    auto it = objects_.find(object_id);
    if (it == objects_.end())
    {
        if (object_id.session_id
            == *SessionCoordinator::GetInstance().GetLocalSessionId())
        {
            auto gen_it = local_id_generations_.find(object_id.local_id);
            if (gen_it != local_id_generations_.end()
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

bool DistributedObjectManager::IsValidObject(const ObjectId& object_id) const
{
    auto result = ValidateObjectId(object_id);
    if (result != DAS_S_OK)
    {
        return false;
    }

    if (objects_.find(object_id) != objects_.end())
    {
        return true;
    }

    if (object_id.session_id
        == *SessionCoordinator::GetInstance().GetLocalSessionId())
    {
        auto gen_it = local_id_generations_.find(object_id.local_id);
        if (gen_it != local_id_generations_.end()
            && gen_it->second != object_id.generation)
        {
            return false;
        }
    }

    return false;
}

bool DistributedObjectManager::IsLocalObject(const ObjectId& object_id) const
{
    auto result = ValidateObjectId(object_id);
    if (result != DAS_S_OK)
    {
        return false;
    }

    if (object_id.session_id
        != *SessionCoordinator::GetInstance().GetLocalSessionId())
    {
        return false;
    }

    auto it = objects_.find(object_id);
    if (it == objects_.end())
    {
        auto gen_it = local_id_generations_.find(object_id.local_id);
        if (gen_it != local_id_generations_.end()
            && gen_it->second != object_id.generation)
        {
            return false;
        }
        return false;
    }

    return it->second.is_local;
}

DasResult DistributedObjectManager::ValidateObjectId(const ObjectId& object_id)
{
    if (IsNullObjectId(object_id))
    {
        return DAS_E_IPC_INVALID_OBJECT_ID;
    }
    return DAS_S_OK;
}

DasResult DistributedObjectManager::HandleRemoteAddRef(
    const ObjectId& object_id)
{
    auto result = ValidateObjectId(object_id);
    if (result != DAS_S_OK)
    {
        return result;
    }

    auto it = objects_.find(object_id);
    if (it == objects_.end())
    {
        if (object_id.session_id
            == *SessionCoordinator::GetInstance().GetLocalSessionId())
        {
            auto gen_it = local_id_generations_.find(object_id.local_id);
            if (gen_it != local_id_generations_.end()
                && gen_it->second != object_id.generation)
            {
                return DAS_E_IPC_STALE_OBJECT_HANDLE;
            }
        }
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    it->second.remote_refcount++;
    return DAS_S_OK;
}

DasResult DistributedObjectManager::HandleRemoteRelease(
    const ObjectId& object_id)
{
    auto result = ValidateObjectId(object_id);
    if (result != DAS_S_OK)
    {
        return result;
    }

    auto it = objects_.find(object_id);
    if (it == objects_.end())
    {
        if (object_id.session_id
            == *SessionCoordinator::GetInstance().GetLocalSessionId())
        {
            auto gen_it = local_id_generations_.find(object_id.local_id);
            if (gen_it != local_id_generations_.end()
                && gen_it->second != object_id.generation)
            {
                return DAS_E_IPC_STALE_OBJECT_HANDLE;
            }
        }
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    if (it->second.remote_refcount == 0)
    {
        return DAS_E_FAIL;
    }

    it->second.remote_refcount--;

    // 只有当 local + remote 都为 0 时才销毁
    if (it->second.local_refcount == 0 && it->second.remote_refcount == 0)
    {
        if (it->second.is_local)
        {
            local_id_generations_[object_id.local_id] =
                IncrementGeneration(object_id.generation);
        }
        objects_.erase(it);
    }

    return DAS_S_OK;
}

DAS_CORE_IPC_NS_END
