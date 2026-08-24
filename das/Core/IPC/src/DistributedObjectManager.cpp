#include <cstdint>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/Logger/Logger.h>
#include <das/IDasBase.h>
#include <mutex>
#include <shared_mutex>
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
    return session_id_.load(std::memory_order_acquire);
}

DasResult DistributedObjectManager::RegisterLocalObject(
    IDasBase* object_ptr,
    ObjectId& out_object_id)
{
    return RegisterLocalObject(object_ptr, out_object_id, true);
}

DasResult DistributedObjectManager::RegisterLocalObject(
    IDasBase* object_ptr,
    ObjectId& out_object_id,
    bool      explicit_pin)
{
    if (object_ptr == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    const std::unique_lock lock{mutex_};

    // 去重检查：同一指针是否已注册
    auto ptr_it = ptr_to_id_.find(object_ptr);
    if (ptr_it != ptr_to_id_.end())
    {
        const ObjectId& existing_id = ptr_it->second;
        auto            obj_it = objects_.find(existing_id);
        if (obj_it != objects_.end())
        {
            if (explicit_pin)
            {
                obj_it->second.local_pins_++;
            }
            // pinless 对已存在条目为无记账 no-op，直接返回既有 oid
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

    // 首次注册：分配新 ObjectId（next_local_id_ 单调递增，gen 恒从 1 起）
    const uint32_t local_id = next_local_id_++;
    local_id_generations_[local_id] = 1;

    const ObjectId obj_id{
        .session_id = GetLocalSessionId(),
        .generation = 1,
        .local_id = local_id};

    // DasPtr 构造函数自动 AddRef；pinless 条目 local_pins_ 为 0（配额由
    // 对端 ADD_REF 填充），显式注册置 1
    const ObjectEntry entry{
        .local_pins_ = explicit_pin ? uint32_t{1} : uint32_t{0},
        .object_id = obj_id,
        .object_ptr = DAS::DasPtr<IDasBase>(object_ptr),
        .is_local = true,
        .holder_sessions_ = {},
        .remote_views_ = 0};

    objects_[obj_id] = entry;
    ptr_to_id_[object_ptr] = obj_id;

    out_object_id = obj_id;
    return DAS_S_OK;
}

DasResult DistributedObjectManager::RegisterRemoteObject(
    const ObjectId& object_id)
{
    bool newly_created = false;
    return RegisterRemoteObject(object_id, newly_created);
}

DasResult DistributedObjectManager::RegisterRemoteObject(
    const ObjectId& object_id,
    bool&           out_newly_created)
{
    auto result = ValidateObjectId(object_id);
    if (result != DAS_S_OK)
    {
        return result;
    }

    const std::unique_lock lock{mutex_};
    auto                   existing = objects_.find(object_id);
    if (existing != objects_.end())
    {
        if (existing->second.is_local)
        {
            return DAS_E_DUPLICATE_ELEMENT;
        }
        ++existing->second.remote_views_;
        out_newly_created = false;
        return DAS_S_OK;
    }

    const ObjectEntry entry{
        .local_pins_ = 0,
        .object_id = object_id,
        .object_ptr = nullptr,
        .is_local = false,
        .holder_sessions_ = {},
        .remote_views_ = 1};

    objects_[object_id] = entry;
    out_newly_created = true;

    return DAS_S_OK;
}

DasResult DistributedObjectManager::UnregisterObject(const ObjectId& object_id)
{
    bool entry_removed = false;
    return UnregisterObject(object_id, entry_removed);
}

DasResult DistributedObjectManager::UnregisterObject(
    const ObjectId& object_id,
    bool&           out_entry_removed)
{
    auto result = ValidateObjectId(object_id);
    if (result != DAS_S_OK)
    {
        return result;
    }

    out_entry_removed = false;
    DAS::DasPtr<IDasBase> object_to_release;

    {
        const std::unique_lock lock{mutex_};
        auto                   it = objects_.find(object_id);
        if (it == objects_.end())
        {
            return DAS_E_IPC_OBJECT_NOT_FOUND;
        }

        ObjectEntry& entry = it->second;

        if (!entry.is_local)
        {
            // 远程条目：peer 侧自有视图计数递减，归零移除
            if (entry.remote_views_ > 1)
            {
                --entry.remote_views_;
                return DAS_S_OK;
            }
            objects_.erase(it);
            out_entry_removed = true;
            return DAS_S_OK;
        }

        // 本地条目过渡语义：显式 pin 递减；无 pin 时旧
        // BUSINESS_CONTROL RELEASE（无 session 身份）只能吃任一 holder
        bool decremented = false;
        if (entry.local_pins_ > 0)
        {
            --entry.local_pins_;
            decremented = true;
        }
        else if (!entry.holder_sessions_.empty())
        {
            entry.holder_sessions_.erase(entry.holder_sessions_.begin());
            decremented = true;
            DAS_CORE_LOG_WARN(
                "UnregisterObject transitional path consumed a remote holder "
                "without session identity: object_id = (session={}, gen={}, "
                "local={})",
                object_id.session_id,
                object_id.generation,
                object_id.local_id);
        }

        // 移除条件（仅在递减发生时判定）：pins 归零且 holders 空
        if (!decremented || entry.local_pins_ > 0
            || !entry.holder_sessions_.empty())
        {
            return DAS_S_OK;
        }

        if (entry.object_ptr)
        {
            ptr_to_id_.erase(entry.object_ptr.Get());
            local_id_generations_[object_id.local_id] =
                IncrementGeneration(object_id.generation);
            object_to_release = std::move(entry.object_ptr);
        }

        objects_.erase(it);
        out_entry_removed = true;
    }

    // 出锁后析构（锁外 final Release，防死锁）
    return DAS_S_OK;
}

DasResult DistributedObjectManager::AddRemoteHolder(
    const ObjectId& object_id,
    uint16_t        session_id)
{
    auto result = ValidateObjectId(object_id);
    if (result != DAS_S_OK)
    {
        return result;
    }

    const std::unique_lock lock{mutex_};
    auto                   it = objects_.find(object_id);
    if (it == objects_.end())
    {
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }
    if (!it->second.is_local)
    {
        return DAS_E_IPC_INVALID_OBJECT_ID;
    }

    // set 语义：同一 (oid, session) 重复 ADD_REF 幂等，计数不涨
    it->second.holder_sessions_.insert(session_id);
    return DAS_S_OK;
}

DasResult DistributedObjectManager::ReleaseRemoteHolder(
    const ObjectId&        object_id,
    uint16_t               session_id,
    DAS::DasPtr<IDasBase>& out_object_to_release)
{
    auto result = ValidateObjectId(object_id);
    if (result != DAS_S_OK)
    {
        return result;
    }

    const std::unique_lock lock{mutex_};
    auto                   it = objects_.find(object_id);
    if (it == objects_.end())
    {
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }
    if (!it->second.is_local)
    {
        return DAS_E_IPC_INVALID_OBJECT_ID;
    }

    ObjectEntry& entry = it->second;
    if (entry.holder_sessions_.erase(session_id) == 0)
    {
        // 该 session 从不是 holder：无递减发生，不评估移除条件
        // （迟到/打空的 RELEASE，调用方按 known-edge 记 WARN 忽略）
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    // 移除条件（递减发生时判定）：无显式 pin 且无远端 holder
    if (entry.local_pins_ > 0 || !entry.holder_sessions_.empty())
    {
        return DAS_S_OK;
    }

    if (entry.object_ptr)
    {
        ptr_to_id_.erase(entry.object_ptr.Get());
        local_id_generations_[object_id.local_id] =
            IncrementGeneration(object_id.generation);
        out_object_to_release = std::move(entry.object_ptr);
    }

    objects_.erase(it);
    return DAS_S_OK;
}

DasResult DistributedObjectManager::UnregisterLocalPin(
    const ObjectId&        object_id,
    DAS::DasPtr<IDasBase>& out_object_to_release)
{
    auto result = ValidateObjectId(object_id);
    if (result != DAS_S_OK)
    {
        return result;
    }

    const std::unique_lock lock{mutex_};
    auto                   it = objects_.find(object_id);
    if (it == objects_.end())
    {
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }
    if (!it->second.is_local)
    {
        return DAS_E_IPC_INVALID_OBJECT_ID;
    }

    ObjectEntry& entry = it->second;

    // pin 递减（为 0 时无递减、幂等），仅在递减后评估移除条件
    bool decremented = false;
    if (entry.local_pins_ > 0)
    {
        --entry.local_pins_;
        decremented = true;
    }

    if (!decremented || entry.local_pins_ > 0
        || !entry.holder_sessions_.empty())
    {
        return DAS_S_OK;
    }

    if (entry.object_ptr)
    {
        ptr_to_id_.erase(entry.object_ptr.Get());
        local_id_generations_[object_id.local_id] =
            IncrementGeneration(object_id.generation);
        out_object_to_release = std::move(entry.object_ptr);
    }

    objects_.erase(it);
    return DAS_S_OK;
}

std::vector<DAS::DasPtr<IDasBase>>
DistributedObjectManager::RemoveSessionHolders(uint16_t session_id)
{
    std::vector<DAS::DasPtr<IDasBase>> removed_objects;

    const std::unique_lock lock{mutex_};
    for (auto it = objects_.begin(); it != objects_.end();)
    {
        ObjectEntry& entry = it->second;
        if (!entry.is_local || entry.holder_sessions_.erase(session_id) == 0)
        {
            ++it;
            continue;
        }

        // 只动 holder_sessions_，绝不动 local_pins_；清空后满足移除条件
        // （pins 为 0 且 holders 空）的条目回收
        if (entry.local_pins_ > 0 || !entry.holder_sessions_.empty())
        {
            ++it;
            continue;
        }

        if (entry.object_ptr)
        {
            ptr_to_id_.erase(entry.object_ptr.Get());
            local_id_generations_[entry.object_id.local_id] =
                IncrementGeneration(entry.object_id.generation);
            removed_objects.push_back(std::move(entry.object_ptr));
        }
        it = objects_.erase(it);
    }

    return removed_objects;
}

DasResult DistributedObjectManager::RemoveIfUnheldAutoEntry(
    const ObjectId&        object_id,
    DAS::DasPtr<IDasBase>& out_object_to_release)
{
    auto result = ValidateObjectId(object_id);
    if (result != DAS_S_OK)
    {
        return result;
    }

    const std::unique_lock lock{mutex_};
    auto                   it = objects_.find(object_id);
    if (it == objects_.end())
    {
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }
    if (!it->second.is_local)
    {
        return DAS_E_IPC_INVALID_OBJECT_ID;
    }

    ObjectEntry& entry = it->second;

    // holders 非空（对端已物化）或仍有显式 pin：不动条目
    if (entry.local_pins_ > 0 || !entry.holder_sessions_.empty())
    {
        return DAS_S_OK;
    }

    if (entry.object_ptr)
    {
        ptr_to_id_.erase(entry.object_ptr.Get());
        local_id_generations_[object_id.local_id] =
            IncrementGeneration(object_id.generation);
        out_object_to_release = std::move(entry.object_ptr);
    }

    objects_.erase(it);
    return DAS_S_OK;
}

size_t DistributedObjectManager::GetRemoteHolderCount(
    const ObjectId& object_id) const
{
    if (ValidateObjectId(object_id) != DAS_S_OK)
    {
        return 0;
    }

    const std::shared_lock lock{mutex_};
    auto                   it = objects_.find(object_id);
    if (it == objects_.end() || !it->second.is_local)
    {
        return 0;
    }
    return it->second.holder_sessions_.size();
}

size_t DistributedObjectManager::GetLocalEntryCount() const
{
    const std::shared_lock lock{mutex_};
    size_t                 local_entry_count = 0;
    for (const auto& [entry_object_id, entry] : objects_)
    {
        if (entry.is_local)
        {
            ++local_entry_count;
        }
    }
    return local_entry_count;
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

    const std::shared_lock lock{mutex_};
    auto                   it = objects_.find(object_id);
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
    if (*object_ptr == nullptr)
    {
        return DAS_E_IPC_INVALID_STATE;
    }
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

    const std::shared_lock lock{mutex_};
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

    const std::shared_lock lock{mutex_};
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

    const std::shared_lock lock{mutex_};
    auto                   it = ptr_to_id_.find(ptr);
    if (it == ptr_to_id_.end())
    {
        return DAS_E_NOT_FOUND;
    }

    out_id = it->second;
    return DAS_S_OK;
}

DAS_CORE_IPC_NS_END
