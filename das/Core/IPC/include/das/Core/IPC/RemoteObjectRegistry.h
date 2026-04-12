#ifndef DAS_CORE_IPC_REMOTE_OBJECT_REGISTRY_H
#define DAS_CORE_IPC_REMOTE_OBJECT_REGISTRY_H

#include <cstdint>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/IDasBase.h>
#include <string>
#include <unordered_map>
#include <vector>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN
struct RemoteObjectInfo
{
    DasGuid     iid;          // 接口ID (原始 GUID)
    uint32_t    interface_id; // 接口ID (FNV-1a hash of GUID)
    ObjectId    object_id;    // 对象ID
    uint16_t    session_id;   // 会话ID
    std::string name;         // 对象名称
    uint16_t    version;      // 接口版本
};

class RemoteObjectRegistry
{
public:
    RemoteObjectRegistry() = default;
    ~RemoteObjectRegistry() = default;

    // 注册远程对象 - 6参数版本：显式指定 interface_id
    DasResult RegisterObject(
        const ObjectId&    object_id,
        const DasGuid&     iid,
        uint32_t           interface_id,
        uint16_t           session_id,
        const std::string& name,
        uint16_t           version = 1);

    // 注册远程对象 - 5参数版本：自动计算 interface_id
    DasResult RegisterObject(
        const ObjectId&    object_id,
        const DasGuid&     iid,
        uint16_t           session_id,
        const std::string& name,
        uint16_t           version = 1);

    // 注销远程对象
    DasResult UnregisterObject(const ObjectId& object_id);
    void      UnregisterAllFromSession(uint16_t session_id);

    // 查找远程对象
    DasResult LookupByName(const std::string& name, RemoteObjectInfo& out_info)
        const;
    DasResult LookupByInterface(
        uint32_t          interface_id,
        RemoteObjectInfo& out_info) const;
    DasResult GetObjectInfo(
        const ObjectId&   object_id,
        RemoteObjectInfo& out_info) const;

    // 列出对象
    void ListAllObjects(std::vector<RemoteObjectInfo>& out_objects) const;
    void ListObjectsBySession(
        uint16_t                       session_id,
        std::vector<RemoteObjectInfo>& out_objects) const;

    // 辅助方法
    bool   ObjectExists(const ObjectId& object_id) const;
    size_t GetObjectCount() const;
    void   Clear();

    // 计算 interface_id (FNV-1a hash of GUID)
    static uint32_t ComputeInterfaceId(const DasGuid& guid);

    // 禁止拷贝和赋值
    RemoteObjectRegistry(const RemoteObjectRegistry&) = delete;
    RemoteObjectRegistry& operator=(const RemoteObjectRegistry&) = delete;

private:
    // 内部数据结构
    struct ObjectEntry
    {
        RemoteObjectInfo info;
    };

    std::unordered_map<uint64_t, ObjectEntry> objects_by_id_;
    std::unordered_map<std::string, uint64_t> objects_by_name_;
    std::unordered_map<uint32_t, uint64_t>    objects_by_interface_;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_REMOTE_OBJECT_REGISTRY_H