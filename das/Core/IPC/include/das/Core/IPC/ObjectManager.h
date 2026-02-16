#ifndef DAS_CORE_IPC_OBJECT_MANAGER_H
#define DAS_CORE_IPC_OBJECT_MANAGER_H

#include <cstdint>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/IDasBase.h>
#include <memory>
#include <mutex>
#include <unordered_map>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        struct RemoteObjectHandle
        {
            ObjectId object_id;
            uint32_t refcount;
            void*    object_ptr;
            bool     is_local;
        };

        class DistributedObjectManager
        {
        public:
            DistributedObjectManager();
            ~DistributedObjectManager();

            DasResult Initialize(uint16_t local_session_id);
            DasResult Shutdown();

            DasResult RegisterLocalObject(
                void*     object_ptr,
                ObjectId& out_object_id);
            DasResult RegisterRemoteObject(const ObjectId& object_id);
            DasResult UnregisterObject(const ObjectId& object_id);

            DasResult AddRef(const ObjectId& object_id);
            DasResult Release(const ObjectId& object_id);

            DasResult LookupObject(
                const ObjectId& object_id,
                void**          object_ptr);

            bool IsValidObject(const ObjectId& object_id) const;
            bool IsLocalObject(const ObjectId& object_id) const;

        private:
            static DasResult ValidateObjectId(const ObjectId& object_id);

            struct Impl;
            std::unique_ptr<Impl> impl_;
        };
    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_OBJECT_MANAGER_H
