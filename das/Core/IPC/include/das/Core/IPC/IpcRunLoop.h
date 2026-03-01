#ifndef DAS_CORE_IPC_IPC_RUN_LOOP_H
#define DAS_CORE_IPC_IPC_RUN_LOOP_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/IDasBase.h>
#include <functional>
#include <queue>

#include <memory>
#include <mutex>
#include <stdexec/execution.hpp>
#include <thread>
#include <unordered_map>
#include <vector>

#include <das/Core/IPC/Config.h>

// Forward declarations
DAS_CORE_IPC_NS_BEGIN
// Forward declarations
class IMessageHandler;
class IpcTransport;

namespace Host
{
    class HandshakeHandler;
}
DAS_CORE_IPC_NS_END

DAS_CORE_IPC_NS_BEGIN
struct NestedCallContext
{
    uint64_t             call_id;
    std::vector<uint8_t> response_buffer;
    bool                 completed;
};

/// 投递的回调类型
using PostedCallback = std::function<void()>;

// 内部类型，不对外导出
class IpcRunLoop
{
public:
    IpcRunLoop();
    ~IpcRunLoop();

    DasResult Initialize();
    DasResult Shutdown();
    // 阻塞式消息循环
    DasResult Run();

    DasResult Stop();

    /**
     * @brief 仅设置 running_ 标志为 false，不 join 线程
     *
     * 用于在 io_thread_ 内部（如 GOODBYE 回调中）安全请求退出，
     * 避免在 io_thread_ 上调用 join 导致死锁。
     * 线程的 join 由 Run() 或 Stop() 完成。
     */
    void RequestStop();

    void SetTransport(std::unique_ptr<IpcTransport> transport);

    IpcTransport* GetTransport() const;

    // 等待消息循环结束
    DasResult WaitForShutdown();

    /**
     * @brief 注册消息处理器
     * @param handler 处理器实例（所有权转移）
     */
    void RegisterHandler(std::unique_ptr<IMessageHandler> handler);

    /**
     * @brief 按接口 ID 查找处理器
     * @param interface_id 接口 ID
     * @return 处理器指针，未找到返回 nullptr
     */
    [[nodiscard]]
    IMessageHandler* GetHandler(uint32_t interface_id) const;

    /**
     * @brief 同步阻塞 IPC 调用（使用内部 transport）
     *
     * 发送请求后进入消息循环等待，支持可重入调用。
     * 内部使用 ReceiveAndDispatch() 处理消息。
     *
     * @param request_header 请求头
     * @param body 请求体
     * @param body_size 请求体大小
     * @param response_body [out] 响应体
     * @param timeout 超时时间（默认30秒）
     * @return 调用结果
     */
    DasResult SendRequest(
        const IPCMessageHeader&   request_header,
        const uint8_t*            body,
        size_t                    body_size,
        std::vector<uint8_t>&     response_body,
        std::chrono::milliseconds timeout = std::chrono::seconds(30));

    /**
     * @brief 同步阻塞 IPC 调用（指定 transport）
     *
     * 用于主进程转发场景：可以在处理 A 的消息时，
     * 使用 Transport_B 发送请求到 Host B。
     *
     * @param transport 指定使用的传输层
     * @param request_header 请求头
     * @param body 请求体
     * @param body_size 请求体大小
     * @param response_body [out] 响应体
     * @param timeout 超时时间（默认30秒）
     * @return 调用结果
     */
    DasResult SendRequest(
        IpcTransport*             transport,
        const IPCMessageHeader&   request_header,
        const uint8_t*            body,
        size_t                    body_size,
        std::vector<uint8_t>&     response_body,
        std::chrono::milliseconds timeout = std::chrono::seconds(30));



    /**
     * @brief 异步 IPC 调用（返回 sender）
     *
     * 返回可 co_await 的 sender，支持协程调用。
     *
     * @param request_header 请求头
     * @param body 请求体
     * @param body_size 请求体大小
     * @return stdexec::sender 包含 pair<DasResult, vector<uint8_t>>
     */
    [[nodiscard]]
    stdexec::sender auto SendMessageAsync(
        const IPCMessageHeader& request_header,
        const uint8_t*          body,
        size_t                  body_size);

    DasResult SendResponse(
        const IPCMessageHeader& response_header,
        const uint8_t*          body,
        size_t                  body_size);

    DasResult SendEvent(
        const IPCMessageHeader& event_header,
        const uint8_t*          body,
        size_t                  body_size);

    bool IsRunning() const;

    //=========================================================================
    // PostMessage API
    //=========================================================================

    /// 投递一个回调到 RunLoop 线程执行
    /// 线程安全，可从任意线程调用
    /// @param callback 要执行的回调
    void PostMessage(PostedCallback callback);

    /// 投递"启动 Host"任务
    /// RunLoop 会管理 HostLauncher 的生命周期
    /// @param plugin_path 插件 manifest 路径
    /// @param on_complete 完成回调（可选）
    void PostStartHost(
        const std::string&                                         plugin_path,
        std::function<void(DasResult result, uint16_t session_id)> on_complete =
            nullptr);

    /// 投递"停止 Host"任务
    /// @param session_id 要停止的 Host 的 session_id
    void PostStopHost(uint16_t session_id);
    friend class ::Das::Core::IPC::Host::HandshakeHandler;

    //=========================================================================
    // PostMessage 内部实现
    //=========================================================================

    /// 发送唤醒消息（内部使用）
    void SendWakeupMessage();

    /// 处理投递的回调（由 HandshakeHandler 调用）
    void ProcessPostedCallbacks();

    DasResult ProcessMessage(
        const IPCMessageHeader& header,
        const uint8_t*          body,
        size_t                  body_size);

    /**
     * @brief 分发消息到注册的处理器
     */
    DasResult DispatchToHandler(
        const IPCMessageHeader&     header,
        const std::vector<uint8_t>& body);

    /**
     * @brief 内部 receive 方法 - 核心可重入逻辑
     */
    bool ReceiveAndDispatch(std::chrono::milliseconds timeout);

    /**
     * @brief 使用指定 transport 的 receive 方法 - 用于转发场景
     *
     * 在主进程转发场景中，需要在处理 Transport_A 的消息时，
     * 使用 Transport_B 发送请求。此方法允许指定 transport 接收消息。
     *
     * @param transport 指定的传输层
     * @param timeout 超时时间
     * @return 是否收到并处理了消息
     */
    bool ReceiveAndDispatchFromTransport(
        IpcTransport*           transport,
        std::chrono::milliseconds timeout);



    void RunInternal();

    // 直接成员（移除 pimpl）
    std::unordered_map<uint64_t, NestedCallContext> pending_calls_;
    std::unique_ptr<IpcTransport>                   transport_;

    std::unordered_map<uint32_t, std::unique_ptr<IMessageHandler>> handlers_;
    std::atomic<uint64_t>  next_call_id_{1};
    std::atomic<bool>      running_{false};
    std::atomic<DasResult> exit_code_{DAS_S_OK};
    std::thread            io_thread_;
    std::mutex             pending_mutex_;

    //=========================================================================
    // PostMessage 成员
    //=========================================================================

    /// 投递回调队列
    std::mutex                 post_queue_mutex_;
    std::queue<PostedCallback> post_queue_;

    /// 管理的 Host 进程（预留，HostLauncher 在 Wave 5 实现）
    std::mutex hosts_mutex_;
    std::unordered_map<uint16_t, void*>
        hosts_; // 先用 void*，后续替换为 HostLauncher*
};
DAS_CORE_IPC_NS_END

// stdexec 方法实现（必须在头文件中，因为返回 auto 类型）
DAS_CORE_IPC_NS_BEGIN
inline stdexec::sender auto IpcRunLoop::SendMessageAsync(
    const IPCMessageHeader& request_header,
    const uint8_t*          body,
    size_t                  body_size)
{
    // 使用同步实现包装
    std::vector<uint8_t> response;
    DasResult result = SendRequest(request_header, body, body_size, response);
    return stdexec::just(std::make_pair(result, std::move(response)));
}
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_RUN_LOOP_H
