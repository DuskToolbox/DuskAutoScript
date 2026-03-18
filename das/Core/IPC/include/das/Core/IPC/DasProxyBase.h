#ifndef DAS_CORE_IPC_DAS_PROXY_BASE_H
#define DAS_CORE_IPC_DAS_PROXY_BASE_H

#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IPCProxyBase.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/MemorySerializer.h>
#include <das/DasConfig.h>
#include <das/DasTypes.hpp>
#include <das/IDasBase.h>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

namespace DasIpcProxy
{
    IDasBase* CreateProxyByInterfaceId(
        uint32_t                      interface_id,
        const ObjectId&               object_id,
        IpcRunLoop&                   run_loop,
        std::weak_ptr<BusinessThread> business_thread,
        DistributedObjectManager&     object_manager);
}

template <typename TInterface>
class DasProxyBase : public IPCProxyBase, public IDasBase
{
public:
    using InterfaceType = TInterface;

    /// @brief 实现 IDasBase::QueryInterface
    /// @note 委托给 IPC 远程 QueryInterface
    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override
    {
        return QueryInterfaceRemote(iid, pp_object);
    }

    ~DasProxyBase() override
    {
        const ObjectId& oid = GetObjectId();
        if (oid.session_id != 0 || oid.local_id != 0)
        {
            // 本地释放（引用计数）
            GetObjectManager().Release(oid);

            // 发送 RELEASE_OBJECT fire-and-forget 消息到远程
            // 构建控制平面 EVENT 消息
            std::vector<uint8_t> release_body;
            // 序列化 ObjectId 到 body
            release_body.insert(
                release_body.end(),
                reinterpret_cast<const uint8_t*>(&oid),
                reinterpret_cast<const uint8_t*>(&oid) + sizeof(oid));

            auto release_header =
                IPCMessageHeaderBuilder()
                    .SetMessageType(MessageType::EVENT)
                    .SetHeaderFlags(HeaderFlags::CONTROL_PLANE)
                    .SetInterfaceId(
                        static_cast<uint32_t>(IpcCommandType::REMOTE_RELEASE))
                    .SetSourceSessionId(GetSourceSessionId())
                    .SetTargetSessionId(oid.session_id)
                    .Build();

            // PostSend 是 fire-and-forget，不检查结果
            GetRunLoop()->PostSend(release_header, std::move(release_body));
        }
    }

    [[nodiscard]]
    DistributedObjectManager& GetObjectManager() const noexcept
        DAS_LIFETIMEBOUND
    {
        return IPCProxyBase::GetObjectManager();
    }

    /// @brief IPC 远程 QueryInterface
    /// @param iid 要查询的接口 GUID
    /// @param pp_object 输出指针
    /// @return DAS_S_OK 成功，DAS_E_NO_INTERFACE 不支持
    DasResult QueryInterfaceRemote(const DasGuid& iid, void** pp_object)
    {
        if (pp_object == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }

        *pp_object = nullptr;

        // 构造请求 Body：只有 iid（ObjectId 在 Header 中）
        MemorySerializerWriter writer;
        writer.Write(&iid, sizeof(DasGuid));

        const auto&          writer_buf = writer.GetBuffer();
        std::vector<uint8_t> response;
        DasResult            result = SendRequest(
            static_cast<uint16_t>(IpcCommandType::QUERY_INTERFACE),
            writer_buf.data(),
            writer_buf.size(),
            response);

        if (DAS::IsFailed(result))
        {
            return result;
        }

        // 解析响应（只读取 result, interface_id, new_object_id）
        MemorySerializerReader reader(response);
        int32_t                query_result_val = 0;
        uint32_t               interface_id = 0;
        uint64_t               new_object_id = 0;
        reader.ReadInt32(&query_result_val);
        reader.ReadUInt32(&interface_id);
        reader.ReadUInt64(&new_object_id);
        DasResult query_result = static_cast<DasResult>(query_result_val);

        if (DAS::IsFailed(query_result))
        {
            return query_result;
        }

        // 根据 interface_id 创建对应的 Proxy
        ObjectId new_obj_id = DecodeObjectId(new_object_id);

        // 使用生成的 CreateProxyByInterfaceId 创建 Proxy
        IDasBase* proxy = DasIpcProxy::CreateProxyByInterfaceId(
            interface_id,
            new_obj_id,
            *GetRunLoop(),
            GetBusinessThread(),
            GetObjectManager());

        if (proxy == nullptr)
        {
            return DAS_E_NO_INTERFACE;
        }

        *pp_object = proxy;
        return DAS_S_OK;
    }

protected:
    DasProxyBase(
        uint32_t                      interface_id,
        const ObjectId&               object_id,
        IpcRunLoop&                   run_loop,
        std::weak_ptr<BusinessThread> business_thread,
        DistributedObjectManager&     object_manager)
        : IPCProxyBase(
              interface_id,
              object_id,
              run_loop,
              std::move(business_thread),
              object_manager)
    {
    }

    template <typename TProxy, typename... Args>
    static DasResult CreateProxy(
        uint64_t                      encoded_object_id,
        IpcRunLoop*                   run_loop,
        DistributedObjectManager*     object_manager,
        std::weak_ptr<BusinessThread> business_thread,
        TProxy**                      out_proxy,
        Args&&... args)
    {
        if (!out_proxy || !run_loop || !object_manager)
        {
            return DAS_E_INVALID_POINTER;
        }

        ObjectId obj_id = DecodeObjectId(encoded_object_id);

        void*     obj_ptr = nullptr;
        DasResult result = object_manager->LookupObject(obj_id, &obj_ptr);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        auto proxy = new TProxy(
            TProxy::InterfaceId,
            obj_id,
            *run_loop,
            business_thread,
            *object_manager,
            std::forward<Args>(args)...);

        *out_proxy = proxy;
        return DAS_S_OK;
    }
};
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_DAS_PROXY_BASE_H
