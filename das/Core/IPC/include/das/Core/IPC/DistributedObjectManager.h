#ifndef DAS_CORE_IPC_DISTRIBUTED_OBJECT_MANAGER_H
#define DAS_CORE_IPC_DISTRIBUTED_OBJECT_MANAGER_H

#include <cassert>
#include <cstdint>
#include <das/Core/IPC/IDistributedObjectManager.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <memory>
#include <thread>
#include <unordered_map>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

struct ObjectEntry
{
    uint32_t              ref_count_{1}; // 引用计数（注册次数）
    ObjectId              object_id;
    DAS::DasPtr<IDasBase> object_ptr; // 本地对象非空，远程对象为 nullptr
    bool                  is_local;   // 决定 UnregisterObject 时是否 Release
};

// DistributedObjectManager 设计为单线程访问（BusinessThread），
// Debug 模式通过 assert 校验，不加 mutex。
class DistributedObjectManager final : public IDistributedObjectManager
{
public:
    DistributedObjectManager();
    ~DistributedObjectManager();

    void SetSessionId(uint16_t session_id) { session_id_ = session_id; }

    void SetBusinessThreadId(std::thread::id id) { business_thread_id_ = id; }

    DasResult RegisterLocalObject(IDasBase* object_ptr, ObjectId& out_object_id)
        override;
    DasResult RegisterRemoteObject(const ObjectId& object_id) override;
    DasResult UnregisterObject(const ObjectId& object_id) override;

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

    uint16_t        session_id_ = 0;
    std::thread::id business_thread_id_;

    std::unordered_map<ObjectId, ObjectEntry> objects_;
    uint32_t                                  next_local_id_{1};
    std::unordered_map<uint32_t, uint16_t>    local_id_generations_;
    std::unordered_map<IDasBase*, ObjectId>
        ptr_to_id_; // 指针 -> ObjectId 反向索引
};
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_DISTRIBUTED_OBJECT_MANAGER_H
