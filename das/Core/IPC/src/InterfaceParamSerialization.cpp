#include <das/Core/IPC/InterfaceParamSerialization.h>

#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/DasProxyBase.h>
#include <das/Core/IPC/DasVariantVectorByValueProxy.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ManualProxyRegistry.h>
#include <das/Core/IPC/MethodMetadata.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasGuidHolder.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>

DAS_CORE_IPC_NS_BEGIN

DasResult SerializeInInterfaceParam(
    IDasBase*                 interface_ptr,
    DistributedObjectManager& object_manager,
    ObjectId&                 out_id,
    bool*                     out_newly_registered) noexcept
{
    if (out_newly_registered != nullptr)
    {
        *out_newly_registered = false;
    }

    // Handle null pointer: return empty ObjectId
    if (interface_ptr == nullptr)
    {
        out_id = ObjectId{0, 0, 0};
        return DAS_S_OK;
    }

    // Try to look up the object in local registry
    //    DAS_E_NOT_FOUND is NOT an error here, try Proxy branch next
    ObjectId  local_id;
    DasResult lookup_result =
        object_manager.LookupObjectIdFromPtr(interface_ptr, local_id);
    if (DAS::IsOk(lookup_result))
    {
        out_id = local_id;
        return DAS_S_OK;
    }

    // Check if the pointer is an IPC Proxy via QueryInterface
    DAS::DasPtr<IPCProxyBase> proxy;
    DasResult                 qi_result = interface_ptr->QueryInterface(
        DasIidOf<IPCProxyBase>(),
        proxy.PutVoid());
    if (DAS::IsOk(qi_result) && proxy != nullptr)
    {
        out_id = proxy->GetObjectId();
        return DAS_S_OK;
    }
    // DAS_E_NO_INTERFACE is NOT an error, try auto-register next

    // Auto-register as a new local object (not found in registry and not a
    // Proxy)
    DAS_CORE_LOG_TRACE(
        "SerializeInInterfaceParam: auto-registering new local object");
    DasResult register_result =
        object_manager.RegisterLocalObject(interface_ptr, out_id);
    if (DAS::IsFailed(register_result))
    {
        DAS_CORE_LOG_ERROR(
            "SerializeInInterfaceParam: failed to auto-register local object, "
            "result = {}",
            static_cast<int>(register_result));
        return register_result;
    }
    if (out_newly_registered != nullptr)
    {
        *out_newly_registered = true;
    }
    return DAS_S_OK;
}

DasResult DeserializeInInterfaceParam(
    uint64_t                      encoded_id,
    uint32_t                      interface_id,
    DistributedObjectManager&     object_manager,
    IpcRunLoop&                   run_loop,
    std::weak_ptr<BusinessThread> business_thread,
    ProxyFactory&                 proxy_factory,
    IDasBase**                    out_ptr) noexcept
{
    if (out_ptr == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    *out_ptr = nullptr;

    ObjectId id = DecodeObjectId(encoded_id);
    if (IsNullObjectId(id))
    {
        return DAS_S_OK; // nullptr parameter
    }

    // Local object: direct lookup
    if (object_manager.IsLocalObject(id))
    {
        // LookupObject AddRefs on success; caller receives ownership
        DasResult result = object_manager.LookupObject(id, out_ptr);
        return result;
    }

    // Dispatch path: inject by-value proxy for IDasVariantVector, skip autogen
    if (interface_id == DasVariantVectorByValueProxy::InterfaceId)
    {
        auto* by_value_proxy = new DasVariantVectorByValueProxy(
            interface_id,
            id,
            run_loop,
            business_thread,
            proxy_factory);
        object_manager.RegisterRemoteObject(id);
        *out_ptr = by_value_proxy;
        return DAS_S_OK;
    }

    // Remote object: create proxy (autogen first, then manual fallback)
    IDasBase* proxy = CreateProxyByInterfaceIdWithFallback(
        interface_id,
        id,
        run_loop,
        business_thread,
        proxy_factory);

    if (proxy == nullptr)
    {
        DAS_CORE_LOG_ERROR(
            "DeserializeInInterfaceParam: CreateProxyByInterfaceId failed, "
            "interface_id = 0x{:08X}",
            interface_id);
        return DAS_E_IPC_DESERIALIZATION_FAILED;
    }

    object_manager.RegisterRemoteObject(id);
    *out_ptr = proxy;
    return DAS_S_OK;
}

DAS_CORE_IPC_NS_END
