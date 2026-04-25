#pragma once

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <das/DasApi.h>
#include <memory>
#include <vector>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

// 前向声明
class IpcResponseSender;
class ValidatedIPCMessageHeader;
class DistributedObjectManager;
class ProxyFactory;
class RemoteObjectRegistry;
class IpcRunLoop;
class BusinessThread;

/**
 * @brief IO-thread context for CONTROL_PLANE handlers.
 *
 * CONTROL_PLANE handlers run on the IPC IO coroutine domain and may only
 * perform transport/session/handshake work. They must not access
 * BusinessThread-owned state: DistributedObjectManager, RemoteObjectRegistry,
 * ProxyFactory, proxy cache, BusinessThread, or BT TLS-dependent
 * Registry/DOM resolution.
 *
 * If a CONTROL_PLANE operation truly needs BT-owned state and cannot be
 * fire-and-forget, it must be modeled as an awaitable/coroutine operation
 * that sends an explicit response. It must not synchronously block the IO
 * thread, and must not use a default-constructed result to feign success.
 *
 * @note Execution domain: IPC IO thread (coroutine context only).
 *       Synchronous semantics: handler runs inside a co_await; no sync wait
 *       on other threads is permitted.
 *       Phase 52 position: this context replaces StubContext usage on the
 *       CONTROL_PLANE IO path, enforcing the thread-model boundary.
 */
struct ControlPlaneContext
{
    IpcRunLoop&                      run_loop;
    const ValidatedIPCMessageHeader& header;
};

/**
 * @brief BusinessThread context for business message handlers.
 *
 * 打包 BusinessThread/business handler
 * 所需的运行时引用，避免虚函数签名参数膨胀。
 * 新增字段只需修改此结构体，无需修改虚函数签名。
 *
 * This context is only valid on the BusinessThread execution domain.
 * BUSINESS_CONTROL and BUSINESS_EVENT handlers receive this context via
 * the inbound_queue_ -> BusinessThread::ProcessInboundMessage path.
 *
 * @note Execution domain: BusinessThread only.
 *       Synchronous semantics: handler runs synchronously on BT;
 *       nested PumpUntilResponse is supported for blocking RESPONSE waits.
 *       Phase 52 position: the sole domain that may access DOM/Registry/proxy.
 *       CONTROL_PLANE handlers must NOT receive this context.
 */
struct StubContext
{
    StubContext(
        DistributedObjectManager&        obj_mgr,
        RemoteObjectRegistry&            reg,
        IpcRunLoop&                      rl,
        std::weak_ptr<BusinessThread>    bt,
        ProxyFactory&                    pf,
        const ValidatedIPCMessageHeader& hdr)
        : object_manager(obj_mgr), registry(reg), run_loop(rl),
          business_thread(bt), proxy_factory(pf), header(hdr)
    {
    }

    DistributedObjectManager&        object_manager;
    RemoteObjectRegistry&            registry;
    IpcRunLoop&                      run_loop;
    std::weak_ptr<BusinessThread>    business_thread;
    ProxyFactory&                    proxy_factory;
    const ValidatedIPCMessageHeader& header;
    uint16_t                         response_flags =
        0; ///< 由 Handle* 方法设置，传递给响应头 flags 字段
};

/**
 * @brief 消息处理器接口
 *
 * 所有 IPC 消息处理器必须实现此接口。
 * 通过 RegisterHandler() 注册到 IpcRunLoop。
 */
class IMessageHandler
{
public:
    virtual ~IMessageHandler() = default;

    /**
     * @brief 增加引用计数
     * @return 新的引用计数
     */
    [[nodiscard]]
    virtual uint32_t AddRef() = 0;

    /**
     * @brief 减少引用计数
     * @return 新的引用计数
     */
    [[nodiscard]]
    virtual uint32_t Release() = 0;

    /**
     * @brief 获取处理器负责的接口 ID
     * @return 接口 ID（用于消息分发）
     */
    [[nodiscard]]
    virtual uint32_t GetInterfaceId() const = 0;

    /**
     * @brief 处理 IPC 消息（同步版本）
     * @param header 已验证的消息头
     * @param body 消息体
     * @param sender 响应发送器（用于发送响应）
     * @param ctx Stub 上下文（包含 object_manager、run_loop、business_thread）
     * @return DasResult 处理结果
     */
    virtual DasResult HandleMessage(
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body,
        IpcResponseSender&               sender,
        StubContext&                     ctx) = 0;
};

/**
 * @brief Awaitable (coroutine) message handler interface for CONTROL_PLANE.
 *
 * CONTROL_PLANE handlers (e.g. HandshakeHandler) use this interface.
 * They run on the IPC IO coroutine domain and receive ControlPlaneContext,
 * which provides only IO-safe references (IpcRunLoop, message header).
 * They must not access BusinessThread-owned DOM/Registry/proxy state.
 *
 * Note: Does not inherit IMessageHandler because HandleMessage return type
 * differs (awaitable<DasResult> vs DasResult). Implementations must provide
 * their own AddRef/Release for lifetime management.
 *
 * @note Execution domain: IPC IO thread (coroutine context).
 *       Synchronous semantics: co_await-based; no sync blocking permitted.
 *       Phase 52 position: replaces the previous StubContext& parameter
 *       to enforce the CONTROL_PLANE/BusinessThread boundary.
 */
class IAwaitableMessageHandler
{
public:
    virtual ~IAwaitableMessageHandler() = default;

    /**
     * @brief 增加引用计数
     * @return 新的引用计数
     */
    [[nodiscard]]
    virtual uint32_t AddRef() = 0;

    /**
     * @brief 减少引用计数
     * @return 新的引用计数
     */
    [[nodiscard]]
    virtual uint32_t Release() = 0;

    /**
     * @brief 获取处理器负责的接口 ID
     * @return 接口 ID（用于消息分发）
     */
    [[nodiscard]]
    virtual uint32_t GetInterfaceId() const = 0;

    /**
     * @brief Handle a CONTROL_PLANE IPC message (coroutine version).
     *
     * Runs on the IPC IO thread coroutine domain. The ControlPlaneContext
     * provides only IO-safe references: IpcRunLoop and message header.
     * Handlers must not access BusinessThread-owned DOM/Registry/proxy.
     *
     * @param header Validated message header.
     * @param body   Message body bytes.
     * @param sender Response sender (contains transport for async response).
     * @param ctx    IO-safe control-plane context (run_loop + header).
     * @return boost::asio::awaitable<DasResult> Coroutine result.
     */
    virtual boost::asio::awaitable<DasResult> HandleMessage(
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body,
        IpcResponseSender&               sender,
        ControlPlaneContext&             ctx) = 0;
};

DAS_CORE_IPC_NS_END
