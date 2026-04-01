#include <das/Core/IPC/InterfaceParamSerialization.h>

#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/DasProxyBase.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ManualProxyRegistry.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/Logger/Logger.h>
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

    // 1. nullptr -> empty ObjectId
    if (interface_ptr == nullptr)
    {
        out_id = ObjectId{0, 0, 0};
        return DAS_S_OK;
    }

    // 2. Lookup registered local object
    //    DAS_E_NOT_FOUND is NOT an error here, try Proxy branch next
    ObjectId  local_id;
    DasResult lookup_result =
        object_manager.LookupObjectIdFromPtr(interface_ptr, local_id);
    if (DAS::IsOk(lookup_result))
    {
        out_id = local_id;
        return DAS_S_OK;
    }

    // 3. Detect IPC Proxy via QI (replaces dynamic_cast)
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

    // 4. New local object (not registered, not Proxy) -> auto-register
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

    // Remote object: create proxy (autogen first, then manual fallback)
    IDasBase* proxy = CreateProxyByInterfaceIdWithFallback(
        interface_id,
        id,
        run_loop,
        business_thread,
        object_manager);

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
