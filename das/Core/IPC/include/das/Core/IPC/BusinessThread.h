#ifndef DAS_CORE_IPC_BUSINESS_THREAD_H
#define DAS_CORE_IPC_BUSINESS_THREAD_H

#include <atomic>
#include <das/Core/IPC/IpcMessageQueue.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <memory>
#include <thread>

DAS_CORE_IPC_NS_BEGIN

class DistributedObjectManager;

/**
 * @brief 业务线程类
 *
 * 从 inbound queue 取消息并 dispatch 到 handler。
 * 支持 PumpUntilResponse 用于嵌套 pump（Phase 24 Proxy 调用）。
 */
class BusinessThread : public std::enable_shared_from_this<BusinessThread>
{
public:
    /**
     * @brief 构造函数
     * @param inbound 入站消息队列（IpcContext 持有）
     * @param run_loop IpcRunLoop 引用（用于 PostSend 和 CompletePendingCall）
     */
    BusinessThread(
        IpcMessageQueue<InboundMessage>& inbound,
        IpcRunLoop&                      run_loop);

    ~BusinessThread();

    // 禁用拷贝
    BusinessThread(const BusinessThread&) = delete;
    BusinessThread& operator=(const BusinessThread&) = delete;

    // 禁用移动（因为有引用成员）
    BusinessThread(BusinessThread&&) = delete;
    BusinessThread& operator=(BusinessThread&&) = delete;

    /**
     * @brief 启动业务线程
     * @param object_manager DistributedObjectManager 引用（由 IpcContext 传入）
     */
    void Start(DistributedObjectManager& object_manager);

    /**
     * @brief 停止业务线程
     *
     * 调用 inbound_.Shutdown() 然后 join 线程
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
     * @brief 检查业务线程是否正在运行
     * @return true 如果线程正在运行
     */
    bool IsRunning() const;

    /**
     * @brief Check if the calling thread is the BusinessThread
     * @return true if called from the BusinessThread's thread of execution
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

    /// DistributedObjectManager 指针（由 Start() 设置）
    DistributedObjectManager* object_manager_ = nullptr;

    /// 业务线程
    std::thread thread_;

    /// 运行状态
    std::atomic<bool> running_{false};
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_BUSINESS_THREAD_H
