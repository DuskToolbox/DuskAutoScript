#ifndef DAS_CORE_IPC_OBJECT_MANAGER_H
#define DAS_CORE_IPC_OBJECT_MANAGER_H

#include <cstdint>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/IDasBase.h>
#include <memory>
#include <mutex>
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

#ifdef _MSC_VER
#pragma warning(disable : 4251)
#endif
class DistributedObjectManager
{
public:
    DistributedObjectManager();
    ~DistributedObjectManager();

    DasResult Initialize(uint16_t local_session_id);
    DasResult Shutdown();

    DasResult RegisterLocalObject(void* object_ptr, ObjectId& out_object_id);
    DasResult RegisterRemoteObject(const ObjectId& object_id);
    DasResult UnregisterObject(const ObjectId& object_id);

    DasResult AddRef(const ObjectId& object_id);
    DasResult Release(const ObjectId& object_id);

    DasResult LookupObject(const ObjectId& object_id, void** object_ptr);

    bool IsValidObject(const ObjectId& object_id) const;
    bool IsLocalObject(const ObjectId& object_id) const;

private:
    static DasResult ValidateObjectId(const ObjectId& object_id);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#ifdef _MSC_VER
#pragma warning(default : 4251)
#endif
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_OBJECT_MANAGER_H
