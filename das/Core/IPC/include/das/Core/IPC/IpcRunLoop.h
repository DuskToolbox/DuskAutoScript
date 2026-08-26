#ifndef DAS_CORE_IPC_IPC_RUN_LOOP_H
#define DAS_CORE_IPC_IPC_RUN_LOOP_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcMessageQueue.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/Core/Utils/StdExecution.h>
#include <das/IDasAsyncCallback.h>
#include <das/IDasBase.h>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <das/Core/IPC/AnyTransport.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/DasPtr.hpp>

DAS_CORE_IPC_NS_BEGIN

class IMessageHandler;
class IAwaitableMessageHandler;
class ConnectionManager;
class HostLauncher;
class IHostConnection;
class IpcRunLoop; // Forward declaration for templates
class ProxyFactory;
class RemoteObjectRegistry;

namespace Host
{
    class HandshakeHandler;
}

DAS_CORE_IPC_NS_END

DAS_CORE_IPC_NS_BEGIN

/// 异步 pending call 的完成回调类型
/// @param result IPC 结果
/// @param response 响应体
/// @param response_flags 响应头 flags（MessageFlags）
using PendingCallCompletion = std::function<
    void(DasResult, std::vector<uint8_t>, uint16_t response_flags)>;

/// 发送目标 transport 解析器（构造注入、必填非可选）
///
/// 每个进程角色在装配期定死一份实现，运行期零判断：
/// - MainProcess 语义：两级查表（FindManagedHostConnection 命中取
///   host->GetTransport()，否则 FindHostLocalTransport(target)）；
/// - Host 语义：恒上行（忽略 target，每次调用现读 host-local 表中
///   main session 的上行 transport——重连/传输重建后闭包不持过期引用）。
/// SendToSessionCoroutine 对其无条件调用（无 if 包裹、无空态回退）；
/// 空 callable 在 IpcRunLoop 构造期被拒绝（fail-fast）。
using TransportResolver =
    std::function<TransportLookupResult(uint16_t target_session_id)>;

/// 用于匹配请求和响应的二元组 key
struct CallKey
{
    uint16_t source_session_id; // 消息来源 session
    uint16_t call_id;           // 请求/响应配对 ID

    bool operator==(const CallKey& other) const noexcept
    {
        return source_session_id == other.source_session_id
               && call_id == other.call_id;
    }
};

/// CallKey 哈希函数
struct CallKeyHash
{
    size_t operator()(const CallKey& k) const noexcept
    {
        // 组合成 32-bit 值作为 hash
        return (static_cast<size_t>(k.source_session_id) << 16)
               | static_cast<size_t>(k.call_id);
    }
};

/// 异步 pending call 状态
struct PendingCallState
{
    CallKey                               call_key;
    std::chrono::steady_clock::time_point deadline;
    PendingCallCompletion                 on_complete;
    uint16_t                              response_flags = 0; ///< 响应头 flags
};

/// 候车中的完整 ADD_REF 帧（每 (对象×对端) 一条；入队/直发两出口共用
/// 同一帧构造——单一事实源在 IpcRunLoop::BuildAddRefFrame）
struct PendingAddRefFrame
{
    ValidatedIPCMessageHeader header;
    std::vector<uint8_t>      body;
};

/// 按 target session 的候车状态：帧队列 + 在途出站发送计数
///
/// 在途计数是闸3 出清的豁免依据：投递中的出站发送必经搭车 helper 查队
/// （合格载体搭走 / 不合格载体先走），周期末 guard 对存在在途发送的
/// session 豁免出清——同周期内的响应（[in] 天然载体）确定性地先于
/// guard 取走 riders，消除两者并发竞速的帧数漂移。
struct PendingAddRefSessionState
{
    std::deque<PendingAddRefFrame> frames;

    /// 该 session 的在途出站发送数（发送入口投递前递增、发送协程任何
    /// 退出路径递减——RAII 配对）
    size_t in_flight_sends = 0;
};

//=============================================================================
// AwaitResponseSender — 真正的异步 IPC 响应等待 sender
//=============================================================================

/**
 * @brief AwaitResponseSender 的 OperationState
 *
 * start() 时调用 PostSend（带回调），由事件循环驱动完成（零轮询）。
 * pending_calls_ 的注册在 IO 线程上完成，并且发生在发送之前。
 */
template <class Receiver>
struct AwaitResponseOperation
{
    IpcRunLoop*               loop_;
    ValidatedIPCMessageHeader header_;
    std::vector<uint8_t>      body_;
    CallKey                   call_key_;
    std::chrono::milliseconds timeout_;
    Receiver                  rcvr_;
};

// tag_invoke 实现在 IpcRunLoop 完整定义之后（见文件末尾），避免 clang
// 对不完整类型的检查

/**
 * @brief 异步等待 IPC 响应的 sender
 *
 * 完成时携带 tuple<DasResult, vector<uint8_t>, uint16_t flags>。
 * 不阻塞调用线程，由消息循环事件驱动完成。
 */
struct AwaitResponseSender
{
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t(
            std::tuple<DasResult, std::vector<uint8_t>, uint16_t>)>;

    IpcRunLoop*               loop_;
    ValidatedIPCMessageHeader header_;
    std::vector<uint8_t>      body_;
    CallKey                   call_key_;
    std::chrono::milliseconds timeout_;

    template <class Receiver>
    friend auto tag_invoke(
        stdexec::connect_t,
        AwaitResponseSender self,
        Receiver            rcvr) noexcept -> AwaitResponseOperation<Receiver>
    {
        return {
            self.loop_,
            self.header_,
            std::move(self.body_),
            self.call_key_,
            self.timeout_,
            std::move(rcvr)};
    }
};

//=============================================================================
// IpcRunLoop — 异步 IPC 运行时
//=============================================================================

class IpcRunLoop
{
public:
    /**
     * @brief RAII 构造函数
     *
     * 构造即完成初始化：创建 io_context、ConnectionManager，注册所有 IPC stub
     * handlers。
     *
     * @param enable_heartbeat 是否启用心跳线程（调试时可禁用，避免超时杀进程）
     * @param inbound_queue 入站消息队列指针（非持有，由 IpcContext 管理），默认
     * nullptr
     * @param proxy_factory ProxyFactory 引用（由 IpcContext 持有）
     * @param registry RemoteObjectRegistry 引用（由 IpcContext 持有）
     * @param transport_resolver 发送目标解析器（必填，无默认值——不传即编译
     * 失败）。两角色形态：MainProcess 传两级查表实现（lambda 捕获 this、
     * 调用期现读 GetConnectionManager()）；Host 传恒上行实现（忽略 target、
     * 调用期现读 FindHostLocalTransport(main session)）。空 callable 构造期
     * 抛 std::invalid_argument（fail-fast）。
     */
    explicit IpcRunLoop(
        bool                             enable_heartbeat,
        IpcMessageQueue<InboundMessage>* inbound_queue,
        ProxyFactory&                    proxy_factory,
        RemoteObjectRegistry&            registry,
        TransportResolver                transport_resolver);

    ~IpcRunLoop();

    // 阻塞式消息循环
    DasResult Run();

    /**
     * @brief 仅设置 running_ 标志为 false，不 join 线程
     *
     * 用于在事件循环线程内部（如 GOODBYE 回调中）安全请求退出，
     * 避免在事件循环线程上调用 join 导致死锁。
     * 线程的 join由调用方完成。
     */
    void RequestStop();

    /// @brief 失败并移除指定 session 的所有 pending call（host
    /// 断连/崩溃时调用）
    ///
    /// host 进程退出或 IPC 连接断开时，该 session 上等待响应的 pending call
    /// 永远收不到回包，调用方（如 HTTP handler）会死等。此方法主动 fail 这些
    /// pending，回传 error。复用 RequestStop 的 fail 逻辑，按 source_session_id
    /// 过滤，不影响其他 session。
    ///
    /// @param session_id 崩溃/断连的对端 host session_id
    /// @param error 回传给调用方的错误码（通常 DAS_E_IPC_CONNECTION_LOST）
    /// @return 被 fail 的 pending call 数量
    size_t FailPendingCallsBySession(uint16_t session_id, DasResult error);

    /// Host 会话断连回调类型（参数 = 断连对端的 session_id）
    using HostSessionDisconnectedCallback =
        std::function<void(uint16_t session_id)>;

    /**
     * @brief 设置 host 会话断连回调（可选挂载）
     *
     * 内部 host 接收循环退出（对端崩溃/断连/正常停机——任何退出路径）
     * 时，在 fail 完该 session 的 pending call 之后、于 IO 线程触发一次。
     * 未注册时退出路径零行为变化。须在装配期（Run() 之前）设置。
     *
     * @param callback 断连回调（传空清除）
     */
    void SetOnHostSessionDisconnected(HostSessionDisconnectedCallback callback);

    /**
     * @brief 注册消息处理器（通过 AddRef 管理生命周期）
     *
     * IpcRunLoop 通过 DasPtr 持有 handler 引用（内部调用 AddRef）。
     * 调用方仍持有原始所有权。
     *
     * @param header_flags 消息头标志
     * @param interface_id 接口 ID
     * @param handler 处理器指针（IpcRunLoop 通过 AddRef 延长生命周期）
     */
    void RegisterHandler(
        uint8_t          header_flags,
        uint32_t         interface_id,
        IMessageHandler* handler);

    /**
     * @brief 注册可等待消息处理器（协程版本，控制平面 handler 用）
     *
     * 控制平面 handler（如 HandshakeHandler）使用此接口注册，
     * 支持通过协程异步发送响应。
     * IpcRunLoop 通过 DasPtr 持有 handler 引用（内部调用 AddRef）。
     *
     * @param header_flags 消息头标志
     * @param interface_id 接口 ID
     * @param handler 处理器指针（IpcRunLoop 通过 AddRef 延长生命周期）
     */
    void RegisterHandler(
        uint8_t                   header_flags,
        uint32_t                  interface_id,
        IAwaitableMessageHandler* handler);

    /**
     * @brief 按 header_flags + interface_id 查找处理器
     * @param header_flags 消息头标志
     * @param interface_id 接口 ID
     * @return 处理器指针，未找到返回 nullptr
     */
    [[nodiscard]]
    IMessageHandler* GetHandler(uint8_t header_flags, uint32_t interface_id)
        const;

    /**
     * @brief 无 transport 的 SendMessageAsync 已删除
     *
     * IpcRunLoop 不再持有内部 transport。
     * 调用者必须使用带 transport 参数的版本。
     */
    [[nodiscard]]
    stdexec::sender auto SendMessageAsync(
        const ValidatedIPCMessageHeader& request_header,
        const uint8_t*                   body,
        size_t                           body_size,
        std::chrono::milliseconds timeout = std::chrono::seconds(30)) = delete;

    /**
     * @brief 使用指定 transport 的异步 IPC 调用
     *
     * 用于转发消息到其他进程时，使用外部 transport 发送。
     *
     * @param transport 目标传输层（生命周期绑定到返回值）
     * @param request_header 请求头
     * @param body 请求体
     * @param body_size 请求体大小
     * @param timeout 超时时间（默认30秒）
     * @return stdexec::sender 包含 pair<DasResult, vector<uint8_t>>
     */
    [[nodiscard]]
    stdexec::sender auto SendMessageAsync(
        AnyTransport& transport          DAS_LIFETIMEBOUND,
        const ValidatedIPCMessageHeader& request_header,
        const uint8_t*                   body,
        size_t                           body_size,
        std::chrono::milliseconds        timeout = std::chrono::seconds(30));

    bool IsRunning() const;

    /**
     * @brief 设置本地 session_id
     *
     * 在 Host 模式下，握手完成后需要更新本地 session_id，
     * 以确保发送请求时使用正确的 source_session_id。
     *
     * @param session_id 新的本地 session_id
     */
    void SetSessionId(uint16_t session_id);

    /// @brief 获取本地 session_id
    /// @return 本地 session_id
    [[nodiscard]]
    uint16_t GetSessionId() const noexcept
    {
        return local_session_id_;
    }

    /**
     * @brief 设置 piggyback 搭车总闸（默认开）
     *
     * 唯一闸门位于物化点分叉（GetOrCreateProxy 的 BT 支路）：关闭时一切
     * ADD_REF 物化走 PostAddRefStandalone 当场直发（完全等价于关闭前的
     * 独立帧行为），pending 队列恒空，下游出清机制因队列恒空自然 no-op。
     * 测试/调试旋钮（D-21 帧数基线 A/B 对比的依赖件），不进产品配置面。
     *
     * @param enabled 是否启用搭车（物化点 BT 入队候车）
     */
    void SetPiggybackEnabled(bool enabled);

    /// @brief 查询 piggyback 搭车总闸状态
    [[nodiscard]]
    bool IsPiggybackEnabled() const noexcept;

    /// 获取 io_context 引用（用于 HostLauncher 等需要共享 io_context 的场景）
    /// 注意：构造完成后可用
    /// @return io_context 引用，生命周期绑定到 this
    boost::asio::io_context& GetIoContext() DAS_LIFETIMEBOUND
    {
        return *io_context_;
    }

    /**
     * @brief 获取 ConnectionManager
     *
     * ConnectionManager 由构造函数无条件创建：MainProcess 侧用于管理
     * HostLauncher / internal host 连接；Host 侧用于登记上行 host-local
     * transport（main session）。
     *
     * @return ConnectionManager& 引用，生命周期绑定到 this
     */
    ConnectionManager& GetConnectionManager() DAS_LIFETIMEBOUND
    {
        return *connection_manager_;
    }

    /**
     * @brief 投递发送任务到 IO 线程（业务线程调用）
     *
     * 线程安全，任何线程可调用。
     * 将发送任务投递到 IO 线程执行。
     * 如果提供了 on_complete，IO 线程会在发送前注册 pending call，
     * 避免响应先于 pending call completion 挂载而到达。
     *
     * @param header 消息头
     * @param body 消息体（会被 move）
     * @param on_complete 可选的完成回调（非空时注册 pending call）
     * @param deadline 超时截止时间（仅 on_complete 非空时有效）
     * @return DasResult 投递结果
     */
    DasResult PostSend(
        const ValidatedIPCMessageHeader&      header,
        std::vector<uint8_t>&&                body,
        PendingCallCompletion                 on_complete = nullptr,
        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max());

    /**
     * @brief 投递发送任务到 IO 线程（指定 connected-host owner）
     *
     * 协程捕获 IHostConnection owner guard，并在协程内部临时取得
     * AnyTransport 引用后发送，避免裸 transport 引用逃逸到 detached
     * coroutine。
     *
     * @param connection connected-host owner guard
     * @param header 消息头
     * @param body 消息体（会被 move）
     * @return DasResult 投递结果
     */
    // clang-format off
    DasResult PostSendWithTransport(DasPtr<IHostConnection> connection,
                                    const ValidatedIPCMessageHeader& header,
                                    std::vector<uint8_t>&& body);
    // clang-format on

    //=========================================================================
    // ADD_REF 候车与出清（段 B 基建：D-12 闸3 出清 + D-19 分叉的承接面）
    //=========================================================================

    /**
     * @brief 把一条完整 ADD_REF 帧加入 pending 候车队列（叶子操作）
     *
     * 物化点在 proxy_cache_mutex_ 持锁区内调用——本方法自带 mutex、
     * 零回调零 IO：锁内仅帧构造与入队（锁序单向 proxy_cache_mutex_ →
     * addref_pending_mutex_）。按 target session（owner）键控。
     *
     * @param object_id 远端对象 id（target = object_id.session_id）
     */
    void EnqueueAddRef(const ObjectId& object_id);

    /**
     * @brief 把一条完整 RELEASE 帧加入出站 FIFO
     *
     * DasProxyBase 析构的唯一 RELEASE 出口（替代直接 PostSend）：
     * fire-and-forget 语义不变，owner 侧条目拆除延迟上界 = 一个
     * dispatch 周期。触发矩阵：BT 线程入队不触发（同周期 Response
     * 带走）；非 BT 线程入队即触发（无 guard 兜底，滞留上界不能退化
     * 为心跳周期）。
     *
     * @param object_id 远端对象 id（target = object_id.session_id）
     */
    void EnqueueReleaseFrame(const ObjectId& object_id);

    /**
     * @brief 当场构帧独立直发一条 ADD_REF（段 A 行为的单一事实源）
     *
     * 非 BT 线程物化（外部线程/IO/weak_ptr 过期）或 piggyback 关闭时的
     * 出口：零延迟零窗口，不经队列不待周期。
     *
     * @param object_id 远端对象 id
     * @return DasResult PostSend 投递结果（fire-and-forget，失败不回滚）
     */
    DasResult PostAddRefStandalone(const ObjectId& object_id);

    /**
     * @brief 取走全部候车队列帧并逐帧独立 PostSend（闸3 出清）
     *
     * 周期末（AddRefDispatchGuard 析构）与测试直驱共用。每 session 内
     * FIFO；帧数不变（逐帧独立形态，无合并不捎带）。存在在途出站发送
     * 的 session 豁免（其 riders 由在途发送的搭车 helper 带走——同周期
     * 响应与 guard 的并发竞速由此消除）。
     *
     * @return size_t 本次出清的帧数
     */
    size_t FlushPendingStandalone();

    /**
     * @brief 清空指定 session 的候车队列（断连清队）
     *
     * 连接断开后该 session 的 pending 帧不再有可达载体，随断连清空
     * 防滞留（fire-and-forget 语义下迟到帧本就无害）。
     *
     * @param session_id 断连的对端 session
     * @return size_t 清掉的帧数
     */
    size_t ClearPendingAddRefs(uint16_t session_id);

    /// @brief 查询指定 session 的候车帧数（测试观测访问器）
    [[nodiscard]]
    size_t GetPendingAddRefCount(uint16_t session_id) const;

    /// @brief 候车队列是否为空（测试观测访问器）
    [[nodiscard]]
    bool PendingAddRefsEmpty() const;

    /// @brief 出站物理写计数（测量 seam：每次 transport 物理写成功递增）
    [[nodiscard]]
    std::uint64_t GetOutboundPhysicalWriteCount() const noexcept
    {
        return outbound_physical_writes_.load(std::memory_order_relaxed);
    }

    /// @brief 出站逻辑帧计数（测量 seam：连写批内按帧数累计）
    [[nodiscard]]
    std::uint64_t GetOutboundLogicalFrameCount() const noexcept
    {
        return outbound_logical_frames_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Fire-and-forget enqueue to BusinessThread.
     *
     * Wraps callback in an ASYNC_CALLBACK inbound message and pushes it to
     * the BusinessThread queue via the existing inbound_queue_ channel.
     * This reports only immediate enqueue failure; it does not wait for
     * or report callback execution result.
     *
     * @param callback IDasAsyncCallback to execute on BusinessThread
     * @return DasResult DAS_S_OK on success, error on enqueue failure
     *
     * @thread_safety May be called from any thread that owns a proxy
     * reference.
     * @architecture Bridges non-BT proxy teardown to the BusinessThread
     * runtime domain without exposing DOM/Registry to the caller.
     */
    DasResult PostToBusinessThread(IDasAsyncCallback* callback) noexcept;

    /**
     * @brief 注册 HostLauncher 并启动接收循环
     *
     * 在 HostLauncher::Start() 成功后调用。
     * Transport 将在注册后立即开始接收消息。
     *
     * @param launcher HostLauncher 实例（DasPtr 内部用 DasPtr 管理生命周期）
     * @return DasResult DAS_S_OK 成功
     */
    DasResult RegisterHostLauncher(DasPtr<HostLauncher> launcher);

    /**
     * @brief 注册内部 Host 并启动接收循环
     *
     * 用于 MainProcess 管理的已连接 host，例如后续 HTTP accept path 创建的
     * HttpHost。不会创建新的 IO 线程，接收协程运行在现有 io_context_ 上。
     *
     * @param host IHostConnection 实例（DasPtr 保持 transport owner 存活）
     * @return DasResult DAS_S_OK 成功
     */
    DasResult RegisterInternalHost(DasPtr<IHostConnection> host);

    friend class ::Das::Core::IPC::Host::HandshakeHandler;

    // AwaitResponseOperation 需要访问内部方法
    template <class Receiver>
    friend struct AwaitResponseOperation;

    /**
     * @brief 分发消息到注册的处理器（协程版本）
     *
     * 使用 co_await 调用 handler->HandleMessage()，
     * 通过 transport 发送响应。
     *
     * @param header 消息头
     * @param body 消息体
     * @param transport 用于发送响应的 transport
     * @return boost::asio::awaitable<void>
     */
    boost::asio::awaitable<void> DispatchToHandlerCoroutine(
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body,
        AnyTransport&                    transport);

    boost::asio::awaitable<void> DispatchToHandlerCoroutine(
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body,
        DasPtr<IHostConnection>          connection);

    /**
     * @brief Route a received message through the shared IPC receive rules.
     *
     * Receive adapters own transport acquire/receive/disconnect behavior and
     * delegate header/body semantics here.
     *
     * @param header Message header
     * @param body Message body
     * @return boost::asio::awaitable<void>
     */
    boost::asio::awaitable<void> RouteIncomingMessage(
        const ValidatedIPCMessageHeader& header,
        std::vector<uint8_t>             body);

    /**
     * @brief Route a received message with a managed-host owner guard.
     *
     * Used by MainProcess managed-host receive adapters so response sends and
     * forwards can retain the connected-host owner across async work.
     *
     * @param header Message header
     * @param body Message body
     * @param connection Managed host owner guard
     * @return boost::asio::awaitable<void>
     */
    boost::asio::awaitable<void> RouteIncomingMessage(
        const ValidatedIPCMessageHeader& header,
        std::vector<uint8_t>             body,
        DasPtr<IHostConnection>          connection);

    //=========================================================================
    // 事件驱动方法（内部使用）
    //=========================================================================

    /**
     * @brief 为内部 Host 启动异步接收循环
     *
     * 协程会捕获 host 的 DasPtr 以保持 AnyTransport owner 跨 co_await 存活。
     *
     * @param session_id 会话 ID
     * @param host IHostConnection 的 DasPtr
     */
    void StartAsyncReceiveForInternalHost(
        uint16_t                session_id,
        DasPtr<IHostConnection> host);

    /**
     * @brief 内部 Host 的接收协程循环
     *
     * @param session_id 会话 ID
     * @param host IHostConnection 的 DasPtr
     */
    boost::asio::awaitable<void> InternalHostReceiveLoop(
        uint16_t                session_id,
        DasPtr<IHostConnection> host);

    /**
     * @brief 调度超时检查定时器
     *
     * 寐隔一段时间检查 pending senders 是否超时。
     */
    void ScheduleTimeoutCheck();

    /**
     * @brief 为异步发送准备请求头和 CallKey（不发送、不注册 pending call）
     *
     * 分配 call_id，构建完整的 ValidatedIPCMessageHeader。
     * AwaitResponseSender::start() 随后会通过 PostSend 在 IO 线程上
     * 先注册 completion，再执行发送。
     *
     * @param request_header 原始请求头模板
     * @return pair<ValidatedIPCMessageHeader, CallKey>
     */
    std::pair<ValidatedIPCMessageHeader, CallKey> PrepareAsyncSendHeader(
        const ValidatedIPCMessageHeader& request_header);

    /**
     * @brief 使用指定 transport 准备发送请求
     *
     * @param transport 目标传输层
     * @param request_header 原始请求头
     * @param body 请求体
     * @param body_size 请求体大小
     * @return pair<DasResult, CallKey>: 结果码和分配的 CallKey
     */
    std::pair<DasResult, CallKey> PrepareSendRequestWithTransport(
        AnyTransport&                    transport,
        const ValidatedIPCMessageHeader& request_header,
        const uint8_t*                   body,
        size_t                           body_size);

    /**
     * @brief 完成指定 CallKey 的 pending call
     *
     * 从 pending_calls_ 中取出 on_complete 回调并调用，
     * 在 mutex 外执行回调以避免死锁。
     *
     * @param call_key CallKey (source_session_id, call_id)
     * @param result 结果码
     * @param response 响应体
     */
    void CompletePendingCall(
        CallKey              call_key,
        DasResult            result,
        std::vector<uint8_t> response,
        uint16_t             response_flags);

    bool TryCompletePendingCallbackOnly(
        CallKey               call_key,
        DasResult             result,
        std::vector<uint8_t>& response,
        uint16_t              response_flags);

    /**
     * @brief 扫描并完成所有超时的 pending call
     *
     * 由 ScheduleTimeoutCheck 定时调用。
     * 检查所有 pending call 的 deadline，超时的调用
     * on_complete(DAS_E_IPC_TIMEOUT, {})。
     */
    void TickPendingSenders();

    /**
     * @brief 获取最近的 pending call 超时时间（毫秒）
     *
     * 用于 ScheduleTimeoutCheck 计算 timed_receive 的超时参数。
     * 如果没有 pending call，返回 0（无限等待）。
     *
     * @return 距离最近 deadline 的毫秒数，最小为 1
     */
    uint32_t GetNearestDeadlineMs() const;

    //=========================================================================
    // 成员变量
    //=========================================================================

    /// pending calls 映射
    std::unordered_map<CallKey, PendingCallState, CallKeyHash> pending_calls_;

    /// 本地 session_id（MainProcess=1, Host 从 Handshake 获取）
    uint16_t local_session_id_ = 0;

    // async_transport_ 已移除
    // IpcRunLoop 不再持有任何 transport，所有 transport 由外部管理
    // - MainProcess 模式：HostLauncher 持有 transport
    // - Host 模式：IpcContext 持有 transport

    /// io_context 用于驱动异步 I/O
    std::unique_ptr<boost::asio::io_context> io_context_;

    /// 超时计时器（用于 pending call 超时管理）
    std::unique_ptr<boost::asio::steady_timer> timeout_timer_;

    /// work guard 保持 io_context 运行（确保 Run() 阻塞直到 RequestStop()）
    using WorkGuard = boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>;
    std::optional<WorkGuard> work_guard_;

    /// 消息处理器映射（按 header_flags + interface_id 索引）
    std::unordered_map<
        uint8_t,
        std::unordered_map<uint32_t, DasPtr<IMessageHandler>>>
        handlers_by_flags_;

    /// 可等待消息处理器映射（协程版本，控制平面 handler 用）
    /// 使用 DasPtr 管理引用计数，控制平面 handler 使用协程直接发送响应
    std::unordered_map<
        uint8_t,
        std::unordered_map<uint32_t, DasPtr<IAwaitableMessageHandler>>>
        awaitable_handlers_;

    /// 下一个 call_id
    std::atomic<uint16_t> next_call_id_{1};

public:
    /// @brief 分配全局唯一的 call_id
    /// @return 16-bit call_id，0 表示无效（不会返回 0）
    /// @note 线程安全，使用 atomic 递增。溢出后从 1 重新开始。
    [[nodiscard]]
    uint16_t AllocateCallId() noexcept
    {
        uint16_t id = next_call_id_.fetch_add(1);
        // 溢出后从 1 重新开始（0 表示无效）
        if (id == 0)
        {
            id = next_call_id_.fetch_add(1);
            if (id == 0)
            {
                id = 1;
            }
        }
        return id;
    }

    /// 运行状态
    std::atomic<bool> running_{false};

    /// 是否启用心跳线程
    bool enable_heartbeat_ = true;

    /// piggyback 搭车总闸（默认开；测试/调试旋钮——物化点分叉的唯一
    /// 闸门，关闭即回退独立帧直发行为）
    bool piggyback_enabled_ = true;

    /// 退出码
    std::atomic<DasResult> exit_code_{DAS_S_OK};

    /// 出站物理写计数（测量 seam，恒开）：每次 transport 物理写成功递增。
    /// 测量纪律：热路径禁任何流式打点（每帧 stderr 写 +50~100us 污染
    /// 测量），只允许此类 atomic 计数 + 进程退出汇总
    std::atomic<std::uint64_t> outbound_physical_writes_{0};

    /// 出站逻辑帧计数（测量 seam）：每写出一条完整逻辑帧递增（连写批内
    /// 按帧数累计）——与物理写计数的比值即合写效率
    std::atomic<std::uint64_t> outbound_logical_frames_{0};

    /// pending_calls_ 的互斥锁
    mutable std::mutex pending_mutex_;

    /// ADD_REF 候车队列（按 target session / owner 键控，每 session FIFO）
    std::unordered_map<uint16_t, PendingAddRefSessionState> pending_add_refs_;

    /// pending_add_refs_ 的互斥锁（锁序单向：proxy_cache_mutex_ → 本锁）
    mutable std::mutex addref_pending_mutex_;

    /// host 会话断连回调（IO 线程触发；装配期经
    /// SetOnHostSessionDisconnected 设置）
    HostSessionDisconnectedCallback on_host_session_disconnected_;

    /// ConnectionManager（构造无条件创建：MainProcess 侧管理 host 连接，
    /// Host 侧登记上行 host-local transport）
    std::unique_ptr<ConnectionManager> connection_manager_;

    /// Last time SHM stale block cleanup was triggered
    std::chrono::steady_clock::time_point last_shm_cleanup_time_ =
        std::chrono::steady_clock::now();

    /// 入站消息队列指针（非持有，由 IpcContext 管理）
    /// 通过构造函数注入，不可变
    IpcMessageQueue<InboundMessage>* inbound_queue_ = nullptr;

    /// ProxyFactory 引用（非持有，由 IpcContext 管理）
    /// 用于传递给控制平面 handler 的 StubContext
    ProxyFactory& proxy_factory_;

    /// RemoteObjectRegistry 引用（非持有，由 IpcContext 管理）
    /// 用于传递给控制平面 handler 的 StubContext
    RemoteObjectRegistry& registry_;

    /// 发送目标解析器（构造注入、必填——空 callable 构造期拒绝；
    /// SendToSessionCoroutine 无条件调用，角色差异全部在装配期 lambda 内）
    TransportResolver transport_resolver_;

private:
    /// @brief 单一转发谓词：帧是否按收件地址应整帧透传
    ///
    /// 纯地址规则：自身地址已确立（非 0）且收件地址非未定址（0）、
    /// 非本地即透传——零消息类别项、零 source 项。自身地址未确立
    /// （握手 bootstrap 期，session 尚为 0）的接收方一律本地消费——
    /// 它还不知道自己的地址，无权把帧判给别人。握手/心跳的收件人恒
    /// 为直连对端（或 0），天然落本地消费；引用申报帧发给 owner 即转
    /// owner 是地址规则的天然结果。扩展段剥离判定（is_transit_frame）、
    /// CONTROL_PLANE 本地消费分支守卫与转发分支三处共用本谓词——
    /// 单一判定，无第二份分歧判断。
    [[nodiscard]]
    bool IsForwardableFrame(
        const ValidatedIPCMessageHeader& header) const noexcept;

    /// @brief 发送失败时构造失败 RESPONSE 并推入 inbound_queue_
    /// @param header 原始请求的 header（用于获取 call_id, session_id）
    /// @param error_code 失败原因的错误码
    void NotifySendFailure(
        const ValidatedIPCMessageHeader& header,
        DasResult                        error_code);

    /// @brief 构造一条完整 ADD_REF 帧（CONTROL_PLANE Event + 130 +
    /// Source=本地 + Target=oid.session_id + body=ObjectId 8B）
    ///
    /// 入队（EnqueueAddRef）与直发（PostAddRefStandalone）两出口共用的
    /// 单一事实源——字节与段 A 的物化申报完全一致。
    PendingAddRefFrame BuildAddRefFrame(const ObjectId& object_id);

    /// @brief 构造一条完整 RELEASE 帧（CONTROL_PLANE Event + 132 +
    /// Source=本地 + Target=oid.session_id + body=ObjectId 8B）
    ///
    /// 镜像 BuildAddRefFrame 的单一事实源形态——字节与 DasProxyBase
    /// 析构旧内联构造完全一致。
    PendingAddRefFrame BuildReleaseFrame(const ObjectId& object_id);

    /// @brief 双汇点共享的出站搭车 helper（D-12 v3：逻辑单点、物理双挂点）
    ///
    /// SendToSessionCoroutine 与 DoSendCoroutine 在 transport 解析后、
    /// SendCoroutine 调用前各调用一次。决策树（piggyback_enabled_ 为假时
    /// 直通零开销 = 段 A 行为）：
    /// - pending[本帧 target session] 为空 → 原样直发；
    /// - 本帧为 RELEASE / SHM 大消息帧 / 转发帧（A7：source 非本地的
    ///   原样透传帧）/ 拼完（header + body + 任一 rider）达到
    ///   LARGE_MESSAGE_THRESHOLD → 闸2 先走：pending 全部先独立发（FIFO）
    ///   再发本帧；
    /// - 合格载体 → 闸1 搭车：riders 塞本帧前缀一帧带走（外层 header 置
    ///   RESERVED_AS_SIZE 位、reserved = 段总字节、body_size 增段总长，
    ///   call_id/source/target 原样），溢出部分紧随独立发。
    /// riders 容器与宿主 body 均为本协程帧局部持有（存活至 async_write
    /// 完成，Pitfall 8）。
    boost::asio::awaitable<DasResult> PrepareOutboundFrame(
        AnyTransport&             transport,
        ValidatedIPCMessageHeader header,
        std::vector<uint8_t>      body);

    /// @brief 取走指定 session 的全部候车帧（锁内 move-out，FIFO 序保持）
    std::vector<PendingAddRefFrame> TakePendingAddRefs(uint16_t session_id);

    /// @brief 标记该 session 的一条在途出站发送（发送入口投递前调用）
    ///
    /// 周期末 guard 据此豁免该 session 的出清——在途发送必经搭车 helper
    /// 查队，riders 由它带走（搭车或先走）。piggyback 关闭时不标记
    /// （零开销直通；关闭态生产路径队列恒空）。
    void MarkOutboundSendInFlight(uint16_t session_id);

    /// @brief 在途出站发送计数递减（发送协程任何退出路径——RAII 配对）
    void UnmarkOutboundSendInFlight(uint16_t session_id);

    /// @brief 在途出站发送计数的 RAII 递减（协程帧局部——任何退出路径
    /// 析构成对递减，与发送入口的 MarkOutboundSendInFlight 配对）
    class InFlightOutboundSendScope
    {
    public:
        InFlightOutboundSendScope(IpcRunLoop& run_loop, uint16_t session_id);
        ~InFlightOutboundSendScope();

        InFlightOutboundSendScope(const InFlightOutboundSendScope&) = delete;
        InFlightOutboundSendScope& operator=(const InFlightOutboundSendScope&) =
            delete;

    private:
        IpcRunLoop& run_loop_;
        uint16_t    session_id_;
    };

    /// @brief 闸2 先走执行：riders 先独立发（FIFO）随后宿主帧
    ///
    /// CanBatchFrames 为真且合写总长低于阈值时 N 个 rider 帧 + 宿主帧合
    /// 一次 SendScatter（第一帧拆为 header 参数 + body 段、其余帧整帧为
    /// 段——SendScatter header 恒在线首的调用形态约定；段序 riders 在前
    /// 宿主在后 = FIFO）；否则逐帧 SendCoroutine（WS 分流契约）。rider
    /// 发送失败仅告警（fire-and-forget 语义），宿主帧结果如实返回。
    boost::asio::awaitable<DasResult> SendRidersThenHostStandalone(
        AnyTransport&                     transport,
        std::vector<PendingAddRefFrame>&& riders,
        ValidatedIPCMessageHeader&&       host_header,
        std::vector<uint8_t>&&            host_body);

    /// @brief 通过协程发送 IPC 消息（内部共用实现）
    ///
    /// 封装 SendCoroutine 调用、连接检查和错误处理。
    /// @param transport 目标传输层
    /// @param header 消息头
    /// @param body 消息体
    boost::asio::awaitable<void> DoSendCoroutine(
        DasPtr<IHostConnection>   connection,
        ValidatedIPCMessageHeader header,
        std::vector<uint8_t>      body);

    /// @brief 按 session_id 查找 transport 并发送（由 PostSend 委托）
    ///
    /// 从 ConnectionManager 查找 launcher/transport，发送前注册
    /// pending call（IO 线程内完成，消除响应早于注册的竞态）。
    ///
    /// @param header 消息头
    /// @param body 消息体
    /// @param on_complete 可选的完成回调（非空时注册 pending call）
    /// @param deadline 超时截止时间
    boost::asio::awaitable<void> SendToSessionCoroutine(
        ValidatedIPCMessageHeader             header,
        std::vector<uint8_t>                  body,
        PendingCallCompletion                 on_complete,
        std::chrono::steady_clock::time_point deadline);

    boost::asio::awaitable<void> DispatchToHandlerWithSender(
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body,
        IpcResponseSender&               sender);
};

//=============================================================================
// AwaitResponseOperation::tag_invoke 实现（需要 IpcRunLoop 完整定义）
//=============================================================================

template <class Receiver>
void tag_invoke(
    stdexec::start_t,
    AwaitResponseOperation<Receiver>& self) noexcept
{
    // 错误路径：loop_ 为 nullptr 表示构造时已检测到错误
    if (!self.loop_)
    {
        auto error_code = static_cast<DasResult>(self.call_key_.call_id);
        stdexec::set_value(
            std::move(self.rcvr_),
            std::make_tuple(error_code, std::vector<uint8_t>{}, uint16_t{0}));
        return;
    }

    auto deadline = std::chrono::steady_clock::now() + self.timeout_;

    // PostSend 带回调：IO 线程内注册 pending_calls_ 后再发送，
    // 消除响应早于 completion 挂载的竞态。
    static_cast<void>(self.loop_->PostSend(
        self.header_,
        std::move(self.body_),
        [rcvr = std::move(self.rcvr_)](
            DasResult            result,
            std::vector<uint8_t> response,
            uint16_t             response_flags) mutable
        {
            stdexec::set_value(
                std::move(rcvr),
                std::make_tuple(result, std::move(response), response_flags));
        },
        deadline));
}

//=============================================================================
// SendMessageAsync 实现（必须在头文件中，因为返回 auto 类型）
//=============================================================================

inline stdexec::sender auto IpcRunLoop::SendMessageAsync(
    AnyTransport&                    transport,
    const ValidatedIPCMessageHeader& request_header,
    const uint8_t*                   body,
    size_t                           body_size,
    std::chrono::milliseconds        timeout)
{
    (void)transport;

    // 分配 call_id 并构建完整的请求头（不发送、不注册 pending call）
    auto [validated_header, call_key] = PrepareAsyncSendHeader(request_header);

    // 构造 body vector，AwaitResponseSender::start() 会通过 PostSend
    // 在 IO 线程上注册 pending call 并发送
    std::vector<uint8_t> body_vec(body, body + body_size);

    return AwaitResponseSender{
        this,
        validated_header,
        std::move(body_vec),
        call_key,
        timeout};
}

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_RUN_LOOP_H
