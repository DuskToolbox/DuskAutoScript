#include <cstdint>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <memory>
#include <unordered_map>

DAS_CORE_IPC_NS_BEGIN

DistributedObjectManager::DistributedObjectManager() = default;

DistributedObjectManager::~DistributedObjectManager() { objects_.clear(); }

uint16_t DistributedObjectManager::GetLocalSessionId() const
{
    if (run_loop_)
    {
        return run_loop_->GetSessionId();
    }
    return 0;
}

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
        .session_id = GetLocalSessionId(),
        .generation = generation,
        .local_id = local_id};

    ObjectEntry entry{
        .object_id = obj_id,
        .object_ptr = object_ptr,
        .is_local = true};

    objects_[obj_id] = entry;

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

    ObjectEntry entry{
        .object_id = object_id,
        .object_ptr = nullptr,
        .is_local = false};

    objects_[object_id] = entry;

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

    // Save before erase
    void* object_ptr = it->second.object_ptr;
    bool  is_local = it->second.is_local;

    if (is_local)
    {
        local_id_generations_[object_id.local_id] =
            IncrementGeneration(object_id.generation);
    }

    objects_.erase(it);

    // Release COM object for local objects (balances Stub's AddRef)
    if (is_local && object_ptr != nullptr)
    {
        static_cast<IDasBase*>(object_ptr)->Release();
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
        if (object_id.session_id == GetLocalSessionId())
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

    if (object_id.session_id == GetLocalSessionId())
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

    if (object_id.session_id != GetLocalSessionId())
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

DAS_CORE_IPC_NS_END
