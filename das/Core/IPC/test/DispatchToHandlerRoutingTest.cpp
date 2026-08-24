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
} // namespace

TEST(DispatchToHandlerRoutingTest, ControlPlaneRemoteRefEventStaysOnIoDomain)
{
    constexpr uint16_t local_session_id = 1;
    constexpr uint16_t peer_session_id = 2;

    ProxyFactory                    proxy_factory;
    RemoteObjectRegistry            registry;
    IpcMessageQueue<InboundMessage> inbound_queue(8);
    IpcRunLoop run_loop(false, &inbound_queue, proxy_factory, registry);
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
    IpcRunLoop run_loop(false, &inbound_queue, proxy_factory, registry);
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

    // 帧序：spawn 的响应协程先于 guard 出清的独立 130（co_spawn 队序）——
    // 响应出第二汇点时队列已被 guard 取走 → 独立帧形态、账不丢。
    // （时序锚点用例：IO 分发域的响应恒被闸3 guard 先行出清——搭车机会
    // 属于 spawn 序更早的响应协程不可达的路径；第二汇点的搭车分支由
    // ForwardedFrameFlushesPendingThroughSecondFunnel 与
    // IpcRunLoopTest 的 PostSendWithTransport 直驱用例钉死）
    EXPECT_EQ(probe.DrainUntil(2, std::chrono::seconds(2)), 2u);
    EXPECT_EQ(probe.TotalFrameCount(), 2u);
    EXPECT_EQ(
        probe.InterfaceIdOrder(),
        (std::vector<uint32_t>{
            static_cast<uint32_t>(
                HandshakeInterfaceId::HandshakeIfaceHeartbeat),
            static_cast<uint32_t>(IpcCommandType::RemoteAddRef)}));
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
    IpcRunLoop run_loop(false, &inbound_queue, proxy_factory, registry);
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
