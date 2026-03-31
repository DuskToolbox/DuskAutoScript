#include <cstdint>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/Logger/Logger.h>
#include <das/IDasBase.h>
#include <unordered_map>

DAS_CORE_IPC_NS_BEGIN

DistributedObjectManager::DistributedObjectManager() = default;

DistributedObjectManager::~DistributedObjectManager()
{
    // DasPtr 析构自动 Release 所有本地对象
    objects_.clear();
}

uint16_t DistributedObjectManager::GetLocalSessionId() const
{
    if (run_loop_)
    {
        return run_loop_->GetSessionId();
    }
    return 0;
}

DasResult DistributedObjectManager::RegisterLocalObject(
    IDasBase* object_ptr,
    ObjectId& out_object_id)
{
    if (object_ptr == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // 去重检查：同一指针是否已注册
    auto ptr_it = ptr_to_id_.find(object_ptr);
    if (ptr_it != ptr_to_id_.end())
    {
        const ObjectId& existing_id = ptr_it->second;
        auto            obj_it = objects_.find(existing_id);
        if (obj_it != objects_.end())
        {
            obj_it->second.ref_count_++;
            out_object_id = existing_id;
            return DAS_S_OK;
        }
        // Invariant violation: ptr_to_id_ and objects_ are out of sync
        DAS_CORE_LOG_ERROR(
            "ptr_to_id_ contains entry for pointer but objects_ does not, "
            "object_id = (session={}, gen={}, local={})",
            existing_id.session_id,
            existing_id.generation,
            existing_id.local_id);
        ptr_to_id_.erase(ptr_it);
        return DAS_E_IPC_INVALID_STATE;
    }

    // 首次注册：分配新 ObjectId
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

    // DasPtr 构造函数自动 AddRef
    ObjectEntry entry{
        .ref_count_ = 1,
        .object_id = obj_id,
        .object_ptr = DAS::DasPtr<IDasBase>(object_ptr),
        .is_local = true};

    objects_[obj_id] = entry;
    ptr_to_id_[object_ptr] = obj_id;

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
        .ref_count_ = 1,
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

    ObjectEntry& entry = it->second;
    bool         is_local = entry.is_local;

    if (is_local)
    {
        local_id_generations_[object_id.local_id] =
            IncrementGeneration(object_id.generation);
    }

    entry.ref_count_--;

    if (entry.ref_count_ == 0)
    {
        // 引用计数归零：擦除 ObjectEntry，DasPtr 析构自动 Release
        // 同时清理反向索引
        if (is_local && entry.object_ptr)
        {
            ptr_to_id_.erase(entry.object_ptr.Get());
        }
        // DasPtr 析构自动 Release
        objects_.erase(it);
    }

    return DAS_S_OK;
}

DasResult DistributedObjectManager::LookupObject(
    const ObjectId& object_id,
    IDasBase**      object_ptr)
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

    // COM 规范：通过输出参数返回引用计数对象必须 AddRef
    *object_ptr = it->second.object_ptr.Get();
    (*object_ptr)->AddRef();
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

DasResult DistributedObjectManager::LookupObjectIdFromPtr(
    IDasBase* ptr,
    ObjectId& out_id) const noexcept
{
    if (ptr == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    auto it = ptr_to_id_.find(ptr);
    if (it == ptr_to_id_.end())
    {
        return DAS_E_NOT_FOUND;
    }

    out_id = it->second;
    return DAS_S_OK;
}

DAS_CORE_IPC_NS_END
