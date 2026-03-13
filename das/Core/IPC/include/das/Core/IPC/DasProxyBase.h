#ifndef DAS_CORE_IPC_DAS_PROXY_BASE_H
#define DAS_CORE_IPC_DAS_PROXY_BASE_H

#include <das/Core/IPC/IPCProxyBase.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/ObjectManager.h>
#include <das/DasConfig.h>
#include <das/IDasBase.h>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN
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
        DAS_LIFETIMEBOUND
    {
        return object_manager_;
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
        writer.WriteBytes(&iid, sizeof(DasGuid));

        std::vector<uint8_t> response;
        DasResult            result = SendRequest(
            static_cast<uint16_t>(IpcCommandType::QUERY_INTERFACE),
            writer.GetData(),
            writer.GetSize(),
            response);

        if (DAS::IsFailed(result))
        {
            return result;
        }

        // 解析响应（只读取 result, interface_id, new_object_id）
        MemorySerializerReader reader(response.data(), response.size());
        DasResult query_result = static_cast<DasResult>(reader.ReadInt32());
        uint32_t  interface_id = reader.ReadUInt32();
        uint64_t  new_object_id = reader.ReadUInt64();

        if (DAS::IsFailed(query_result))
        {
            return query_result;
        }

        // 根据 interface_id 创建对应的 Proxy
        // 注意：这里需要调用 IDL 生成的 CreateProxyByInterfaceId 函数
        // 由于是模板函数，需要在生成的代码中调用
        ObjectId new_obj_id = DecodeObjectId(new_object_id);
        (void)interface_id;
        (void)new_obj_id;

        // TODO: 使用生成的 CreateProxyByInterfaceId 创建 Proxy
        // IPCProxyBase* proxy = CreateProxyByInterfaceId(
        //     interface_id,
        //     new_obj_id,
        //     GetRunLoop(),
        //     GetObjectManager());

        // if (proxy == nullptr)
        // {
        //     return DAS_E_NO_INTERFACE;
        // }

        // *pp_object = proxy;
        return DAS_E_NOT_IMPLEMENTED;
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
        if (DAS::IsFailed(result))
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
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_DAS_PROXY_BASE_H
