#ifndef DAS_CORE_IPC_DAS_PROXY_BASE_H
#define DAS_CORE_IPC_DAS_PROXY_BASE_H

#include <das/Core/IPC/IPCProxyBase.h>
#include <das/Core/IPC/ObjectManager.h>
#include <das/IDasBase.h>

#ifndef DAS_FAILED
#define DAS_FAILED(result) ((result) != DAS_S_OK)
#endif

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        template <typename TInterface>
        class DasProxyBase : public IPCProxyBase
        {
        public:
            using InterfaceType = TInterface;

            ~DasProxyBase() override
            {
                if (object_manager_
                    && (object_id_.session_id != 0 || object_id_.local_id != 0))
                {
                    object_manager_->Release(EncodeObjectId(object_id_));
                }
            }

            [[nodiscard]]
            DistributedObjectManager* GetObjectManager() const noexcept
            {
                return object_manager_;
            }

        protected:
            DasProxyBase(
                uint32_t                  interface_id,
                const ObjectId&           object_id,
                IpcRunLoop*               run_loop,
                DistributedObjectManager* object_manager)
                : IPCProxyBase(interface_id, object_id, run_loop),
                  object_manager_(object_manager)
            {
            }

            template <typename TProxy, typename... Args>
            static DasResult CreateProxy(
                uint64_t                  encoded_object_id,
                IpcRunLoop*               run_loop,
                DistributedObjectManager* object_manager,
                TProxy**                  out_proxy,
                Args&&... args)
            {
                if (!out_proxy || !run_loop || !object_manager)
                    return DAS_E_INVALIDARG;

                void*     obj_ptr = nullptr;
                DasResult result =
                    object_manager->LookupObject(encoded_object_id, &obj_ptr);
                if (DAS_FAILED(result))
                    return result;

                ObjectId obj_id = DecodeObjectId(encoded_object_id);

                auto proxy = new TProxy(
                    TProxy::InterfaceId,
                    obj_id,
                    run_loop,
                    object_manager,
                    std::forward<Args>(args)...);

                *out_proxy = proxy;
                return DAS_S_OK;
            }

            using IPCProxyBase::object_id_;

        private:
            DistributedObjectManager* object_manager_;
        };

    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_DAS_PROXY_BASE_H
