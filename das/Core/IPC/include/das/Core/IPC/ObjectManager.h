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
            uint64_t object_id;
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
                uint64_t& out_object_id);
            DasResult RegisterRemoteObject(uint64_t object_id);
            DasResult UnregisterObject(uint64_t object_id);

            DasResult AddRef(uint64_t object_id);
            DasResult Release(uint64_t object_id);

            DasResult LookupObject(uint64_t object_id, void** object_ptr);

            bool IsValidObject(uint64_t object_id) const;
            bool IsLocalObject(uint64_t object_id) const;

        private:
            DasResult ValidateObjectId(uint64_t object_id, ObjectId& out_id)
                const;

            struct Impl;
            std::unique_ptr<Impl> impl_;
        };
    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_OBJECT_MANAGER_H
