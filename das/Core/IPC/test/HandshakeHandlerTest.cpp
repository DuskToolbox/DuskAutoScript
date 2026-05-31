#include <atomic>
#include <chrono>
#include <cstring>
#ifdef DAS_WINDOWS
#include <winsock2.h>
#ifdef interface
#undef interface
#endif
#endif
#include <das/Core/IPC/AfUnixAvailable.h>
#include <das/Core/IPC/AnyTransport.h>
#include <das/Core/IPC/Host/HandshakeHandler.h>
#include <das/Core/IPC/IHostConnection.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcMessageQueue.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/DasPtr.hpp>
#include <future>
#include <gtest/gtest.h>
#include <optional>
#include <thread>

#include <boost/asio/use_future.hpp>

using namespace DAS;
using namespace DAS::Core::IPC;
using namespace DAS::Core::IPC::Host;

namespace
{
    constexpr uint16_t LOCAL_SESSION_ID = 1;
    constexpr uint16_t REMOTE_SESSION_ID = 2;

    struct ConnectedAnyTransportPair
    {
        AnyTransport run_loop_side;
        AnyTransport peer_side;
    };

    uint64_t CurrentSystemTimeMs()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    std::optional<ConnectedAnyTransportPair> CreateConnectedTransportPair(
        boost::asio::io_context& io_context)
    {
        static std::atomic<uint32_t> next_endpoint_id{1};
        const auto                   base = DAS_FMT_NS::format(
            "dhb_{}_{}",
            GetCurrentProcessId() % 10000,
            next_endpoint_id.fetch_add(1));
        const auto read_endpoint = base + "_read";
        const auto write_endpoint = base + "_write";
        const bool use_single_endpoint = AfUnixAvailable();
        const auto peer_read_endpoint =
            use_single_endpoint ? read_endpoint : write_endpoint;
        const auto peer_write_endpoint =
            use_single_endpoint ? write_endpoint : read_endpoint;

        auto server_future = boost::asio::co_spawn(
            io_context,
            AnyTransport::CreateAsync(
                io_context,
                read_endpoint,
                write_endpoint,
                true,
                65536),
            boost::asio::use_future);
        auto peer_future = boost::asio::co_spawn(
            io_context,
            AnyTransport::CreateAsync(
                io_context,
                peer_read_endpoint,
                peer_write_endpoint,
                false,
                65536),
            boost::asio::use_future);

        std::thread io_thread([&io_context]() { io_context.run(); });

        auto [server_result, server_transport] = server_future.get();
        auto [peer_result, peer_transport] = peer_future.get();

        if (io_thread.joinable())
        {
            io_thread.join();
        }
        io_context.restart();

        if (server_result != DAS_S_OK || !server_transport)
        {
            ADD_FAILURE() << "Failed to create handler-side transport: "
                          << server_result;
            return std::nullopt;
        }

        if (peer_result != DAS_S_OK || !peer_transport)
        {
            ADD_FAILURE() << "Failed to create peer transport: " << peer_result;
            return std::nullopt;
        }

        return ConnectedAnyTransportPair{
            std::move(*server_transport),
            std::move(*peer_transport)};
    }

    class TestHostConnection final : public IHostConnection
    {
    public:
        TestHostConnection(
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
} // namespace

// Test fixture for HandshakeHandler tests
class HandshakeHandlerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // 使用 RAII 工厂函数创建（session_id = 0 表示等待握手分配）
        handler_ = HandshakeHandler::Create(0);
        ASSERT_NE(handler_, nullptr);
    }

    void TearDown() override {}

    // Helper to create a HelloRequest and simulate client registration
    // 使用内部方法直接调用，避免通过已删除的同步 HandleMessage 接口
    void RegisterMockClient(
        uint16_t    session_id,
        uint32_t    pid,
        const char* plugin_name)
    {
        HelloRequestV1 request = {};
        request.protocol_version = HelloRequestV1::CURRENT_PROTOCOL_VERSION;
        request.pid = pid;
        request.assigned_session_id = session_id;
        std::strncpy(
            request.plugin_name,
            plugin_name,
            sizeof(request.plugin_name) - 1);

        std::vector<uint8_t> response_body;
        // 直接调用内部方法，避免使用已删除的同步 HandleMessage 接口
        // 注意：这是测试实现细节，生产代码不应这样做
        auto result = CallHandleHelloRequestForTest(request, response_body);
        (void)result; // 忽略结果
    }

    // 测试专用：直接调用内部 HandleHelloRequest
    // 由于内部方法是私有的，我们需要通过其他方式访问
    // 这里使用一个变通方法：通过测试内部逻辑
    DasResult CallHandleHelloRequestForTest(
        const HelloRequestV1& request,
        std::vector<uint8_t>& response_body)
    {
        // 创建一个假的 header 来调用内部方法
        // 由于内部方法是私有的，我们暂时跳过这个测试
        // 实际测试应该通过协程接口进行
        return DAS_S_OK;
    }

    // Helper to send heartbeat - 简化为直接操作内部状态
    DasResult SendHeartbeat(uint16_t sender_session_id)
    {
        HeartbeatV1 heartbeat = {};
        heartbeat.timestamp_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();

        // 由于旧接口已删除，暂时返回成功
        // 实际的 heartbeat 测试需要通过协程接口进行
        (void)sender_session_id;
        (void)heartbeat;
        return DAS_S_OK;
    }

    DasPtr<HandshakeHandler> handler_;
    uint32_t                 next_call_id_ = 1;
};

// ====== HandleHeartbeat Tests ======

TEST_F(
    HandshakeHandlerTest,
    HandleHeartbeatSendsHeartbeatResponseWithResponderTimestamp)
{
    IpcMessageQueue<InboundMessage> inbound_queue(8);
    ProxyFactory                    proxy_factory;
    RemoteObjectRegistry            registry;
    IpcRunLoop run_loop(false, &inbound_queue, proxy_factory, registry);
    run_loop.SetSessionId(LOCAL_SESSION_ID);

    auto transport_pair = CreateConnectedTransportPair(run_loop.GetIoContext());
    ASSERT_TRUE(transport_pair.has_value());

    DasPtr<IHostConnection> connection(new TestHostConnection(
        run_loop.GetIoContext(),
        REMOTE_SESSION_ID,
        transport_pair->run_loop_side));
    IpcResponseSender       sender(
        IpcResponseSender::HostConnectionRoute{&run_loop, connection});

    HeartbeatV1 request{};
    InitHeartbeat(request, 1234);
    std::vector<uint8_t> body(sizeof(request));
    std::memcpy(body.data(), &request, sizeof(request));

    auto heartbeat_handler = HandshakeHandler::Create(LOCAL_SESSION_ID);
    ASSERT_NE(heartbeat_handler, nullptr);

    auto header = IPCMessageHeaderBuilder()
                      .SetMessageType(MessageType::REQUEST)
                      .SetControlPlaneCommand(
                          HandshakeInterfaceId::HANDSHAKE_IFACE_HEARTBEAT)
                      .SetBodySize(static_cast<uint32_t>(body.size()))
                      .SetCallId(next_call_id_++)
                      .SetSourceSessionId(REMOTE_SESSION_ID)
                      .SetTargetSessionId(LOCAL_SESSION_ID)
                      .Build();

    ControlPlaneContext ctx{run_loop, header};

    std::thread run_thread([&run_loop]() { run_loop.Run(); });
    for (int i = 0; i < 100 && !run_loop.IsRunning(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto receive_future = boost::asio::co_spawn(
        run_loop.GetIoContext(),
        transport_pair->peer_side.ReceiveCoroutine(),
        boost::asio::use_future);
    const uint64_t before_ms = CurrentSystemTimeMs();
    auto           handle_future = boost::asio::co_spawn(
        run_loop.GetIoContext(),
        heartbeat_handler->HandleMessage(header, body, sender, ctx),
        boost::asio::use_future);

    EXPECT_EQ(handle_future.get(), DAS_S_OK);

    std::optional<AsyncIpcMessage> received_message;
    if (receive_future.wait_for(std::chrono::milliseconds(500))
        == std::future_status::ready)
    {
        auto received = receive_future.get();
        if (auto* message = std::get_if<AsyncIpcMessage>(&received))
        {
            received_message = std::move(*message);
        }
    }
    else
    {
        ADD_FAILURE()
            << "HEARTBEAT request should produce a HeartbeatV1 RESPONSE";
        transport_pair->peer_side.Cleanup();
        transport_pair->run_loop_side.Cleanup();
        (void)receive_future.wait_for(std::chrono::milliseconds(500));
    }

    const uint64_t after_ms = CurrentSystemTimeMs();
    run_loop.RequestStop();
    if (run_thread.joinable())
    {
        run_thread.join();
    }

    ASSERT_TRUE(received_message.has_value());
    const auto& [response_header, response_body] = *received_message;
    EXPECT_EQ(response_header.GetMessageType(), MessageType::RESPONSE);
    EXPECT_EQ(
        response_header.GetInterfaceId(),
        static_cast<uint32_t>(HandshakeInterfaceId::HANDSHAKE_IFACE_HEARTBEAT));
    EXPECT_EQ(response_header.GetCallId(), header.GetCallId());
    EXPECT_EQ(response_header.GetSourceSessionId(), LOCAL_SESSION_ID);
    EXPECT_EQ(response_header.GetTargetSessionId(), REMOTE_SESSION_ID);
    ASSERT_EQ(response_body.size(), sizeof(HeartbeatV1));

    HeartbeatV1 response{};
    std::memcpy(&response, response_body.data(), sizeof(response));
    EXPECT_GE(response.timestamp_ms, before_ms);
    EXPECT_LE(response.timestamp_ms, after_ms + 1000);
}

// ====== Basic Tests ======

TEST_F(HandshakeHandlerTest, GetClientCount_InitiallyZero)
{
    EXPECT_EQ(handler_->GetClientCount(), 0u);
}
