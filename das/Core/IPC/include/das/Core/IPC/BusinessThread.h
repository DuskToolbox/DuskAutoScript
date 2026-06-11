#ifndef DAS_CORE_IPC_BUSINESS_THREAD_H
#define DAS_CORE_IPC_BUSINESS_THREAD_H

#include <atomic>
#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <das/Core/IPC/IpcMessageQueue.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/DasExport.h>
#include <functional>
#include <memory>
#include <string_view>
#include <thread>

DAS_CORE_IPC_NS_BEGIN

class BusinessThread;
class DistributedObjectManager;
class ProxyFactory;
class RemoteObjectRegistry;

[[nodiscard]]
DAS_API bool IsCurrentBusinessThread() noexcept;

[[nodiscard]]
DAS_API BusinessThread* GetCurrentBusinessThread() noexcept;

/**
 * @brief 业务线程类
 *
 * 从 inbound queue 取消息并 dispatch 到 handler。
 * 支持 PumpUntilResponse 用于嵌套 pump（Phase 24 Proxy 调用）。
 */
class BusinessThread
    : public std::enable_shared_from_this<BusinessThread>
{
public:
    /**
     * @brief 构造函数（构造即启动线程）
     * @param inbound 入站消息队列（IpcContext 持有）
     * @param run_loop IpcRunLoop 引用（用于 PostSend 和 CompletePendingCall）
     * @param resolve_context IResolveContext 引用（用于绑定 g_current_context）
     * @param proxy_factory ProxyFactory 引用（由 IpcContext 持有）
     * @param registry RemoteObjectRegistry 引用（由 IpcContext 持有）
     */
    BusinessThread(
        IpcMessageQueue<InboundMessage>& inbound,
        IpcRunLoop&                      run_loop,
        IResolveContext&                 resolve_context,
        ProxyFactory&                    proxy_factory,
        RemoteObjectRegistry&            registry);

    ~BusinessThread();

    // 禁用拷贝
    BusinessThread(const BusinessThread&) = delete;
    BusinessThread& operator=(const BusinessThread&) = delete;

    // 禁用移动（因为有引用成员）
    BusinessThread(BusinessThread&&) = delete;
    BusinessThread& operator=(BusinessThread&&) = delete;

    /**
     * @brief 停止业务线程
     *
     * 调用 inbound_.Uninitialize() 然后 join 线程
     */
    void Stop();

    /**
     * @brief 泵入直到收到匹配的响应（用于嵌套 pump）
     *
     * 用于 Phase 24 Proxy 调用场景：业务线程发送请求后，
     * 需要在同一线程上泵入入站消息，直到收到匹配的响应。
     *
     * @param my_call_key 等待的 CallKey
     * @param out_response [out] 响应体
     * @param out_flags [out] 可选：响应头 flags 字段
     * @return DasResult DAS_S_OK 成功，DAS_E_IPC_CANCELED 队列已关闭
     */
    DasResult PumpUntilResponse(
        CallKey               my_call_key,
        std::vector<uint8_t>& out_response,
        uint16_t*             out_flags = nullptr);

    /**
     * @brief 泵入直到外部 predicate 完成（用于 BT 内同步 ABI 等待）
     *
     * 用于 JS director Promise settlement 等非 IPC response
     * 信号。当前线程已经是 BusinessThread 时，等待期间继续处理入站
     * IPC，避免阻塞嵌套 REQUEST/EVENT。
     *
     * @param wait_reason 调试用等待原因
     * @param is_done 轻量完成条件；必须只读取 atomic/已同步状态
     * @return DasResult DAS_S_OK predicate 完成，DAS_E_IPC_CANCELED 队列已关闭
     */
    DasResult PumpUntilPredicate(
        std::string_view             wait_reason,
        const std::function<bool()>& is_done);

    /// @brief 唤醒当前 BusinessThread 队列等待者重新检查外部 predicate
    void NotifyWaiters();

    /**
     * @brief 检查业务线程是否正在运行
     * @return true 如果线程正在运行
     */
    bool IsRunning() const;

    /**
     * @brief 检查当前线程是否为业务线程
     * @return true 如果当前线程是业务线程的执行线程
     */
    [[nodiscard]]
    bool IsCurrentThread() const;

private:
    /**
     * @brief 线程主循环
     */
    void Run();

    /**
     * @brief 分发消息到 handler
     * @param msg 入站消息
     */
    void ProcessInboundMessage(InboundMessage& msg);

    /// 入站消息队列（非持有，IpcContext 的值成员引用）
    IpcMessageQueue<InboundMessage>& inbound_;

    /// IpcRunLoop 引用（用于 PostSend 和 CompletePendingCall）
    IpcRunLoop& run_loop_;

    /// IResolveContext 引用（用于绑定 g_current_context）
    IResolveContext& resolve_context_;

    /// ProxyFactory 引用（由构造函数注入）
    ProxyFactory& proxy_factory_;

    /// RemoteObjectRegistry 引用（由构造函数注入）
    RemoteObjectRegistry& registry_;

    /// 业务线程
    std::thread thread_;

    /// 运行状态
    std::atomic<bool> running_{false};
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_BUSINESS_THREAD_H
