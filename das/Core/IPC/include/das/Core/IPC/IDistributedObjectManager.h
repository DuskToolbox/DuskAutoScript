#ifndef DAS_CORE_IPC_IDISTRIBUTED_OBJECT_MANAGER_H
#define DAS_CORE_IPC_IDISTRIBUTED_OBJECT_MANAGER_H

#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/IDasBase.h>

DAS_CORE_IPC_NS_BEGIN

/// 分布式对象管理器接口（抽象基类）
/// 外部调用者通过此接口调用，无需导出 DistributedObjectManager 具体类
class IDistributedObjectManager
{
public:


    /// 注册本地对象
    /// @param object_ptr 对象指针
    /// @param out_object_id 输出分配的对象ID
    /// @return DasResult
    virtual DasResult RegisterLocalObject(
        void*     object_ptr,
        ObjectId& out_object_id) = 0;

    /// 注册远程对象
    /// @param object_id 对象ID
    /// @return DasResult
    virtual DasResult RegisterRemoteObject(const ObjectId& object_id) = 0;

    /// 注销对象
    /// @param object_id 对象ID
    /// @return DasResult
    virtual DasResult UnregisterObject(const ObjectId& object_id) = 0;

    /// 增加引用计数
    /// @param object_id 对象ID
    /// @return DasResult
    virtual DasResult AddRef(const ObjectId& object_id) = 0;

    /// 减少引用计数
    /// @param object_id 对象ID
    /// @return DasResult
    virtual DasResult Release(const ObjectId& object_id) = 0;

    /// 查找对象
    /// @param object_id 对象ID
    /// @param object_ptr 输出对象指针
    /// @return DasResult
    virtual DasResult LookupObject(
        const ObjectId& object_id,
        void**          object_ptr) = 0;

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
