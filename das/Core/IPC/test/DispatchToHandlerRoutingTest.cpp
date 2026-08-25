/**
 * @file DispatchToHandlerRoutingTest.cpp
 * @brief Tests verifying CONTROL_PLANE/BusinessThread context boundary.
 *
 * Validates that:
 * - ControlPlaneContext only has IO-safe fields (run_loop, header)
 * - StubContext retains BT-owned fields for business handlers
 * - IAwaitableMessageHandler accepts ControlPlaneContext (compile-time check)
 * - IMessageHandler retains StubContext (compile-time check)
 * - CONTROL_PLANE RemoteAddRef/RemoteRelease EVENT 分发留在 IO 协程域，
 *   不进 inbound_queue_（BusinessThread）
 */

#include <boost/asio/use_future.hpp>
#include <das/Core/IPC/AnyTransport.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/IHostConnection.h>
#include <das/Core/IPC/IMessageHandler.h>
#include <das/Core/IPC/InternalCallbackHandler.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcMessageQueue.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/IPC/RemoteRefHandler.h>
#include <das/DasPtr.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <type_traits>
#include <vector>

#include "MockDasObject.h"
#include "OutboundFrameProbe.h"

using namespace DAS;
using namespace DAS::Core::IPC;

// ============================================================================
// Compile-time verification: ControlPlaneContext has only IO-safe fields
// ============================================================================

/**
 * @brief ControlPlaneContext::run_loop is IpcRunLoop&
 */
TEST(DispatchToHandlerRoutingTest, ControlPlaneContext_RunLoopIsIpcRunLoopRef)
{
    EXPECT_TRUE(
        (std::is_same_v<decltype(ControlPlaneContext::run_loop), IpcRunLoop&>));
}

/**
 * @brief ControlPlaneContext::header is const ValidatedIPCMessageHeader&
 */
TEST(DispatchToHandlerRoutingTest, ControlPlaneContext_HeaderIsConstRef)
{
    EXPECT_TRUE((std::is_same_v<
                 decltype(ControlPlaneContext::header),
                 const ValidatedIPCMessageHeader&>));
}

/**
 * @brief sizeof(ControlPlaneContext) == sizeof two references (no extra fields)
 *
 * ControlPlaneContext should contain exactly two reference-sized members:
 * run_loop (IpcRunLoop&) and header (const ValidatedIPCMessageHeader&).
 * Any BT-owned fields would increase the size beyond this.
 */
TEST(DispatchToHandlerRoutingTest, ControlPlaneContext_SizeIsTwoReferences)
{
    // On 64-bit: two pointers = 16 bytes (references are pointer-sized)
    // On 32-bit: two pointers = 8 bytes
    constexpr size_t expected = sizeof(void*) * 2;
    EXPECT_EQ(sizeof(ControlPlaneContext), expected)
        << "ControlPlaneContext should contain exactly run_loop + header "
        << "(two reference-sized fields). Extra fields may indicate "
        << "BT-owned components leaking into the IO-safe context.";
}

// ============================================================================
// Compile-time verification: StubContext retains BT-owned fields
// ============================================================================

/**
 * @brief StubContext::object_manager is DistributedObjectManager&
 */
TEST(DispatchToHandlerRoutingTest, StubContext_HasObjectManager)
{
    EXPECT_TRUE((std::is_same_v<
                 decltype(StubContext::object_manager),
                 DistributedObjectManager&>));
}

/**
 * @brief StubContext::registry is RemoteObjectRegistry&
 */
TEST(DispatchToHandlerRoutingTest, StubContext_HasRegistry)
{
    EXPECT_TRUE((
        std::
            is_same_v<decltype(StubContext::registry), RemoteObjectRegistry&>));
}

/**
 * @brief StubContext::proxy_factory is ProxyFactory&
 */
TEST(DispatchToHandlerRoutingTest, StubContext_HasProxyFactory)
{
    EXPECT_TRUE(
        (std::is_same_v<decltype(StubContext::proxy_factory), ProxyFactory&>));
}

/**
 * @brief StubContext::business_thread is std::weak_ptr<BusinessThread>
 */
TEST(DispatchToHandlerRoutingTest, StubContext_HasBusinessThread)
{
    EXPECT_TRUE((std::is_same_v<
                 decltype(StubContext::business_thread),
                 std::weak_ptr<BusinessThread>>));
}

/**
 * @brief StubContext is significantly larger than ControlPlaneContext
 *
 * StubContext contains DOM, Registry, ProxyFactory, BusinessThread weak_ptr,
 * run_loop, header, and response_flags. It must be larger than the IO-safe
 * ControlPlaneContext which only has run_loop and header.
 */
TEST(DispatchToHandlerRoutingTest, StubContext_LargerThanControlPlaneContext)
{
    EXPECT_GT(sizeof(StubContext), sizeof(ControlPlaneContext))
        << "StubContext must be larger than ControlPlaneContext because it "
        << "carries BT-owned components (DOM, Registry, ProxyFactory, etc.)";
}

// ============================================================================
// IAwaitableMessageHandler signature verification (compile-time)
// ============================================================================

/**
 * @brief Mock IAwaitableMessageHandler using ControlPlaneContext
 *
 * If this compiles, the IAwaitableMessageHandler::HandleMessage signature
 * has been updated from StubContext& to ControlPlaneContext&.
 */
namespace
{
    struct MockAwaitableHandler : public IAwaitableMessageHandler
    {
        uint32_t ref_count_ = 0;

        [[nodiscard]]
        uint32_t AddRef() override
        {
            return ++ref_count_;
        }

        [[nodiscard]]
        uint32_t Release() override
        {
            if (--ref_count_ == 0)
            {
                delete this;
                return 0;
            }
            return ref_count_;
        }

        [[nodiscard]]
        uint32_t GetInterfaceId() const override
        {
            return 0;
        }

        boost::asio::awaitable<DasResult> HandleMessage(
            const ValidatedIPCMessageHeader& /*header*/,
            const std::vector<uint8_t>& /*body*/,
            IpcResponseSender& /*sender*/,
            ControlPlaneContext& ctx) override
        {
            (void)ctx;
            co_return DAS_S_OK;
        }
    };
} // anonymous namespace

/**
 * @brief MockAwaitableHandler can be instantiated (signature check passed)
 */
TEST(DispatchToHandlerRoutingTest, AwaitableHandler_AcceptsControlPlaneContext)
{
    auto* handler = new MockAwaitableHandler();
    auto  add_result = handler->AddRef();
    EXPECT_EQ(add_result, 1u);
    auto release_result = handler->Release();
    EXPECT_EQ(release_result, 0u);
}

// ============================================================================
// Runtime dispatch: CONTROL_PLANE RemoteAddRef/RemoteRelease stay on the IO
// coroutine domain (awaitable dispatch), never enter inbound_queue_ (BT)
// ============================================================================

namespace
{
    std::vector<uint8_t> EncodeObjectIdBody(const ObjectId& object_id)
    {
        return {
            reinterpret_cast<const uint8_t*>(&object_id),
            reinterpret_cast<const uint8_t*>(&object_id) + sizeof(object_id)};
    }

    constexpr uint16_t UPSTREAM_FALLBACK_HOST_SESSION_ID = 5;
    constexpr uint16_t UPSTREAM_FALLBACK_MAIN_SESSION_ID = 1;
    constexpr uint16_t UPSTREAM_FALLBACK_UNKNOWN_TARGET_SESSION_ID = 42;
    constexpr uint32_t UPSTREAM_FALLBACK_INTERFACE_ID = 0x4242;

    // main 语义解析体（两级查表）：FindManagedHostConnection 命中取
    // host->GetTransport()，否则 FindHostLocalTransport——本文件全部
    // main 角色构造点共用（各构造点 lambda 自行捕获 run loop 引用）
    TransportLookupResult ResolveTransportMainSemantic(
        ConnectionManager& connection_manager,
        uint16_t           target_session_id)
    {
        const DasPtr<IHostConnection> host =
            connection_manager.FindManagedHostConnection(target_session_id);
        if (host)
        {
            return host->GetTransport();
        }
        return connection_manager.FindHostLocalTransport(target_session_id);
    }
} // namespace

TEST(DispatchToHandlerRoutingTest, ControlPlaneRemoteRefEventStaysOnIoDomain)
{
    constexpr uint16_t local_session_id = 1;
    constexpr uint16_t peer_session_id = 2;

    ProxyFactory                    proxy_factory;
    RemoteObjectRegistry            registry;
    IpcMessageQueue<InboundMessage> inbound_queue(8);
    // main 语义构造点：显式传两级查表 resolver（模拟 main 角色的
    // 发送解析）
    IpcRunLoop run_loop(
        false,
        &inbound_queue,
        proxy_factory,
        registry,
        [&run_loop](uint16_t target_session_id) -> TransportLookupResult
        {
            return ResolveTransportMainSemantic(
                run_loop.GetConnectionManager(),
                target_session_id);
        });
    run_loop.SetSessionId(local_session_id);

    DistributedObjectManager& object_manager = proxy_factory.GetObjectManager();
    object_manager.SetSessionId(local_session_id);

    auto remote_ref_handler =
        RemoteRefHandler::Create(object_manager, run_loop);
    run_loop.RegisterHandler(
        HeaderFlags::CONTROL_PLANE,
        static_cast<uint32_t>(IpcCommandType::RemoteAddRef),
        remote_ref_handler.Get());
    run_loop.RegisterHandler(
        HeaderFlags::CONTROL_PLANE,
        static_cast<uint32_t>(IpcCommandType::RemoteRelease),
        remote_ref_handler.Get());

    auto*    mock = new MockDasObject();
    ObjectId object_id{};
    ASSERT_EQ(
        object_manager.RegisterLocalObject(mock, object_id, false),
        DAS_S_OK);

    const auto object_body = EncodeObjectIdBody(object_id);
    const auto make_event_header = [&](IpcCommandType command_type)
    {
        return IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::Event)
            .SetControlPlaneCommand(command_type)
            .SetSourceSessionId(peer_session_id)
            .SetTargetSessionId(local_session_id)
            .SetBodySize(static_cast<uint32_t>(object_body.size()))
            .Build();
    };

    const auto route_event = [&](const ValidatedIPCMessageHeader& header)
    {
        auto future = boost::asio::co_spawn(
            run_loop.GetIoContext(),
            run_loop.RouteIncomingMessage(header, object_body),
            boost::asio::use_future);
        if (run_loop.GetIoContext().stopped())
        {
            run_loop.GetIoContext().restart();
        }
        run_loop.GetIoContext().run();
        future.get();
    };

    route_event(make_event_header(IpcCommandType::RemoteAddRef));

    // IO 域 awaitable 分发完成 holder 记账，未进 BT 队列
    EXPECT_EQ(object_manager.GetRemoteHolderCount(object_id), 1u);
    EXPECT_FALSE(inbound_queue.TryPop().has_value());

    route_event(make_event_header(IpcCommandType::RemoteRelease));

    EXPECT_EQ(object_manager.GetLocalEntryCount(), 0u);

    // RELEASE 的 EVENT 本身仍在 IO 域消化；唯一入队的是条目移除后的
    // final Release 桥接回调（内部 AsyncCallback 命令，非 130/132 EVENT）
    auto queued = inbound_queue.TryPop();
    ASSERT_TRUE(queued.has_value());
    EXPECT_EQ(
        queued->header.GetHeaderFlags(),
        DAS::Core::IPC::HeaderFlags::BUSINESS_CONTROL);
    EXPECT_EQ(
        queued->header.GetInterfaceId(),
        static_cast<uint32_t>(
            DAS::Core::IPC::InternalBusinessCommand::AsyncCallback));
    EXPECT_FALSE(inbound_queue.TryPop().has_value());
}

// ============================================================================
// 09h 汇点 B E2E：IO 控制面分发（CONTROL_PLANE + HostConnection 路由）的
// 响应经 PostSendWithTransport → DoSendCoroutine 出站——第二汇点可达性
// 与该路径的 pending 出清时序（闸3 guard 在分发协程退出时先出清，
// 随后 spawn 的响应协程过第二汇点时队列已空 = 独立帧形态；账不丢）
// ============================================================================

namespace
{
    constexpr uint16_t ROUTING_E2E_LOCAL_SESSION_ID = 1;
    constexpr uint16_t ROUTING_E2E_PEER_SESSION_ID = 2;
    constexpr uint32_t ROUTING_E2E_REQUEST_INTERFACE_ID = 0x7788;

    class RoutingE2ETestInternalHost final : public IHostConnection
    {
    public:
        RoutingE2ETestInternalHost(
            boost::asio::io_context& io_context,
            uint16_t                 session_id,
            AnyTransport&            transport)
            : io_context_(io_context), session_id_(session_id),
              transport_(transport)
        {
        }

        [[nodiscard]]
        uint32_t AddRef() override
        {
            return ++ref_count_;
        }

        [[nodiscard]]
        uint32_t Release() override
        {
            const auto result = --ref_count_;
            if (result == 0)
            {
                delete this;
            }
            return result;
        }

        DasResult QueryInterface(const DasGuid& iid, void** pp_object) override
        {
            if (!pp_object)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DasIidOf<IHostConnection>())
            {
                (void)AddRef();
                *pp_object = static_cast<IHostConnection*>(this);
                return DAS_S_OK;
            }
            *pp_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        TransportLookupResult GetTransport() override
        {
            if (!transport_.IsConnected())
            {
                return {DAS_E_IPC_CONNECTION_LOST, std::nullopt};
            }
            return {DAS_S_OK, std::optional{std::ref(transport_)}};
        }

        boost::asio::io_context& GetIoContext() override { return io_context_; }

        uint16_t GetSessionId() const override { return session_id_; }

        uint32_t GetPid() const override { return 0; }

        bool IsRunning() const override { return true; }

        void Stop() override {}
        void ClearCallbacks() override {}
        void NotifyHeartbeatTimeout() override {}
        void TerminateIfRunning() override {}

    private:
        std::atomic<uint32_t>    ref_count_{0};
        boost::asio::io_context& io_context_;
        uint16_t                 session_id_;
        AnyTransport&            transport_;
    };

    // 应答型 awaitable handler：响应经 HostConnectionRoute sender 出站
    // （IpcResponseSender.cpp 的 PostSendWithTransport 分支）。静态存储
    // （不自删——与 MockAwaitableHandler 同型）
    class RespondingAwaitableHandler final : public IAwaitableMessageHandler
    {
    public:
        [[nodiscard]]
        uint32_t AddRef() override
        {
            return ++ref_count_;
        }

        [[nodiscard]]
        uint32_t Release() override
        {
            return --ref_count_;
        }

        [[nodiscard]]
        uint32_t GetInterfaceId() const override
        {
            return ROUTING_E2E_REQUEST_INTERFACE_ID;
        }

        boost::asio::awaitable<DasResult> HandleMessage(
            const ValidatedIPCMessageHeader& header,
            const std::vector<uint8_t>&      body,
            IpcResponseSender&               sender,
            ControlPlaneContext&             ctx) override
        {
            (void)body;
            (void)ctx;
            const auto response_header =
                IPCMessageHeaderBuilder()
                    .SetMessageType(MessageType::Response)
                    .SetControlPlaneCommand(
                        HandshakeInterfaceId::HandshakeIfaceHeartbeat)
                    .SetCallId(header.GetCallId())
                    .SetSourceSessionId(header.GetTargetSessionId())
                    .SetTargetSessionId(header.GetSourceSessionId())
                    .SetBodySize(0)
                    .Build();
            const std::vector<uint8_t> empty_body;
            co_return co_await sender.SendResponseAsync(
                response_header,
                empty_body);
        }

    private:
        std::atomic<uint32_t> ref_count_{0};
    };
} // namespace

TEST(
    DispatchToHandlerRoutingTest,
    IoHostConnectionResponseExitsThroughSecondFunnel)
{
    ProxyFactory                    proxy_factory;
    RemoteObjectRegistry            registry;
    IpcMessageQueue<InboundMessage> inbound_queue(8);
    // main 语义构造点：显式传两级查表 resolver（模拟 main 角色的
    // 发送解析）
    IpcRunLoop run_loop(
        false,
        &inbound_queue,
        proxy_factory,
        registry,
        [&run_loop](uint16_t target_session_id) -> TransportLookupResult
        {
            return ResolveTransportMainSemantic(
                run_loop.GetConnectionManager(),
                target_session_id);
        });
    run_loop.SetSessionId(ROUTING_E2E_LOCAL_SESSION_ID);

    // 外部驱动模式：夹具构造先于 StartRunLoop，之后 io 由 Run 线程持续驱动
    OutboundFrameProbe probe(run_loop.GetIoContext(), true);
    ASSERT_TRUE(probe.IsHealthy());
    auto counterpart_transport = probe.TakeCounterpartTransport();
    ASSERT_TRUE(counterpart_transport.has_value());

    const DasPtr<IHostConnection> host(new RoutingE2ETestInternalHost(
        run_loop.GetIoContext(),
        ROUTING_E2E_PEER_SESSION_ID,
        *counterpart_transport));
    ASSERT_EQ(run_loop.RegisterInternalHost(host), DAS_S_OK);

    static RespondingAwaitableHandler responding_handler;
    run_loop.RegisterHandler(
        HeaderFlags::CONTROL_PLANE,
        ROUTING_E2E_REQUEST_INTERFACE_ID,
        &responding_handler);

    std::thread run_thread;
    run_thread = std::thread([&run_loop]() { run_loop.Run(); });
    for (int i = 0; i < 100 && !run_loop.IsRunning(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(run_loop.IsRunning());

    // 该 target 存在 pending ADD_REF（测试直驱入队模拟 IO 域物化）
    const ObjectId object_id{
        .session_id = ROUTING_E2E_PEER_SESSION_ID,
        .generation = 1,
        .local_id = 0x31};
    run_loop.EnqueueAddRef(object_id);
    EXPECT_EQ(run_loop.GetPendingAddRefCount(ROUTING_E2E_PEER_SESSION_ID), 1u);

    // 注入 CONTROL_PLANE 请求 → IO 分发（guard 挂点）→ handler 经
    // HostConnectionRoute 应答 → DoSendCoroutine（第二汇点）出站
    // （interface_id 为测试专用值，不经 SetControlPlaneCommand 的
    // IpcCommandType 强转——该枚举底层 uint8_t 会截断）
    const auto request_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::Request)
            .SetHeaderFlags(HeaderFlags::CONTROL_PLANE)
            .SetInterfaceId(ROUTING_E2E_REQUEST_INTERFACE_ID)
            .SetCallId(71)
            .SetSourceSessionId(ROUTING_E2E_PEER_SESSION_ID)
            .SetTargetSessionId(ROUTING_E2E_LOCAL_SESSION_ID)
            .SetBodySize(0)
            .Build();
    const std::vector<uint8_t> empty_body;
    ASSERT_EQ(
        probe.SendFrameFromObserver(request_header, empty_body),
        DAS_S_OK);

    // 收敛后语义：IO 分发域的响应协程与分发协程帧析构里的 guard 出清并发
    // 竞速，两种合法结局（响应经第二汇点携 rider 一帧带走 / 响应独立 +
    // guard 出清独立 130）都不跨周期且账完整。响应帧恒先出线
    EXPECT_EQ(probe.DrainUntil(1, std::chrono::seconds(2)), 1u);
    ASSERT_EQ(probe.TotalFrameCount(), 1u);
    const auto& response_frame = probe.Frames()[0];
    EXPECT_EQ(
        response_frame.header.GetInterfaceId(),
        static_cast<uint32_t>(HandshakeInterfaceId::HandshakeIfaceHeartbeat));

    size_t embedded_add_ref_count = 0;
    if (response_frame.header.GetFlags()
        & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE)
    {
        // 结局一：响应经第二汇点携 rider——扩展段恰一条内嵌 130
        DAS::Core::IPC::HeaderValidationResult embedded_error;
        const auto embedded_header = ValidatedIPCMessageHeader::Deserialize(
            response_frame.body.data(),
            sizeof(DAS::Core::IPC::IPCMessageHeader),
            &embedded_error);
        ASSERT_TRUE(embedded_header.has_value());
        EXPECT_EQ(
            embedded_header->GetInterfaceId(),
            static_cast<uint32_t>(IpcCommandType::RemoteAddRef));
        embedded_add_ref_count = 1;
    }
    else
    {
        // 结局二：guard 先出清——独立 130 紧随
        EXPECT_EQ(probe.DrainUntil(1, std::chrono::seconds(2)), 1u);
    }

    // 守恒：恰一次 ADD_REF 申报到达（内嵌 + 独立 == 1）
    EXPECT_EQ(
        embedded_add_ref_count
            + probe.CountByInterfaceId(
                static_cast<uint32_t>(IpcCommandType::RemoteAddRef)),
        1u);
    EXPECT_EQ(run_loop.GetPendingAddRefCount(ROUTING_E2E_PEER_SESSION_ID), 0u);
    EXPECT_TRUE(run_loop.PendingAddRefsEmpty());

    run_loop.RequestStop();
    if (run_thread.joinable())
    {
        run_thread.join();
    }
    probe.Shutdown();
}

// 09h 第二汇点的路由级 RED 锚：经真实 RouteIncomingMessage 转发分支进入
// PostSendWithTransport 的帧（转发帧，A7 不合格载体）——第二汇点漏挂时
// 该 target 的 pending 永远搭不上车也不出清（本用例红）
TEST(
    DispatchToHandlerRoutingTest,
    ForwardedFrameFlushesPendingThroughSecondFunnel)
{
    ProxyFactory                    proxy_factory;
    RemoteObjectRegistry            registry;
    IpcMessageQueue<InboundMessage> inbound_queue(8);
    // main 语义构造点：显式传两级查表 resolver（模拟 main 角色的
    // 发送解析）
    IpcRunLoop run_loop(
        false,
        &inbound_queue,
        proxy_factory,
        registry,
        [&run_loop](uint16_t target_session_id) -> TransportLookupResult
        {
            return ResolveTransportMainSemantic(
                run_loop.GetConnectionManager(),
                target_session_id);
        });
    run_loop.SetSessionId(ROUTING_E2E_LOCAL_SESSION_ID);

    OutboundFrameProbe probe(run_loop.GetIoContext(), true);
    ASSERT_TRUE(probe.IsHealthy());
    auto counterpart_transport = probe.TakeCounterpartTransport();
    ASSERT_TRUE(counterpart_transport.has_value());

    const DasPtr<IHostConnection> host(new RoutingE2ETestInternalHost(
        run_loop.GetIoContext(),
        ROUTING_E2E_PEER_SESSION_ID,
        *counterpart_transport));
    ASSERT_EQ(run_loop.RegisterInternalHost(host), DAS_S_OK);

    std::thread run_thread;
    run_thread = std::thread([&run_loop]() { run_loop.Run(); });
    for (int i = 0; i < 100 && !run_loop.IsRunning(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(run_loop.IsRunning());

    // 转发帧的 target = 已注册托管连接的对端 session：pending 按同 target 键控
    const ObjectId object_id{
        .session_id = ROUTING_E2E_PEER_SESSION_ID,
        .generation = 2,
        .local_id = 0x32};
    run_loop.EnqueueAddRef(object_id);
    EXPECT_EQ(run_loop.GetPendingAddRefCount(ROUTING_E2E_PEER_SESSION_ID), 1u);

    // 注入非控制平面帧：target=对端（非本地）→ 转发分支 →
    // PostSendWithTransport → DoSendCoroutine（第二汇点）
    const auto forwarded_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::Request)
            .SetHeaderFlags(HeaderFlags::NONE)
            .SetInterfaceId(0x1237)
            .SetCallId(72)
            .SetSourceSessionId(3)
            .SetTargetSessionId(ROUTING_E2E_PEER_SESSION_ID)
            .SetBodySize(0)
            .Build();
    const std::vector<uint8_t> empty_body;
    ASSERT_EQ(
        probe.SendFrameFromObserver(forwarded_header, empty_body),
        DAS_S_OK);

    // 第二汇点过闸2：pending 先独立发（FIFO）随后转发帧原样独立发出
    EXPECT_EQ(probe.DrainUntil(2, std::chrono::seconds(2)), 2u);
    EXPECT_EQ(probe.TotalFrameCount(), 2u);
    EXPECT_EQ(
        probe.InterfaceIdOrder(),
        (std::vector<uint32_t>{
            static_cast<uint32_t>(IpcCommandType::RemoteAddRef),
            0x1237}));
    const auto& forwarded_wire_frame = probe.Frames()[1];
    EXPECT_EQ(
        forwarded_wire_frame.header.GetFlags()
            & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
        0);
    EXPECT_EQ(run_loop.GetPendingAddRefCount(ROUTING_E2E_PEER_SESSION_ID), 0u);
    EXPECT_TRUE(run_loop.PendingAddRefsEmpty());

    run_loop.RequestStop();
    if (run_thread.joinable())
    {
        run_thread.join();
    }
    probe.Shutdown();
}

// ============================================================================
// 发送侧解析注入（TransportResolver 必填构造）单元锚点：host 恒上行语义
// 对未知 target 的 PostSend 帧经上行管道出线且 header 逐字段不改写；
// main 两级查表语义保持既有失败路径；空 callable 构造期 fail-fast
// ============================================================================

TEST(
    DispatchToHandlerRoutingTest,
    UpstreamFallbackSendsUnknownTargetThroughUpstreamUnmodified)
{
    ProxyFactory                    proxy_factory;
    RemoteObjectRegistry            registry;
    IpcMessageQueue<InboundMessage> inbound_queue(8);
    // host 语义构造点：显式传恒上行 resolver——忽略 target，调用期现读
    // host-local 表中 main session 的上行管道（不快照 transport 引用）
    IpcRunLoop run_loop(
        false,
        &inbound_queue,
        proxy_factory,
        registry,
        [&run_loop](uint16_t /*target_session_id*/) -> TransportLookupResult
        {
            constexpr uint16_t main_process_session_id = 1;
            return run_loop.GetConnectionManager().FindHostLocalTransport(
                main_process_session_id);
        });
    run_loop.SetSessionId(UPSTREAM_FALLBACK_HOST_SESSION_ID);

    // 探针对端注册为 host-local session 1 充当上行管道
    OutboundFrameProbe probe(run_loop.GetIoContext(), true);
    ASSERT_TRUE(probe.IsHealthy());
    auto counterpart_transport = probe.TakeCounterpartTransport();
    ASSERT_TRUE(counterpart_transport.has_value());
    ASSERT_EQ(
        run_loop.GetConnectionManager().RegisterHostLocalTransport(
            UPSTREAM_FALLBACK_MAIN_SESSION_ID,
            std::move(*counterpart_transport)),
        DAS_S_OK);

    std::thread run_thread([&run_loop]() { run_loop.Run(); });
    for (int i = 0; i < 100 && !run_loop.IsRunning(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(run_loop.IsRunning());

    // 未知 target session 的 PostSend：host 语义下不再本地即败——帧经
    // 上行出线（裁决权在接收侧）
    const auto outbound_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::Request)
            .SetHeaderFlags(HeaderFlags::NONE)
            .SetInterfaceId(UPSTREAM_FALLBACK_INTERFACE_ID)
            .SetCallId(99)
            .SetSourceSessionId(UPSTREAM_FALLBACK_HOST_SESSION_ID)
            .SetTargetSessionId(UPSTREAM_FALLBACK_UNKNOWN_TARGET_SESSION_ID)
            .SetBodySize(0)
            .Build();
    ASSERT_EQ(
        run_loop.PostSend(outbound_header, std::vector<uint8_t>{}),
        DAS_S_OK);

    ASSERT_EQ(probe.DrainUntil(1, std::chrono::seconds(2)), 1u);
    ASSERT_EQ(probe.TotalFrameCount(), 1u);
    const auto& wire_frame = probe.Frames()[0];
    // header 逐字段不改写：source/target/call_id/interface_id/flags 原样
    EXPECT_EQ(
        wire_frame.header.GetSourceSessionId(),
        UPSTREAM_FALLBACK_HOST_SESSION_ID);
    EXPECT_EQ(
        wire_frame.header.GetTargetSessionId(),
        UPSTREAM_FALLBACK_UNKNOWN_TARGET_SESSION_ID);
    EXPECT_EQ(wire_frame.header.GetCallId(), 99);
    EXPECT_EQ(
        wire_frame.header.GetInterfaceId(),
        UPSTREAM_FALLBACK_INTERFACE_ID);
    EXPECT_EQ(wire_frame.header.GetHeaderFlags(), HeaderFlags::NONE);
    EXPECT_EQ(
        wire_frame.header.GetFlags()
            & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
        0);

    run_loop.RequestStop();
    if (run_thread.joinable())
    {
        run_thread.join();
    }
    probe.Shutdown();
}

TEST(
    DispatchToHandlerRoutingTest,
    TwoLevelLookupResolverKeepsExistingFailureSemantics)
{
    ProxyFactory                    proxy_factory;
    RemoteObjectRegistry            registry;
    IpcMessageQueue<InboundMessage> inbound_queue(8);
    // main 语义构造点：显式传两级查表 resolver（解析体单源在
    // ResolveTransportMainSemantic）
    IpcRunLoop run_loop(
        false,
        &inbound_queue,
        proxy_factory,
        registry,
        [&run_loop](uint16_t target_session_id) -> TransportLookupResult
        {
            return ResolveTransportMainSemantic(
                run_loop.GetConnectionManager(),
                target_session_id);
        });
    run_loop.SetSessionId(UPSTREAM_FALLBACK_MAIN_SESSION_ID);

    // 探针注册为 session 1 的 host-local 位——失败路径零出帧的反证
    // 观察点
    OutboundFrameProbe probe(run_loop.GetIoContext(), true);
    ASSERT_TRUE(probe.IsHealthy());
    auto counterpart_transport = probe.TakeCounterpartTransport();
    ASSERT_TRUE(counterpart_transport.has_value());
    ASSERT_EQ(
        run_loop.GetConnectionManager().RegisterHostLocalTransport(
            UPSTREAM_FALLBACK_MAIN_SESSION_ID,
            std::move(*counterpart_transport)),
        DAS_S_OK);

    std::thread run_thread([&run_loop]() { run_loop.Run(); });
    for (int i = 0; i < 100 && !run_loop.IsRunning(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(run_loop.IsRunning());

    // 未知 target 的带回调 PostSend：既有失败语义保持——无任何出帧，
    // 完成回调收到 DAS_E_IPC_NO_CONNECTIONS
    std::promise<DasResult> completion_promise;
    auto                    completion_future = completion_promise.get_future();
    const auto              request_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::Request)
            .SetHeaderFlags(HeaderFlags::NONE)
            .SetInterfaceId(UPSTREAM_FALLBACK_INTERFACE_ID)
            .SetCallId(100)
            .SetSourceSessionId(UPSTREAM_FALLBACK_MAIN_SESSION_ID)
            .SetTargetSessionId(UPSTREAM_FALLBACK_UNKNOWN_TARGET_SESSION_ID)
            .SetBodySize(0)
            .Build();
    ASSERT_EQ(
        run_loop.PostSend(
            request_header,
            std::vector<uint8_t>{},
            [&completion_promise](
                DasResult            result,
                std::vector<uint8_t> response,
                uint16_t             response_flags)
            {
                (void)response;
                (void)response_flags;
                completion_promise.set_value(result);
            }),
        DAS_S_OK);

    ASSERT_EQ(completion_future.get(), DAS_E_IPC_NO_CONNECTIONS);
    EXPECT_EQ(probe.DrainFor(std::chrono::milliseconds(200)), 0u);
    EXPECT_EQ(probe.TotalFrameCount(), 0u);

    run_loop.RequestStop();
    if (run_thread.joinable())
    {
        run_thread.join();
    }
    probe.Shutdown();
}

TEST(DispatchToHandlerRoutingTest, EmptyResolverFailsFastAtConstruction)
{
    ProxyFactory         proxy_factory;
    RemoteObjectRegistry registry;
    // 空 callable（默认构造的 TransportResolver）——构造期 fail-fast，
    // 不产生可用对象
    EXPECT_THROW(
        {
            IpcRunLoop run_loop(
                false,
                nullptr,
                proxy_factory,
                registry,
                TransportResolver{});
        },
        std::invalid_argument);
}

// ============================================================================
// 接收分类地址化单元锚点：转发判定按收件地址（单一谓词供扩展段剥离
// 判定 / CONTROL_PLANE 本地消费分支守卫 / 转发分支三处共用，无第二份
// 分歧判断）
// ============================================================================

namespace
{
    // 物化方 session（组合帧的 source）：既非本地 main 也非 owner 对端
    constexpr uint16_t ADDRESS_RULE_MATERIALIZER_SESSION_ID = 3;

    // 内嵌 130 rider 完整帧（32B header + ObjectId 8B body，自描述）
    std::vector<uint8_t> BuildEmbeddedAddRefFrame(
        const ObjectId& object_id,
        uint16_t        target_session_id)
    {
        const auto header =
            IPCMessageHeaderBuilder()
                .SetMessageType(MessageType::Event)
                .SetControlPlaneCommand(IpcCommandType::RemoteAddRef)
                .SetSourceSessionId(ADDRESS_RULE_MATERIALIZER_SESSION_ID)
                .SetTargetSessionId(target_session_id)
                .SetBodySize(static_cast<uint32_t>(sizeof(object_id)))
                .Build();
        const auto* header_bytes = reinterpret_cast<const uint8_t*>(
            static_cast<const IPCMessageHeader*>(header));
        std::vector<uint8_t> frame(
            header_bytes,
            header_bytes + sizeof(IPCMessageHeader));
        const auto object_body = EncodeObjectIdBody(object_id);
        frame.insert(frame.end(), object_body.begin(), object_body.end());
        return frame;
    }

    // bit1 组合宿主帧：CONTROL_PLANE Event 130 携带内嵌扩展段（闸1 搭车
    // 产物的真实线上形态——130 宿主不在载体排除列表）
    ValidatedIPCMessageHeader MakePiggybackedControlPlaneHostHeader(
        uint16_t target_session_id,
        uint32_t section_size,
        uint32_t total_body_size)
    {
        const auto base =
            IPCMessageHeaderBuilder()
                .SetMessageType(MessageType::Event)
                .SetControlPlaneCommand(IpcCommandType::RemoteAddRef)
                .SetSourceSessionId(ADDRESS_RULE_MATERIALIZER_SESSION_ID)
                .SetTargetSessionId(target_session_id)
                .SetBodySize(total_body_size)
                .Build();
        IPCMessageHeader raw = base.Raw();
        raw.flags = raw.flags | DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE;
        raw.reserved = section_size;
        // magic/version 未动，Deserialize 必然成功
        return ValidatedIPCMessageHeader::Deserialize(&raw, sizeof(raw))
            .value();
    }
} // namespace

TEST(
    DispatchToHandlerRoutingTest,
    PiggybackedControlPlaneFrameStaysTransparentInTransit)
{
    ProxyFactory                    proxy_factory;
    RemoteObjectRegistry            registry;
    IpcMessageQueue<InboundMessage> inbound_queue(8);
    // main 语义构造点：显式传两级查表 resolver（模拟 main 角色的
    // 发送解析）
    IpcRunLoop run_loop(
        false,
        &inbound_queue,
        proxy_factory,
        registry,
        [&run_loop](uint16_t target_session_id) -> TransportLookupResult
        {
            return ResolveTransportMainSemantic(
                run_loop.GetConnectionManager(),
                target_session_id);
        });
    run_loop.SetSessionId(ROUTING_E2E_LOCAL_SESSION_ID);

    // owner（收件地址）为已注册托管连接：地址规则下在途帧可达转发分支
    OutboundFrameProbe probe(run_loop.GetIoContext(), true);
    ASSERT_TRUE(probe.IsHealthy());
    auto counterpart_transport = probe.TakeCounterpartTransport();
    ASSERT_TRUE(counterpart_transport.has_value());

    const DasPtr<IHostConnection> host(new RoutingE2ETestInternalHost(
        run_loop.GetIoContext(),
        ROUTING_E2E_PEER_SESSION_ID,
        *counterpart_transport));
    ASSERT_EQ(run_loop.RegisterInternalHost(host), DAS_S_OK);

    // rider 与宿主各自申报的对象（本地 DOM 条目 = 递归分发的可观测锚点）
    DistributedObjectManager& object_manager = proxy_factory.GetObjectManager();
    object_manager.SetSessionId(ROUTING_E2E_LOCAL_SESSION_ID);
    const DasPtr<RemoteRefHandler> remote_ref_handler =
        RemoteRefHandler::Create(object_manager, run_loop);
    run_loop.RegisterHandler(
        HeaderFlags::CONTROL_PLANE,
        static_cast<uint32_t>(IpcCommandType::RemoteAddRef),
        remote_ref_handler.Get());

    auto*    mock_rider_object = new MockDasObject();
    auto*    mock_host_object = new MockDasObject();
    ObjectId rider_object_id{};
    ObjectId host_object_id{};
    ASSERT_EQ(
        object_manager
            .RegisterLocalObject(mock_rider_object, rider_object_id, false),
        DAS_S_OK);
    ASSERT_EQ(
        object_manager
            .RegisterLocalObject(mock_host_object, host_object_id, false),
        DAS_S_OK);

    std::thread run_thread([&run_loop]() { run_loop.Run(); });
    for (int i = 0; i < 100 && !run_loop.IsRunning(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(run_loop.IsRunning());

    // 组合帧体：[内嵌 130 rider（target=local）][宿主正常 body（ObjectId）]
    const std::vector<uint8_t> embedded_rider =
        BuildEmbeddedAddRefFrame(rider_object_id, ROUTING_E2E_LOCAL_SESSION_ID);
    const std::vector<uint8_t> host_tail = EncodeObjectIdBody(host_object_id);
    std::vector<uint8_t>       transit_body = embedded_rider;
    transit_body.insert(transit_body.end(), host_tail.begin(), host_tail.end());
    const auto transit_header = MakePiggybackedControlPlaneHostHeader(
        ROUTING_E2E_PEER_SESSION_ID,
        static_cast<uint32_t>(embedded_rider.size()),
        static_cast<uint32_t>(transit_body.size()));

    // 在途半（宿主 target=owner ≠ local）：整帧不剥离——内嵌 rider 不在
    // 本地递归分发（剥离判定与转发分支共用同一地址谓词）
    ASSERT_EQ(
        probe.SendFrameFromObserver(transit_header, transit_body),
        DAS_S_OK);
    static_cast<void>(probe.DrainUntil(1, std::chrono::milliseconds(500)));
    EXPECT_EQ(object_manager.GetRemoteHolderCount(rider_object_id), 0u);
    EXPECT_EQ(object_manager.GetRemoteHolderCount(host_object_id), 0u);

    // 对照半（宿主 target=local）：本地剥离 + 内嵌帧递归分发（终点递归
    // 剥离语义不回归）
    std::vector<uint8_t> local_body = embedded_rider;
    local_body.insert(local_body.end(), host_tail.begin(), host_tail.end());
    const auto local_header = MakePiggybackedControlPlaneHostHeader(
        ROUTING_E2E_LOCAL_SESSION_ID,
        static_cast<uint32_t>(embedded_rider.size()),
        static_cast<uint32_t>(local_body.size()));
    ASSERT_EQ(probe.SendFrameFromObserver(local_header, local_body), DAS_S_OK);
    static_cast<void>(probe.DrainFor(std::chrono::milliseconds(300)));
    EXPECT_EQ(object_manager.GetRemoteHolderCount(rider_object_id), 1u);
    EXPECT_EQ(object_manager.GetRemoteHolderCount(host_object_id), 1u);

    run_loop.RequestStop();
    if (run_thread.joinable())
    {
        run_thread.join();
    }
    probe.Shutdown();
}
