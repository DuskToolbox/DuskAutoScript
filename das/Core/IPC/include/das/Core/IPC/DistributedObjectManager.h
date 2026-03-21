#ifndef DAS_CORE_IPC_DISTRIBUTED_OBJECT_MANAGER_H
#define DAS_CORE_IPC_DISTRIBUTED_OBJECT_MANAGER_H

#include <cstdint>
#include <das/Core/IPC/IDistributedObjectManager.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/IDasBase.h>
#include <das/DasPtr.hpp>
#include <memory>
#include <unordered_map>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

class IpcRunLoop;
struct ObjectEntry
{
    ObjectId                 object_id;
    DAS::DasPtr<IDasBase>    object_ptr;  // 本地对象非空，远程对象为 nullptr
    bool                     is_local;    // 决定 UnregisterObject 时是否 Release
};

class DistributedObjectManager : public IDistributedObjectManager
{
public:
    DistributedObjectManager();
    ~DistributedObjectManager();

    void SetRunLoop(IpcRunLoop* run_loop) { run_loop_ = run_loop; }

    DasResult RegisterLocalObject(IDasBase* object_ptr, ObjectId& out_object_id)
        override;
    DasResult RegisterRemoteObject(const ObjectId& object_id) override;
    DasResult UnregisterObject(const ObjectId& object_id) override;

    DasResult LookupObject(const ObjectId& object_id, IDasBase** object_ptr)
        override;

    bool IsValidObject(const ObjectId& object_id) const override;
    bool IsLocalObject(const ObjectId& object_id) const override;

private:
    static DasResult ValidateObjectId(const ObjectId& object_id);

    uint16_t GetLocalSessionId() const;

    IpcRunLoop* run_loop_ = nullptr;

    std::unordered_map<ObjectId, ObjectEntry> objects_;
    uint32_t                                  next_local_id_{1};
    std::unordered_map<uint32_t, uint16_t>    local_id_generations_;
};
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_DISTRIBUTED_OBJECT_MANAGER_H
