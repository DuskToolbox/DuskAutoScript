#ifndef DAS_CORE_IPC_INTERFACE_PARAM_SERIALIZATION_H
#define DAS_CORE_IPC_INTERFACE_PARAM_SERIALIZATION_H

#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <vector>

DAS_CORE_IPC_NS_BEGIN

class DistributedObjectManager;
class IpcRunLoop;
class BusinessThread;

/// @brief RAII guard for tracking newly registered [in] parameter exports
///
/// When SerializeInInterfaceParam auto-registers a new local object
/// (out_newly_registered = true), the caller uses Track() to record it.
/// After SendRequest returns, if the result is NOT a transport-level
/// error (IsTransportLevelError), Commit() is called and the destructor
/// does nothing. If it IS a transport-level error, the destructor calls
/// UnregisterObject() for all tracked newly-registered objects.
class PendingInParamExportGuard
{
public:
    explicit PendingInParamExportGuard(
        DistributedObjectManager& object_manager) noexcept
        : object_manager_(object_manager), committed_(false)
    {
    }

    ~PendingInParamExportGuard()
    {
        if (!committed_)
        {
            for (const auto& id : pending_ids_)
            {
                object_manager_.UnregisterObject(id);
            }
        }
    }

    // Non-copyable, non-movable
    PendingInParamExportGuard(const PendingInParamExportGuard&) = delete;
    PendingInParamExportGuard& operator=(const PendingInParamExportGuard&) =
        delete;
    PendingInParamExportGuard(PendingInParamExportGuard&&) = delete;
    PendingInParamExportGuard& operator=(PendingInParamExportGuard&&) = delete;

    /// @brief Track an ObjectId that was newly registered during serialization
    void Track(const ObjectId& object_id, bool newly_registered) noexcept
    {
        if (newly_registered)
        {
            pending_ids_.push_back(object_id);
        }
    }

    /// @brief Commit all tracked exports (prevent rollback on destruction)
    void Commit() noexcept { committed_ = true; }

private:
    DistributedObjectManager& object_manager_;
    bool                      committed_;
    std::vector<ObjectId>     pending_ids_;
};

/// @brief 序列化[in] 接口指针参数中 ObjectId
/// @param interface_ptr
/// 接口指针（可以是本地对象、Proxy 或 nullptr）
/// @param object_manager
/// 分布式对象管理器（用于反向查询和自动注册）
/// @param out_id [out] 序列化后的 ObjectId
/// @param out_newly_registered [out, optional] 是否为新自动注册的对象
/// @return DAS_S_OK 成功，其他值表示失败
/// @note 使用 QueryInterface(DasIidOf<IPCProxyBase>()) 替代 dynamic_cast 检测
/// Proxy；全新本地对象会自动 RegisterLocalObject()
[[nodiscard]]
DasResult SerializeInInterfaceParam(
    IDasBase*                 interface_ptr,
    DistributedObjectManager& object_manager,
    ObjectId&                 out_id,
    bool*                     out_newly_registered = nullptr) noexcept;

/// @brief 反序列化[in] 接口指针参数从编码 ObjectId
/// @param encoded_id 编码的 ObjectId（uint64_t）
/// @param interface_id
/// 接口哈希 ID（FNV-1a of interface UUID）
/// @param object_manager 分布式对象管理器
/// @param run_loop IPC 运行循环
/// @param business_thread 业务线程
/// @param proxy_factory Proxy 工厂（用于创建 proxy）
/// @param out_ptr [out] 输出的接口指针
/// @return DasResult
[[nodiscard]]
DasResult DeserializeInInterfaceParam(
    uint64_t                      encoded_id,
    uint32_t                      interface_id,
    DistributedObjectManager&     object_manager,
    IpcRunLoop&                   run_loop,
    std::weak_ptr<BusinessThread> business_thread,
    ProxyFactory&                 proxy_factory,
    IDasBase**                    out_ptr) noexcept;

/// @brief Check if a DasResult from SendRequest is a transport-level failure.
/// Transport-level errors mean the request definitively NEVER reached the
/// remote process. These MUST trigger rollback of any newly exported [in]
/// parameter objects. Business errors (request reached remote but remote
/// returned an error) must NOT trigger rollback.
inline bool IsTransportLevelError(DasResult result) noexcept
{
    return result == DAS_E_IPC_NOT_INITIALIZED   // io_context null
           || result == DAS_E_IPC_DISCONNECTED   // BusinessThread gone
           || result == DAS_E_IPC_NO_CONNECTIONS // no transport for session
           || result == DAS_E_IPC_SEND_FAILED    // SendCoroutine returned error
           || result == DAS_E_IPC_CANCELED // queue shutdown / thread stopped
           || result == DAS_E_IPC_REMOTE_ERROR; // SendCoroutine threw /
                                                // sync_wait failed
}

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_INTERFACE_PARAM_SERIALIZATION_H
