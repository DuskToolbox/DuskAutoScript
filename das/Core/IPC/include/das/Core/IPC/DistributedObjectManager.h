#ifndef DAS_CORE_IPC_DISTRIBUTED_OBJECT_MANAGER_H
#define DAS_CORE_IPC_DISTRIBUTED_OBJECT_MANAGER_H

#include <cstdint>
#include <das/Core/IPC/IDistributedObjectManager.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/IDasBase.h>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN
struct RemoteObjectHandle
{
    ObjectId object_id;
    uint32_t refcount;
    void*    object_ptr;
    bool     is_local;
};

class DistributedObjectManager : public IDistributedObjectManager
{
public:
    DistributedObjectManager();
    ~DistributedObjectManager();

    DasResult Initialize(uint16_t local_session_id);
    DasResult Shutdown();

    DasResult RegisterLocalObject(void* object_ptr, ObjectId& out_object_id)
        override;
    DasResult RegisterRemoteObject(const ObjectId& object_id) override;
    DasResult UnregisterObject(const ObjectId& object_id) override;

    DasResult AddRef(const ObjectId& object_id) override;
    DasResult Release(const ObjectId& object_id) override;

    DasResult LookupObject(const ObjectId& object_id, void** object_ptr)
        override;

    bool IsValidObject(const ObjectId& object_id) const override;
    bool IsLocalObject(const ObjectId& object_id) const override;

private:
    static DasResult ValidateObjectId(const ObjectId& object_id);

    std::unordered_map<ObjectId, RemoteObjectHandle> objects_;
    mutable std::shared_mutex                        objects_mutex_;
    uint16_t                                         local_session_id_{0};
    uint32_t                                         next_local_id_{1};
    std::unordered_map<uint32_t, uint16_t>           local_id_generations_;
};
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_DISTRIBUTED_OBJECT_MANAGER_H
