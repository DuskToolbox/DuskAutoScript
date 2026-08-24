#include <das/Core/IPC/InterfaceParamSerialization.h>

#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/DasProxyBase.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/MethodMetadata.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasGuidHolder.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>

DAS_CORE_IPC_NS_BEGIN

PendingInParamExportGuard::~PendingInParamExportGuard()
{
    if (committed_)
    {
        return;
    }
    for (const auto& pending_id : pending_ids_)
    {
        DAS::DasPtr<IDasBase> object_to_release;
        const DasResult remove_result = object_manager_.RemoveIfUnheldAutoEntry(
            pending_id,
            object_to_release);
        if (DAS::IsFailed(remove_result))
        {
            DAS_CORE_LOG_WARN(
                "PendingInParamExportGuard rollback could not inspect entry, "
                "result = {}, object_id = (session={}, gen={}, local={})",
                remove_result,
                pending_id.session_id,
                pending_id.generation,
                pending_id.local_id);
            continue;
        }
        if (object_to_release.Get() == nullptr)
        {
            // 条目仍被远端 holder 或显式 pin 持有：保留，由对端 RELEASE /
            // 显式注销按配对收口——绝不连根拔已物化条目
            DAS_CORE_LOG_WARN(
                "PendingInParamExportGuard rollback kept a still-held entry, "
                "object_id = (session={}, gen={}, local={})",
                pending_id.session_id,
                pending_id.generation,
                pending_id.local_id);
            continue;
        }
        // 对端从未收到 handle（transport 错误）：条目就地回收，对象本体在
        // 序列化调用线程（业务侧调用栈）原地析构
    }
}

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
    ObjectId        local_id;
    const DasResult lookup_result =
        object_manager.LookupObjectIdFromPtr(interface_ptr, local_id);
    if (DAS::IsOk(lookup_result))
    {
        // Dedup 命中：零记账零消息——对端缓存单例不产生新 proxy，配额由
        // 对端物化申报（RemoteAddRef）持有
        out_id = local_id;
        return DAS_S_OK;
    }

    // Check if the pointer is an IPC Proxy via QueryInterface
    IPCProxyBase*   runtime_tag = nullptr;
    const DasResult qi_result = interface_ptr->QueryInterface(
        DasIidOf<IPCProxyBase>(),
        reinterpret_cast<void**>(&runtime_tag));
    if (DAS::IsOk(qi_result) && runtime_tag != nullptr)
    {
        out_id = runtime_tag->GetObjectId();
        static_cast<void>(runtime_tag->ReleaseRuntimeTagRef());
        return DAS_S_OK;
    }
    // DAS_E_NO_INTERFACE is NOT an error, try auto-register next

    // Auto-register as a new local object (not found in registry and not a
    // Proxy). Pinless：条目不携带本地名额，对端物化时以 RemoteAddRef 申报
    // 配额；"handle 发出但对端从未物化"的条目空挂至进程退出（有界，缓存
    // 语义）
    DAS_CORE_LOG_TRACE(
        "SerializeInInterfaceParam: auto-registering new local object");
    DasResult register_result =
        object_manager.RegisterLocalObject(interface_ptr, out_id, false);
    if (DAS::IsFailed(register_result))
    {
        DAS_CORE_LOG_ERROR(
            "SerializeInInterfaceParam: failed to auto-register local object, "
            "result = {}",
            register_result);
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

    const ObjectId id = DecodeObjectId(encoded_id);
    if (IsNullObjectId(id))
    {
        return DAS_S_OK; // nullptr parameter
    }

    // Local object: direct lookup
    if (object_manager.IsLocalObject(id))
    {
        // LookupObject AddRefs on success; caller receives ownership
        const DasResult result = object_manager.LookupObject(id, out_ptr);
        return result;
    }

    // Remote object: create proxy via cache (autogen first, then manual
    // fallback)
    auto [create_result, proxy] = proxy_factory.GetOrCreateProxy(
        run_loop,
        business_thread,
        id,
        interface_id);

    if (!proxy)
    {
        DAS_CORE_LOG_ERROR(
            "DeserializeInInterfaceParam: GetOrCreateProxy failed, "
            "result = {}, interface_id = 0x{:08X}",
            create_result,
            interface_id);
        return create_result;
    }

    *out_ptr = proxy.Get();
    (*out_ptr)->AddRef();
    return DAS_S_OK;
}

DAS_CORE_IPC_NS_END
