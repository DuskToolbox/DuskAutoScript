#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/Logger/Logger.h>
#include <das/IDasBase.h>
#include <das/Utils/StringUtils.h>

DAS_CORE_IPC_NS_BEGIN
ProxyFactory& ProxyFactory::GetInstance()
{
    static ProxyFactory instance;
    return instance;
}

DasResult ProxyFactory::Initialize(
    DistributedObjectManager* object_manager,
    RemoteObjectRegistry*     object_registry,
    IpcRunLoop*               run_loop)
{
    if (!object_manager || !object_registry)
    {
        return DAS_E_FAIL;
    }

    object_manager_ = object_manager;
    object_registry_ = object_registry;
    run_loop_ = run_loop;

    return DAS_S_OK;
}

bool ProxyFactory::IsInitialized() const
{
    return (object_manager_ != nullptr) && (object_registry_ != nullptr);
}

DasResult ProxyFactory::SetRunLoop(IpcRunLoop* run_loop)
{
    run_loop_ = run_loop;
    return DAS_S_OK;
}

IPCProxyBase* ProxyFactory::CreateIPCProxy(const ObjectId& object_id)
{
    // Generic Proxy 不再支持，需要使用 IDL 生成的具体 Proxy 类
    (void)object_id; // 未使用的参数
    DAS_CORE_LOG_ERROR(
        "CreateIPCProxy: Generic proxy not supported. Use IDL-generated proxy instead.");
    return nullptr;
}

DasResult ProxyFactory::ValidateObject(
    const ObjectId&   object_id,
    RemoteObjectInfo& out_info)
{
    if (!object_registry_)
    {
        return DAS_E_IPC_INVALID_STATE;
    }

    DasResult result = object_registry_->GetObjectInfo(object_id, out_info);
    if (result != DAS_S_OK)
    {
        return result;
    }

    if (!object_registry_->ObjectExists(object_id))
    {
        return DAS_E_IPC_OBJECT_NOT_FOUND;
    }

    return DAS_S_OK;
}

uint32_t ProxyFactory::GetObjectInterfaceId(const ObjectId& object_id)
{
    RemoteObjectInfo info;
    DasResult        result = ValidateObject(object_id, info);
    if (result != DAS_S_OK)
    {
        throw std::runtime_error("Cannot get object interface id");
    }

    return info.interface_id;
}
DAS_CORE_IPC_NS_END
