#ifndef DAS_CORE_IPC_IDISTRIBUTED_OBJECT_MANAGER_H
#define DAS_CORE_IPC_IDISTRIBUTED_OBJECT_MANAGER_H

#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/IDasBase.h>

DAS_CORE_IPC_NS_BEGIN

/// 分布式对象管理器接口（抽象基类）
/// 外部调用者通过此接口调用，无需导出 DistributedObjectManager 具体类
///
/// @note 生命周期约束：不通过基类指针 delete（无 virtual dtor）。
///       所有使用方通过引用访问。使用 DestroyIpcContext() 销毁 Context
///       时会清理 DOM，不需要单独 delete。
struct IDistributedObjectManager
{

    /// 注册本地对象
    /// @param object_ptr 对象指针（会被 AddRef）
    /// @param out_object_id 输出分配的对象ID
    /// @return DasResult
    virtual DasResult RegisterLocalObject(
        IDasBase* object_ptr,
        ObjectId& out_object_id) = 0;

    /// 注册远程对象
    /// @param object_id 对象ID
    /// @return DasResult
    virtual DasResult RegisterRemoteObject(const ObjectId& object_id) = 0;

    /// 注销对象（对于本地对象，同时释放 COM 引用以平衡 Stub 端的 AddRef）
    /// @param object_id 对象ID
    /// @return DasResult
    virtual DasResult UnregisterObject(const ObjectId& object_id) = 0;

    /// 查找对象（返回 AddRef'd 指针，调用者负责 Release）
    /// @param object_id 对象ID
    /// @param object_ptr 输出 AddRef'd 的对象指针
    /// @return DasResult
    virtual DasResult LookupObject(
        const ObjectId& object_id,
        IDasBase**      object_ptr) = 0;

    /// 检查对象是否有效
    /// @param object_id 对象ID
    /// @return 是否有效
    virtual bool IsValidObject(const ObjectId& object_id) const = 0;

    /// 检查对象是否为本地对象
    /// @param object_id 对象ID
    /// @return 是否为本地对象
    virtual bool IsLocalObject(const ObjectId& object_id) const = 0;

protected:
    IDistributedObjectManager() = default;
    IDistributedObjectManager(const IDistributedObjectManager&) = delete;
    IDistributedObjectManager& operator=(const IDistributedObjectManager&) =
        delete;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IDISTRIBUTED_OBJECT_MANAGER_H
