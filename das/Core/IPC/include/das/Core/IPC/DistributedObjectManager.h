#ifndef DAS_CORE_IPC_DISTRIBUTED_OBJECT_MANAGER_H
#define DAS_CORE_IPC_DISTRIBUTED_OBJECT_MANAGER_H

#include <atomic>
#include <cassert>
#include <cstdint>
#include <das/Core/IPC/IDistributedObjectManager.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

struct ObjectEntry
{
    uint32_t              ref_count_{1}; // 引用计数（注册次数）
    ObjectId              object_id;
    DAS::DasPtr<IDasBase> object_ptr; // 本地对象非空，远程对象为 nullptr
    bool                  is_local;   // 决定 UnregisterObject 时是否 Release

    // 远端 holder 账本：每对端 0/1（单 proxy 不变量），holder 身份为
    // 对端 session_id，由 CONTROL_PLANE RemoteAddRef/RemoteRelease 维护
    std::unordered_set<uint16_t> holder_sessions_;
};

// 本地对象的受限句柄：仅 DistributedObjectManager::WithLocalObject 在持锁
// 期间发给回调，只暴露合法的引用计数/取值操作——既避免调用方反复加锁，
// 又不泄露 ObjectEntry 内部。
class LocalObjectRef
{
    ObjectEntry& entry_;
    explicit LocalObjectRef(ObjectEntry& e) noexcept : entry_{e} {}
    friend class DistributedObjectManager;

public:
    // 引用计数 +1（配平远端 proxy 销毁时的 REMOTE_RELEASE）。
    void AddRef() noexcept { entry_.ref_count_++; }

    [[nodiscard]]
    IDasBase* Get() const noexcept
    {
        return entry_.object_ptr.Get();
    }
};

// DistributedObjectManager is a free-threaded runtime table. It only tracks
// ObjectId bookkeeping; real object execution ownership remains elsewhere.
class DistributedObjectManager final : public IDistributedObjectManager
{
public:
    DistributedObjectManager();
    ~DistributedObjectManager();

    void SetSessionId(uint16_t session_id) noexcept
    {
        session_id_.store(session_id, std::memory_order_release);
    }

    void SetBusinessThreadId(std::thread::id id) { business_thread_id_ = id; }

    DasResult RegisterLocalObject(IDasBase* object_ptr, ObjectId& out_object_id)
        override;
    DasResult RegisterRemoteObject(const ObjectId& object_id) override;
    DasResult UnregisterObject(const ObjectId& object_id) override;

    /// @brief 注册本地对象（显式 pin / pinless 双模式重载）
    /// @param object_ptr 本地对象
    /// @param out_object_id [out] 分配或复用的 ObjectId
    /// @param explicit_pin true = 显式 Register/Unregister 配对（重复注册
    ///                     递增计数，保持既有行为）；false = pinless
    ///                     auto-register（条目计数为 0，对已存在条目为
    ///                     无记账 no-op，配额由对端 ADD_REF 填充）
    /// @return DAS_S_OK 成功，DAS_E_INVALID_POINTER 空指针
    DasResult RegisterLocalObject(
        IDasBase* object_ptr,
        ObjectId& out_object_id,
        bool      explicit_pin);

    /// @brief 记录一个远端 holder（对端物化申报 RemoteAddRef 的记账目标）
    /// @param object_id 本地条目 ObjectId
    /// @param session_id 对端 session（holder 身份）
    /// @return DAS_S_OK 成功（重复插入幂等）；DAS_E_IPC_OBJECT_NOT_FOUND
    ///         条目不存在；DAS_E_IPC_INVALID_OBJECT_ID 条目非本地对象
    DasResult AddRemoteHolder(const ObjectId& object_id, uint16_t session_id);

    /// @brief 移除一个远端 holder；条目满足移除条件（无显式 pin 且
    /// holders 空，仅在真实递减发生时判定）时回收：gen bump 作废该 oid、
    /// ptr 反查清理，并把对象本体唯一一次移交给出参——DOM 不原地析构，
    /// final Release 的线程域由调用方决定
    /// @param object_id 本地条目 ObjectId
    /// @param session_id 对端 session
    /// @param out_object_to_release [out] 条目被移除时非空
    /// @return DAS_S_OK 成功；
    ///         DAS_E_IPC_OBJECT_NOT_FOUND 条目不存在或该 session 从不是
    ///         holder（迟到/打空的 RELEASE，无递减不评估移除）；
    ///         DAS_E_IPC_INVALID_OBJECT_ID 条目非本地对象
    DasResult ReleaseRemoteHolder(
        const ObjectId&        object_id,
        uint16_t               session_id,
        DAS::DasPtr<IDasBase>& out_object_to_release);

    /// @brief 查询条目的远端 holder 数量（测试/日志可观测用）
    [[nodiscard]]
    size_t GetRemoteHolderCount(const ObjectId& object_id) const;

    /// @brief 查询本地条目数量（测试/日志可观测用）
    [[nodiscard]]
    size_t GetLocalEntryCount() const;

    DasResult LookupObject(const ObjectId& object_id, IDasBase** object_ptr)
        override;

    bool IsValidObject(const ObjectId& object_id) const override;
    bool IsLocalObject(const ObjectId& object_id) const override;

    /// @brief 从指针查找 ObjectId（反向索引查询）
    /// @param ptr 已注册的本地对象指针
    /// @param out_id [out] 找到的 ObjectId
    /// @return DAS_S_OK 成功，DAS_E_INVALID_POINTER 指针为空，DAS_E_NOT_FOUND
    /// 指针未注册
    [[nodiscard]]
    DasResult LookupObjectIdFromPtr(IDasBase* ptr, ObjectId& out_id)
        const noexcept;

    /// @brief 持 DOM 锁对本地对象执行 fn（原子，避免多操作反复加锁）。
    /// @param object_id 目标对象
    /// @param fn 回调，收到 LocalObjectRef；其返回值透传给调用方
    /// @return fn 的返回值；对象不存在 → OBJECT_NOT_FOUND，已回收(gen 不匹配)
    ///         → STALE_OBJECT_HANDLE，非本地对象 → INVALID_OBJECT_ID
    template <typename Fn>
    DasResult WithLocalObject(const ObjectId& object_id, Fn&& fn)
    {
        const DasResult validate = ValidateObjectId(object_id);
        if (validate != DAS_S_OK)
        {
            return validate;
        }

        const std::unique_lock lock{mutex_};
        const auto             it = objects_.find(object_id);
        if (it == objects_.end())
        {
            if (object_id.session_id == GetLocalSessionId())
            {
                const auto gen_it =
                    local_id_generations_.find(object_id.local_id);
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
        LocalObjectRef ref{it->second};
        return std::forward<Fn>(fn)(ref);
    }

private:
    static DasResult ValidateObjectId(const ObjectId& object_id);

    uint16_t GetLocalSessionId() const;

#ifndef NDEBUG
    void AssertBusinessThread() const
    {
        assert(
            business_thread_id_ != std::thread::id()
            && std::this_thread::get_id() == business_thread_id_);
    }
#endif

    std::atomic<uint16_t> session_id_{0};
    std::thread::id       business_thread_id_;

    mutable std::shared_mutex                 mutex_;
    std::unordered_map<ObjectId, ObjectEntry> objects_;
    uint32_t                                  next_local_id_{1};
    std::unordered_map<uint32_t, uint16_t>    local_id_generations_;
    std::unordered_map<IDasBase*, ObjectId>
        ptr_to_id_; // 指针 -> ObjectId 反向索引
};
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_DISTRIBUTED_OBJECT_MANAGER_H
