#ifndef DAS_CORE_IPC_DAS_PROXY_BASE_H
#define DAS_CORE_IPC_DAS_PROXY_BASE_H

#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IPCProxyBase.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/MemorySerializer.h>
#include <das/Core/IPC/ProxyFactory.h>
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
        ProxyFactory&                 proxy_factory);
}

template <typename TInterface>
class DasProxyBase : public IPCProxyBase
{
public:
    using InterfaceType = TInterface;

    /**
     * @brief Destructor with remote-ref paired teardown.
     *
     * Removes this proxy's interface view from the factory cache, then
     * decrements the DOM remote view count via
     * DOM::UnregisterObject(oid, entry_removed). A CONTROL_PLANE
     * RemoteRelease EVENT is sent fire-and-forget only when that call
     * removed the DOM entry (the last view of this object died) — one
     * message per (object × peer), strictly paired with the RemoteAddRef
     * the factory sent on first materialization. The IsRuntimeAvailable
     * gate keeps shutdown-period destruction silent.
     */
    ~DasProxyBase() override
    {
        const ObjectId& oid = GetObjectId();
        if ((oid.session_id != 0 || oid.local_id != 0) && IsRuntimeAvailable())
        {
            proxy_factory_.OnProxyFinalRelease(
                oid,
                GetInterfaceId(),
                static_cast<IPCProxyBase*>(this));

            auto& dom = GetObjectManager();
            // 远程条目判定先于递减：同 oid 撞号的本地条目即使被移除也不
            // 发 RELEASE——申报只配对 RegisterRemoteObject 建立的远程条目
            const bool is_local_entry = dom.IsLocalObject(oid);

            bool entry_removed = false;
            dom.UnregisterObject(oid, entry_removed);

            if (entry_removed && !is_local_entry)
            {
                // 条目移除（最后一个视图归零）时发送唯一的 RELEASE：经
                // 出站 FIFO 入队（fire-and-forget 不变，帧构造单一事实源
                // 在 IpcRunLoop::BuildReleaseFrame；owner 侧条目拆除延迟
                // 上界 = 一个 dispatch 周期）
                GetRunLoop().EnqueueReleaseFrame(oid);
            }
        }
    }

    [[nodiscard]]
    DistributedObjectManager& GetObjectManager() const noexcept
        DAS_LIFETIMEBOUND
    {
        return proxy_factory_.GetObjectManager();
    }

    /// @brief 增加引用计数
    /// @return 新的引用计数
    [[nodiscard]]
    uint32_t AddRef()
    {
        return AddRefImpl();
    }

    /// @brief 释放引用计数
    /// @return 新的引用计数
    [[nodiscard]]
    uint32_t Release()
    {
        return ReleaseImpl();
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

        // 本地拦截：识别 Proxy 身份（替代 dynamic_cast）
        if (iid == DasIidOf<IPCProxyBase>())
        {
            *pp_object = static_cast<IPCProxyBase*>(this);
            (void)AddRefImpl();
            return DAS_S_OK;
        }

        // 构造请求 Body：ObjectId + DasGuid
        // Host 端通过 ObjectId 查找真实对象，通过 iid 调用 QueryInterface
        const ObjectId&      obj_id = GetObjectId();
        std::vector<uint8_t> body;
        body.insert(
            body.end(),
            reinterpret_cast<const uint8_t*>(&obj_id),
            reinterpret_cast<const uint8_t*>(&obj_id) + sizeof(obj_id));
        body.insert(
            body.end(),
            reinterpret_cast<const uint8_t*>(&iid),
            reinterpret_cast<const uint8_t*>(&iid) + sizeof(DasGuid));

        std::vector<uint8_t> response;
        DasResult            result = SendBusinessControlRequest(
            IpcCommandType::QueryInterface,
            body.data(),
            body.size(),
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
        const DasResult query_result = static_cast<DasResult>(query_result_val);

        if (DAS::IsFailed(query_result))
        {
            return query_result;
        }

        // 根据 interface_id 创建对应的 Proxy（走 ProxyFactory 缓存）
        const ObjectId new_obj_id = DecodeObjectId(new_object_id);

        result = CheckRuntimeAvailable("DasProxyBase::QueryInterfaceRemote");
        if (DAS::IsFailed(result))
        {
            return result;
        }

        auto [create_result, proxy] = proxy_factory_.GetOrCreateProxy(
            GetRunLoop(),
            GetBusinessThread(),
            new_obj_id,
            interface_id);

        if (!proxy)
        {
            return create_result;
        }

        *pp_object = proxy.Get();
        static_cast<IDasBase*>(*pp_object)
            ->AddRef(); // QI contract: AddRef for caller
        return DAS_S_OK;
        // proxy (DasPtr) 析构时 Release，与 AddRef 配对
    }

protected:
    DasProxyBase(
        uint32_t                      interface_id,
        const ObjectId&               object_id,
        IpcRunLoop&                   run_loop,
        std::weak_ptr<BusinessThread> business_thread,
        ProxyFactory&                 proxy_factory)
        : IPCProxyBase(
              interface_id,
              object_id,
              run_loop,
              std::move(business_thread),
              proxy_factory)
    {
    }

    template <typename TProxy, typename... Args>
    static DasResult CreateProxy(
        uint64_t                      encoded_object_id,
        IpcRunLoop*                   run_loop,
        ProxyFactory*                 proxy_factory,
        std::weak_ptr<BusinessThread> business_thread,
        TProxy**                      out_proxy,
        Args&&... args)
    {
        if (!out_proxy || !run_loop || !proxy_factory)
        {
            return DAS_E_INVALID_POINTER;
        }

        ObjectId obj_id = DecodeObjectId(encoded_object_id);

        DAS::DasPtr<IDasBase> obj_ptr;
        const DasResult result = proxy_factory->GetObjectManager().LookupObject(
            obj_id,
            obj_ptr.Put());
        if (DAS::IsFailed(result))
        {
            return result;
        }

        auto proxy = new TProxy(
            TProxy::InterfaceId,
            obj_id,
            *run_loop,
            business_thread,
            *proxy_factory,
            std::forward<Args>(args)...);

        *out_proxy = proxy;
        return DAS_S_OK;
    }
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_DAS_PROXY_BASE_H
