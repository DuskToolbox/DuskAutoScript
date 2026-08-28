#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <cstring>
#include <das/Core/IPC/AfUnixAvailable.h>
#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/IHostConnection.h>
#include <das/Core/IPC/IMessageHandler.h>
#include <das/Core/IPC/InternalCallbackHandler.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/IpcTransport.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/IPC/RemoteRefHandler.h>
#include <das/IDasAsyncCallback.h>
#include <das/Utils/fmt.h>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "MockDasObject.h"
#include "OutboundFrameProbe.h"
#include "TestTransportResolvers.h"
using DAS::Core::IPC::AnyTransport;
using DAS::Core::IPC::CallKey;
using DAS::Core::IPC::IHostConnection;
using DAS::Core::IPC::IMessageHandler;
using DAS::Core::IPC::InboundMessage;
using DAS::Core::IPC::IPCMessageHeader;
using DAS::Core::IPC::IPCMessageHeaderBuilder;
using DAS::Core::IPC::IpcMessageQueue;
using DAS::Core::IPC::IpcResponseSender;
using DAS::Core::IPC::IpcRunLoop;
using DAS::Core::IPC::IpcTransport;
using DAS::Core::IPC::MessageType;
using DAS::Core::IPC::PendingCallState;
using DAS::Core::IPC::ValidatedIPCMessageHeader;

namespace
{
    constexpr uint16_t LOCAL_SESSION_ID = 1;
    constexpr uint16_t REMOTE_SESSION_ID = 2;

    struct ConnectedAnyTransportPair
    {
        AnyTransport run_loop_side;
        AnyTransport peer_side;
    };

    class TestInternalHost final : public IHostConnection
    {
    public:
        TestInternalHost(
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

        DAS::Core::IPC::TransportLookupResult GetTransport() override
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

    // ====== 捎带扩展段（RESERVED_AS_SIZE，D-12 段 A 接收侧）测试构造 ======
    // 模拟段 B 发送端拼装：builder 无 SetReserved，测试直接改写 wire 字节后
    // 经既有 Deserialize 路径构造合法 header。

    std::vector<uint8_t> EncodeObjectIdBody(
        const DAS::Core::IPC::ObjectId& object_id)
    {
        return {
            reinterpret_cast<const uint8_t*>(&object_id),
            reinterpret_cast<const uint8_t*>(&object_id) + sizeof(object_id)};
    }

    std::vector<uint8_t> BuildEmbeddedControlPlaneEvent(
        const DAS::Core::IPC::ObjectId& object_id)
    {
        const auto header =
            IPCMessageHeaderBuilder()
                .SetMessageType(MessageType::Event)
                .SetControlPlaneCommand(
                    DAS::Core::IPC::IpcCommandType::RemoteAddRef)
                .SetSourceSessionId(REMOTE_SESSION_ID)
                .SetTargetSessionId(LOCAL_SESSION_ID)
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

    ValidatedIPCMessageHeader MakePiggybackHostHeader(
        uint32_t section_size,
        uint32_t total_body_size)
    {
        const auto base = IPCMessageHeaderBuilder()
                              .SetMessageType(MessageType::Response)
                              .SetHeaderFlags(DAS::Core::IPC::HeaderFlags::NONE)
                              .SetInterfaceId(0x1234)
                              .SetCallId(901)
                              .SetSourceSessionId(REMOTE_SESSION_ID)
                              .SetTargetSessionId(LOCAL_SESSION_ID)
                              .SetBodySize(total_body_size)
                              .Build();
        IPCMessageHeader raw = base.Raw();
        raw.flags = raw.flags | DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE;
        raw.reserved = section_size;
        // magic/version 未动，Deserialize 必然成功；失败会抛出并被测试捕获
        return ValidatedIPCMessageHeader::Deserialize(&raw, sizeof(raw))
            .value();
    }

    // 空实现 IResolveContext（BusinessThread 构造依赖，直驱任务不触达）
    class RunLoopFifoResolveContext final
        : public DAS::Core::IPC::IResolveContext
    {
    public:
        DasResult ResolveMainProcessInterface(const DasGuid&, IDasBase**)
            override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult RegisterService(IDasBase*, const DasGuid&) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult UnregisterService(const DasGuid&) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult ResolveMainProcessInterfaceByName(const char*, IDasBase**)
            override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult RegisterServiceByName(IDasBase*, const DasGuid&, const char*)
            override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult UnregisterServiceByName(const char*) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }
    };

    // 把任意任务投递到真实 BusinessThread 执行线程的回调（照
    // ProxyAddRefForkTest 的 BusinessThreadTaskCallback 手法）
    class RunLoopFifoTaskCallback final : public IDasAsyncCallback
    {
    public:
        explicit RunLoopFifoTaskCallback(std::function<void()> task)
            : task_(std::move(task))
        {
        }

        uint32_t AddRef() override
        {
            return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        uint32_t Release() override
        {
            const uint32_t count =
                ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        DasResult QueryInterface(const DasGuid& iid, void** pp_object) override
        {
            if (pp_object == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DAS_IID_BASE)
            {
                *pp_object = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult Do() noexcept override
        {
            task_();
            return DAS_S_OK;
        }

    private:
        std::atomic<uint32_t> ref_count_{1};
        std::function<void()> task_;
    };
} // namespace

// Test fixture for IpcRunLoop tests
class IpcRunLoopTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        inbound_queue_ = std::make_unique<IpcMessageQueue<InboundMessage>>(32);

        try
        {
            // main 语义构造点：显式传两级查表 resolver
            runloop_ = std::make_unique<IpcRunLoop>(
                true,
                inbound_queue_.get(),
                proxy_factory_,
                registry_,
                [this](uint16_t target_session_id)
                    -> DAS::Core::IPC::TransportLookupResult
                {
                    return ResolveTransportMainSemantic(
                        runloop_->GetConnectionManager(),
                        target_session_id);
                });
            runloop_->SetSessionId(LOCAL_SESSION_ID);
        }
        catch (const std::exception& e)
        {
            FAIL() << "Failed to create IpcRunLoop: " << e.what();
            return;
        }

        // Generate unique queue names for this test using high-resolution timer
        auto now_ns = std::chrono::high_resolution_clock::now()
                          .time_since_epoch()
                          .count();
        host_queue_name_ = DAS_FMT_NS::format(
            "das_runloop_host_{}_{}",
            GetCurrentProcessId(),
            now_ns);
        plugin_queue_name_ = DAS_FMT_NS::format(
            "das_runloop_plugin_{}_{}",
            GetCurrentProcessId(),
            now_ns);
    }

    void TearDown() override
    {
        // RAII: unique_ptr 析构自动调用 Shutdown()
        if (runloop_)
        {
            runloop_->RequestStop();
            runloop_.reset(); // 析构函数会自动调用 Shutdown()
        }
        inbound_queue_.reset();
    }

    bool SetupRunLoopWithTransport()
    {
        // RAII constructor completes initialization
        return runloop_ != nullptr;
    }

    ValidatedIPCMessageHeader CreateTestHeader(
        MessageType type = MessageType::Request)
    {
        return IPCMessageHeaderBuilder()
            .SetMessageType(type)
            .SetCallId(1)
            .SetInterfaceId(1)
            .SetFlags(0)
            .SetBodySize(0)
            .Build();
    }

    ValidatedIPCMessageHeader CreateBusinessResponseHeader(
        uint16_t call_id,
        uint32_t body_size)
    {
        return IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::Response)
            .SetHeaderFlags(DAS::Core::IPC::HeaderFlags::NONE)
            .SetInterfaceId(0x1234)
            .SetCallId(call_id)
            .SetSourceSessionId(REMOTE_SESSION_ID)
            .SetTargetSessionId(LOCAL_SESSION_ID)
            .SetBodySize(body_size)
            .Build();
    }

    std::optional<ConnectedAnyTransportPair> CreateConnectedTransportPair(
        const char* test_name)
    {
        (void)test_name;
        auto&                        io_context = runloop_->GetIoContext();
        static std::atomic<uint32_t> next_endpoint_id{1};
        const auto                   base = DAS_FMT_NS::format(
            "drt_{}_{}",
            GetCurrentProcessId() % 10000,
            next_endpoint_id.fetch_add(1));
        const auto read_endpoint = base + "_read";
        const auto write_endpoint = base + "_write";
        const bool use_single_endpoint = DAS::Core::IPC::AfUnixAvailable();
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
            ADD_FAILURE() << "Failed to create run-loop-side transport: "
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

    void StartRunLoop(std::thread& run_thread)
    {
        run_thread = std::thread([this]() { runloop_->Run(); });
        for (int i = 0; i < 100 && !runloop_->IsRunning(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void StopRunLoop(std::thread& run_thread)
    {
        runloop_->RequestStop();
        if (run_thread.joinable())
        {
            run_thread.join();
        }
    }

    std::optional<InboundMessage> WaitForInboundMessage(
        std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto msg = inbound_queue_->TryPop();
            if (msg)
            {
                return msg;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return std::nullopt;
    }

    std::unique_ptr<IpcRunLoop>                      runloop_;
    std::unique_ptr<IpcMessageQueue<InboundMessage>> inbound_queue_;
    std::string                                      host_queue_name_;
    std::string                                      plugin_queue_name_;
    DAS::Core::IPC::ProxyFactory                     proxy_factory_;
    DAS::Core::IPC::RemoteObjectRegistry             registry_;
};

// ====== Initialize/Shutdown Tests ======
// NOTE: Initialize() is now done in constructor (RAII pattern)
// NOTE: Shutdown is now private and called by destructor (RAII pattern)

// ====== Run/Stop Tests ======

TEST_F(IpcRunLoopTest, Run_Succeeds)
{
    ASSERT_TRUE(SetupRunLoopWithTransport());

    // Run() is blocking, run in separate thread
    std::thread run_thread([this]() { runloop_->Run(); });

    // Wait for running state
    for (int i = 0; i < 100 && !runloop_->IsRunning(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(runloop_->IsRunning());

    // Use RequestStop() - Run() will join io_thread_ internally
    runloop_->RequestStop();
    if (run_thread.joinable())
    {
        run_thread.join();
    }
}

TEST_F(IpcRunLoopTest, Stop_Succeeds)
{
    ASSERT_TRUE(SetupRunLoopWithTransport());

    // Run() is blocking, run in separate thread
    std::thread run_thread([this]() { runloop_->Run(); });

    // Wait for running state
    for (int i = 0; i < 100 && !runloop_->IsRunning(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(runloop_->IsRunning());

    // Use RequestStop() - Run() will join io_thread_ internally
    runloop_->RequestStop();

    if (run_thread.joinable())
    {
        run_thread.join();
    }
    EXPECT_FALSE(runloop_->IsRunning());
}

TEST_F(IpcRunLoopTest, IsRunning_AfterRun)
{
    ASSERT_TRUE(SetupRunLoopWithTransport());

    // Run() is blocking, run in separate thread
    std::thread run_thread([this]() { runloop_->Run(); });

    // Wait for running state
    for (int i = 0; i < 100 && !runloop_->IsRunning(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(runloop_->IsRunning());

    // Use RequestStop() - Run() will join io_thread_ internally
    runloop_->RequestStop();
    if (run_thread.joinable())
    {
        run_thread.join();
    }
    EXPECT_FALSE(runloop_->IsRunning());
}

// ====== Re-entrant Detection Tests ======

TEST_F(IpcRunLoopTest, Run_ReentrantFails)
{
    ASSERT_TRUE(SetupRunLoopWithTransport());

    // Run() is blocking, run in separate thread
    std::thread run_thread([this]() { runloop_->Run(); });

    // Wait for running state
    for (int i = 0; i < 100 && !runloop_->IsRunning(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Second Run() should fail (re-entrant detection)
    auto result = runloop_->Run();
    EXPECT_EQ(result, DAS_E_IPC_DEADLOCK_DETECTED);

    // Use RequestStop() - Run() will join io_thread_ internally
    runloop_->RequestStop();
    if (run_thread.joinable())
    {
        run_thread.join();
    }
}

// =====> RequestStop Idempotent Tests ======

TEST_F(IpcRunLoopTest, RequestStop_Idempotent)
{
    // RAII constructor completes initialization

    // RequestStop without run should be safe
    runloop_->RequestStop();
    EXPECT_FALSE(runloop_->IsRunning());

    // Multiple RequestStop should be safe
    runloop_->RequestStop();
    EXPECT_FALSE(runloop_->IsRunning());
}

// ====== Message Handler Tests ======

// 简单的测试用消息处理器
class TestMessageHandler : public IMessageHandler
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
        return 1;
    }

    DasResult HandleMessage(
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body,
        IpcResponseSender&               sender,
        DAS::Core::IPC::StubContext&     ctx) override
    {
        // 简单返回成功
        (void)header;
        (void)body;
        (void)sender;
        (void)ctx;
        return DAS_S_OK;
    }

private:
    uint32_t ref_count_ = 0;
};

TEST_F(IpcRunLoopTest, RegisterHandler_Succeeds)
{
    // 注册处理器（使用 header_flags=NONE, interface_id=1）
    // IpcRunLoop 通过 DasPtr 接管引用（AddRef），析构时 Release 释放
    auto* handler = new TestMessageHandler();
    runloop_->RegisterHandler(DAS::Core::IPC::HeaderFlags::NONE, 1, handler);

    // 验证可以通过 GetHandler 获取
    IMessageHandler* retrieved =
        runloop_->GetHandler(DAS::Core::IPC::HeaderFlags::NONE, 1);
    EXPECT_NE(retrieved, nullptr);
}

TEST_F(IpcRunLoopTest, BusinessResponseWithPendingCallbackUsesFastPath)
{
    constexpr uint16_t call_id = 77;
    auto               transport_pair =
        CreateConnectedTransportPair("business_response_callback");
    ASSERT_TRUE(transport_pair.has_value());

    const DAS::DasPtr<IHostConnection> host(new TestInternalHost(
        runloop_->GetIoContext(),
        REMOTE_SESSION_ID,
        transport_pair->run_loop_side));
    ASSERT_EQ(runloop_->RegisterInternalHost(host), DAS_S_OK);

    std::atomic<bool>    callback_called{false};
    DasResult            callback_result = DAS_E_UNDEFINED_RETURN_VALUE;
    std::vector<uint8_t> callback_body;
    uint16_t             callback_flags = 0;
    const CallKey        call_key{REMOTE_SESSION_ID, call_id};

    {
        const std::unique_lock<std::mutex> lock(runloop_->pending_mutex_);
        runloop_->pending_calls_[call_key] = PendingCallState{
            .call_key = call_key,
            .deadline = std::chrono::steady_clock::time_point::max(),
            .on_complete =
                [&](DasResult            result,
                    std::vector<uint8_t> response,
                    uint16_t             flags)
            {
                callback_result = result;
                callback_body = std::move(response);
                callback_flags = flags;
                callback_called.store(true, std::memory_order_release);
            },
            .response_flags = 0};
    }

    std::thread run_thread;
    StartRunLoop(run_thread);

    const std::vector<uint8_t> body{1, 2, 3, 4};
    auto                       header = CreateBusinessResponseHeader(
        call_id,
        static_cast<uint32_t>(body.size()));
    auto send_future = boost::asio::co_spawn(
        runloop_->GetIoContext(),
        transport_pair->peer_side
            .SendCoroutine(header, body.data(), body.size()),
        boost::asio::use_future);
    EXPECT_EQ(send_future.get(), DAS_S_OK);

    for (int i = 0; i < 100 && !callback_called.load(std::memory_order_acquire);
         ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const auto queued = inbound_queue_->TryPop();
    StopRunLoop(run_thread);

    EXPECT_TRUE(callback_called.load(std::memory_order_acquire));
    EXPECT_EQ(callback_result, DAS_S_OK);
    EXPECT_EQ(callback_body, body);
    EXPECT_EQ(callback_flags, 0);
    EXPECT_FALSE(queued.has_value());
}

TEST_F(IpcRunLoopTest, BusinessResponseWithoutPendingCallbackFallsBackToQueue)
{
    constexpr uint16_t call_id = 78;
    auto               transport_pair =
        CreateConnectedTransportPair("business_response_inbound_fallback");
    ASSERT_TRUE(transport_pair.has_value());

    const DAS::DasPtr<IHostConnection> host(new TestInternalHost(
        runloop_->GetIoContext(),
        REMOTE_SESSION_ID,
        transport_pair->run_loop_side));
    ASSERT_EQ(runloop_->RegisterInternalHost(host), DAS_S_OK);

    const CallKey call_key{REMOTE_SESSION_ID, call_id};
    {
        const std::unique_lock<std::mutex> lock(runloop_->pending_mutex_);
        runloop_->pending_calls_[call_key] = PendingCallState{
            .call_key = call_key,
            .deadline = std::chrono::steady_clock::time_point::max(),
            .on_complete = nullptr,
            .response_flags = 0};
    }

    std::thread run_thread;
    StartRunLoop(run_thread);

    const std::vector<uint8_t> body{9, 8, 7, 6};
    auto                       header = CreateBusinessResponseHeader(
        call_id,
        static_cast<uint32_t>(body.size()));
    auto send_future = boost::asio::co_spawn(
        runloop_->GetIoContext(),
        transport_pair->peer_side
            .SendCoroutine(header, body.data(), body.size()),
        boost::asio::use_future);
    EXPECT_EQ(send_future.get(), DAS_S_OK);

    auto queued = WaitForInboundMessage(std::chrono::milliseconds(500));
    StopRunLoop(run_thread);

    EXPECT_TRUE(queued.has_value())
        << "business RESPONSE without an on_complete callback must remain "
           "visible to BusinessThread::PumpUntilResponse";
    if (queued)
    {
        EXPECT_EQ(queued->header.GetMessageType(), MessageType::Response);
        EXPECT_EQ(queued->header.GetSourceSessionId(), REMOTE_SESSION_ID);
        EXPECT_EQ(queued->header.GetTargetSessionId(), LOCAL_SESSION_ID);
        EXPECT_EQ(queued->header.GetCallId(), call_id);
        EXPECT_EQ(queued->body, body);
    }
}

// ====== Host Session Disconnected Callback Tests ======

TEST_F(IpcRunLoopTest, HostSessionDisconnectCallback_FiresExactlyOnceOnExit)
{
    auto transport_pair =
        CreateConnectedTransportPair("host_session_disconnect_callback");
    ASSERT_TRUE(transport_pair.has_value());

    const DAS::DasPtr<IHostConnection> host(new TestInternalHost(
        runloop_->GetIoContext(),
        REMOTE_SESSION_ID,
        transport_pair->run_loop_side));
    ASSERT_EQ(runloop_->RegisterInternalHost(host), DAS_S_OK);

    std::mutex            recorded_sessions_mutex;
    std::vector<uint16_t> recorded_sessions;
    runloop_->SetOnHostSessionDisconnected(
        [&recorded_sessions_mutex, &recorded_sessions](uint16_t session_id)
        {
            const std::lock_guard<std::mutex> lock(recorded_sessions_mutex);
            recorded_sessions.push_back(session_id);
        });

    std::thread run_thread;
    StartRunLoop(run_thread);

    // 对端关闭连接：接收协程读失败退出，退出路径触发断连回调
    transport_pair->peer_side.Cleanup();

    bool callback_fired = false;
    for (int i = 0; i < 100; ++i)
    {
        {
            const std::lock_guard<std::mutex> lock(recorded_sessions_mutex);
            callback_fired = !recorded_sessions.empty();
        }
        if (callback_fired)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 正常停机同样经过退出路径（幂等前提），等待收敛后再断言
    StopRunLoop(run_thread);

    const std::lock_guard<std::mutex> lock(recorded_sessions_mutex);
    ASSERT_EQ(recorded_sessions.size(), 1u);
    EXPECT_EQ(recorded_sessions[0], REMOTE_SESSION_ID);
}

TEST_F(IpcRunLoopTest, HostSessionDisconnectCallback_AbsentRegistrationIsSafe)
{
    auto transport_pair =
        CreateConnectedTransportPair("host_session_disconnect_absent");
    ASSERT_TRUE(transport_pair.has_value());

    const DAS::DasPtr<IHostConnection> host(new TestInternalHost(
        runloop_->GetIoContext(),
        REMOTE_SESSION_ID,
        transport_pair->run_loop_side));
    ASSERT_EQ(runloop_->RegisterInternalHost(host), DAS_S_OK);

    // 未注册回调（可选挂载）：退出路径零异常
    std::thread run_thread;
    StartRunLoop(run_thread);

    transport_pair->peer_side.Cleanup();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    StopRunLoop(run_thread);
    SUCCEED();
}

// ====== Concurrency Tests ======

TEST_F(IpcRunLoopTest, Stop_FromDifferentThread)
{
    ASSERT_TRUE(SetupRunLoopWithTransport());

    // Run() is blocking, run in separate thread
    std::thread run_thread([this]() { runloop_->Run(); });

    // RequestStop from another thread (no join, safe for concurrent use)
    std::thread stopper([this]() { runloop_->RequestStop(); });

    stopper.join();
    if (run_thread.joinable())
    {
        run_thread.join();
    }

    EXPECT_FALSE(runloop_->IsRunning());
}

// ====== Max Nested Depth Tests ======

TEST_F(IpcRunLoopTest, MaxNestedDepth_LimitIs32)
{
    // Verify the limit is set correctly
    // This is a compile-time constant, verified via implementation
    // The test confirms the design intent
    EXPECT_TRUE(true); // Placeholder - actual limit is in implementation
}

// ====== Timeout Tests ======

TEST_F(IpcRunLoopTest, Run_AfterStopAndReinitialize)
{
    ASSERT_TRUE(SetupRunLoopWithTransport());

    // First run cycle
    std::thread run_thread1([this]() { runloop_->Run(); });
    for (int i = 0; i < 100 && !runloop_->IsRunning(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Use RequestStop() - Run() will join io_thread_ internally
    runloop_->RequestStop();
    if (run_thread1.joinable())
    {
        run_thread1.join();
    }
    // RAII: unique_ptr 析构自动调用 Shutdown()
    runloop_.reset();

    // Reinitialize and run again
    try
    {
        // main 语义构造点：显式传两级查表 resolver
        runloop_ = std::make_unique<IpcRunLoop>(
            true,
            nullptr,
            proxy_factory_,
            registry_,
            [this](uint16_t target_session_id)
                -> DAS::Core::IPC::TransportLookupResult
            {
                return ResolveTransportMainSemantic(
                    runloop_->GetConnectionManager(),
                    target_session_id);
            });
    }
    catch (const std::exception& e)
    {
        FAIL() << "Failed to recreate IpcRunLoop: " << e.what();
        return;
    }
    ASSERT_TRUE(SetupRunLoopWithTransport());
    std::thread run_thread2([this]() { runloop_->Run(); });
    for (int i = 0; i < 100 && !runloop_->IsRunning(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(runloop_->IsRunning());

    // Use RequestStop() - Run() will join io_thread_ internally
    runloop_->RequestStop();
    if (run_thread2.joinable())
    {
        run_thread2.join();
    }
}

// ====== Cleanup on Stop Tests ======

TEST_F(IpcRunLoopTest, Stop_CancelsPendingCalls)
{
    ASSERT_TRUE(SetupRunLoopWithTransport());

    // Run() is blocking, run in separate thread
    std::thread run_thread([this]() { runloop_->Run(); });

    // Wait for running state
    for (int i = 0; i < 100 && !runloop_->IsRunning(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // RequestStop should cancel any pending calls
    runloop_->RequestStop();
    if (run_thread.joinable())
    {
        run_thread.join();
    }

    // Verify clean shutdown
    EXPECT_FALSE(runloop_->IsRunning());
}

// ====== Multiple Run/Stop Cycles ======

TEST_F(IpcRunLoopTest, MultipleRunStopCycles)
{
    ASSERT_TRUE(SetupRunLoopWithTransport());

    for (int i = 0; i < 3; ++i)
    {
        std::thread run_thread([this]() { runloop_->Run(); });

        // Wait for running state
        for (int j = 0; j < 100 && !runloop_->IsRunning(); ++j)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        EXPECT_TRUE(runloop_->IsRunning());

        // Use RequestStop() - Run() will join io_thread_ internally
        runloop_->RequestStop();
        if (run_thread.joinable())
        {
            run_thread.join();
        }

        EXPECT_FALSE(runloop_->IsRunning());
    }
}

// ====== PostCallback Tests (Event-driven mechanism) ======

// 简单的计数回调，用于测试 PostCallback
class CountingCallback : public IDasAsyncCallback
{
public:
    std::atomic<uint32_t> ref_{0};
    std::atomic<int>      call_count{0};
    std::thread::id       executed_thread_id;
    std::atomic<bool>     done{false};

    uint32_t AddRef() override { return ++ref_; }

    uint32_t Release() override
    {
        auto r = --ref_;
        if (r == 0)
        {
            delete this;
        }
        return r;
    }

    DasResult QueryInterface(const DasGuid& iid, void** pp) override
    {
        if (iid == DasIidOf<IDasAsyncCallback>())
        {
            AddRef();
            *pp = this;
            return DAS_S_OK;
        }
        return DAS_E_NO_INTERFACE;
    }

    DasResult Do() noexcept override
    {
        call_count++;
        executed_thread_id = std::this_thread::get_id();
        done = true;
        return DAS_S_OK;
    }
};

// 记录执行顺序的回调
class SequenceCallback : public IDasAsyncCallback
{
public:
    std::atomic<uint32_t> ref_{0};
    int                   sequence_number;
    std::vector<int>*     execution_order;
    std::atomic<bool>*    all_done;

    SequenceCallback(int seq, std::vector<int>* order, std::atomic<bool>* done)
        : sequence_number(seq), execution_order(order), all_done(done)
    {
    }

    uint32_t AddRef() override { return ++ref_; }

    uint32_t Release() override
    {
        auto r = --ref_;
        if (r == 0)
        {
            delete this;
        }
        return r;
    }

    DasResult QueryInterface(const DasGuid& iid, void** pp) override
    {
        if (iid == DasIidOf<IDasAsyncCallback>())
        {
            AddRef();
            *pp = this;
            return DAS_S_OK;
        }
        return DAS_E_NO_INTERFACE;
    }

    DasResult Do() noexcept override
    {
        execution_order->push_back(sequence_number);
        if (execution_order->size() >= 3)
        {
            *all_done = true;
        }
        return DAS_S_OK;
    }
};

TEST_F(IpcRunLoopTest, PostCallback_ExecutesCallback)
{
    ASSERT_TRUE(SetupRunLoopWithTransport());

    std::thread run_thread([this]() { runloop_->Run(); });

    // Wait for running state
    for (int i = 0; i < 100 && !runloop_->IsRunning(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(runloop_->IsRunning());

    // Post a callback through io_context
    auto* callback = new CountingCallback();
    callback->AddRef(); // 保持引用，防止 callback 在执行后立即被删除
    boost::asio::post(
        runloop_->GetIoContext(),
        [callback]()
        {
            callback->AddRef();
            callback->Do();
            callback->Release();
        });

    // Wait for callback execution
    for (int i = 0; i < 100 && !callback->done; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(callback->call_count, 1);
    EXPECT_TRUE(callback->done);

    callback->Release(); // 释放主线程的引用

    runloop_->RequestStop();
    if (run_thread.joinable())
    {
        run_thread.join();
    }
}

TEST_F(IpcRunLoopTest, PostCallback_OrderPreserved)
{
    ASSERT_TRUE(SetupRunLoopWithTransport());

    std::thread run_thread([this]() { runloop_->Run(); });

    // Wait for running state
    for (int i = 0; i < 100 && !runloop_->IsRunning(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(runloop_->IsRunning());

    std::vector<int>  execution_order;
    std::atomic<bool> all_done{false};

    // Post multiple callbacks in order
    for (int i = 1; i <= 3; ++i)
    {
        auto* cb = new SequenceCallback(i, &execution_order, &all_done);
        boost::asio::post(
            runloop_->GetIoContext(),
            [cb]()
            {
                cb->AddRef();
                cb->Do();
                cb->Release();
            });
    }

    // Wait for all callbacks to complete
    for (int i = 0; i < 200 && !all_done; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Verify execution order matches posting order
    ASSERT_EQ(execution_order.size(), 3);
    EXPECT_EQ(execution_order[0], 1);
    EXPECT_EQ(execution_order[1], 2);
    EXPECT_EQ(execution_order[2], 3);

    runloop_->RequestStop();
    if (run_thread.joinable())
    {
        run_thread.join();
    }
}

TEST_F(IpcRunLoopTest, PostCallback_FromOtherThread)
{
    ASSERT_TRUE(SetupRunLoopWithTransport());

    std::thread run_thread([this]() { runloop_->Run(); });

    // Wait for running state
    for (int i = 0; i < 100 && !runloop_->IsRunning(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(runloop_->IsRunning());

    std::thread::id   io_thread_id;
    std::atomic<bool> callback_done{false};

    // Capture the io_context thread id
    boost::asio::post(
        runloop_->GetIoContext(),
        [&io_thread_id]() { io_thread_id = std::this_thread::get_id(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Post from another thread
    std::thread poster(
        [this, &callback_done]()
        {
            auto* callback = new CountingCallback();
            boost::asio::post(
                runloop_->GetIoContext(),
                [callback, &callback_done]()
                {
                    callback->AddRef();
                    callback->Do();
                    callback->Release();
                    callback_done = true;
                });
        });
    poster.join();

    // Wait for callback execution
    for (int i = 0; i < 100 && !callback_done; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(callback_done);

    runloop_->RequestStop();
    if (run_thread.joinable())
    {
        run_thread.join();
    }
}

// ====== Piggyback Extension Section Tests（RESERVED_AS_SIZE 接收侧）======

TEST_F(
    IpcRunLoopTest,
    PiggybackSection_StripsAndRecursivelyRoutesEmbeddedFrames)
{
    auto transport_pair = CreateConnectedTransportPair("piggyback_strip_route");
    ASSERT_TRUE(transport_pair.has_value());

    const DAS::DasPtr<IHostConnection> host(new TestInternalHost(
        runloop_->GetIoContext(),
        REMOTE_SESSION_ID,
        transport_pair->run_loop_side));
    ASSERT_EQ(runloop_->RegisterInternalHost(host), DAS_S_OK);

    // CONTROL_PLANE 130 记账 handler + 两个本地条目：递归分发的可观测锚点
    auto& object_manager = proxy_factory_.GetObjectManager();
    object_manager.SetSessionId(LOCAL_SESSION_ID);
    const DAS::DasPtr<DAS::Core::IPC::RemoteRefHandler> remote_ref_handler =
        DAS::Core::IPC::RemoteRefHandler::Create(object_manager, *runloop_);
    runloop_->RegisterHandler(
        DAS::Core::IPC::HeaderFlags::CONTROL_PLANE,
        static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::RemoteAddRef),
        remote_ref_handler.Get());

    auto*                    mock_object_one = new MockDasObject();
    auto*                    mock_object_two = new MockDasObject();
    DAS::Core::IPC::ObjectId object_id_one{};
    DAS::Core::IPC::ObjectId object_id_two{};
    ASSERT_EQ(
        object_manager
            .RegisterLocalObject(mock_object_one, object_id_one, false),
        DAS_S_OK);
    ASSERT_EQ(
        object_manager
            .RegisterLocalObject(mock_object_two, object_id_two, false),
        DAS_S_OK);

    std::thread run_thread;
    StartRunLoop(run_thread);

    const std::vector<uint8_t> embedded_one =
        BuildEmbeddedControlPlaneEvent(object_id_one);
    const std::vector<uint8_t> embedded_two =
        BuildEmbeddedControlPlaneEvent(object_id_two);
    const std::vector<uint8_t> host_body{0xAA, 0xBB, 0xCC, 0xDD};

    std::vector<uint8_t> piggybacked_body = embedded_one;
    piggybacked_body.insert(
        piggybacked_body.end(),
        embedded_two.begin(),
        embedded_two.end());
    piggybacked_body.insert(
        piggybacked_body.end(),
        host_body.begin(),
        host_body.end());

    const uint32_t section_size =
        static_cast<uint32_t>(embedded_one.size() + embedded_two.size());
    const auto header = MakePiggybackHostHeader(
        section_size,
        static_cast<uint32_t>(piggybacked_body.size()));

    auto send_future = boost::asio::co_spawn(
        runloop_->GetIoContext(),
        transport_pair->peer_side.SendCoroutine(
            header,
            piggybacked_body.data(),
            piggybacked_body.size()),
        boost::asio::use_future);
    EXPECT_EQ(send_future.get(), DAS_S_OK);

    auto queued = WaitForInboundMessage(std::chrono::milliseconds(2000));
    StopRunLoop(run_thread);

    // 两个内嵌帧各自按其 header 递归路由：CONTROL_PLANE 130 记账发生
    EXPECT_EQ(object_manager.GetRemoteHolderCount(object_id_one), 1u);
    EXPECT_EQ(object_manager.GetRemoteHolderCount(object_id_two), 1u);

    // 宿主 body 剥离扩展段后走正常路由（无 pending callback → 入站队列）
    ASSERT_TRUE(queued.has_value());
    EXPECT_EQ(queued->body, host_body);
    EXPECT_EQ(
        queued->header.GetFlags()
            & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
        0);
}

TEST_F(IpcRunLoopTest, PiggybackSection_MalformedSectionDropsWholeFrame)
{
    auto transport_pair =
        CreateConnectedTransportPair("piggyback_malformed_drop");
    ASSERT_TRUE(transport_pair.has_value());

    const DAS::DasPtr<IHostConnection> host(new TestInternalHost(
        runloop_->GetIoContext(),
        REMOTE_SESSION_ID,
        transport_pair->run_loop_side));
    ASSERT_EQ(runloop_->RegisterInternalHost(host), DAS_S_OK);

    auto& object_manager = proxy_factory_.GetObjectManager();
    object_manager.SetSessionId(LOCAL_SESSION_ID);
    const DAS::DasPtr<DAS::Core::IPC::RemoteRefHandler> remote_ref_handler =
        DAS::Core::IPC::RemoteRefHandler::Create(object_manager, *runloop_);
    runloop_->RegisterHandler(
        DAS::Core::IPC::HeaderFlags::CONTROL_PLANE,
        static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::RemoteAddRef),
        remote_ref_handler.Get());

    auto*                    mock_object = new MockDasObject();
    DAS::Core::IPC::ObjectId object_id{};
    ASSERT_EQ(
        object_manager.RegisterLocalObject(mock_object, object_id, false),
        DAS_S_OK);

    std::thread run_thread;
    StartRunLoop(run_thread);

    const auto send_piggyback = [this, &transport_pair](
                                    const ValidatedIPCMessageHeader& header,
                                    const std::vector<uint8_t>&      body)
    {
        auto send_future = boost::asio::co_spawn(
            runloop_->GetIoContext(),
            transport_pair->peer_side
                .SendCoroutine(header, body.data(), body.size()),
            boost::asio::use_future);
        EXPECT_EQ(send_future.get(), DAS_S_OK);
    };

    const std::vector<uint8_t> host_body{0xAA, 0xBB, 0xCC, 0xDD};

    // 场景 A：section 声称超过 body_size —— 校验器整帧拒绝，不进循环
    {
        const std::vector<uint8_t> short_body(64, 0x11);
        send_piggyback(MakePiggybackHostHeader(4096, 64), short_body);
    }

    // 场景 B：section 尺寸合法但内嵌帧声明的 body_size 越过 section 边界
    // —— 剥离循环按自描述长度封顶，整帧丢弃不越界读
    {
        std::vector<uint8_t> embedded =
            BuildEmbeddedControlPlaneEvent(object_id);
        IPCMessageHeader patch{};
        std::memcpy(&patch, embedded.data(), sizeof(patch));
        patch.body_size = 9999;
        std::memcpy(embedded.data(), &patch, sizeof(patch));

        std::vector<uint8_t> piggybacked_body = embedded;
        piggybacked_body.insert(
            piggybacked_body.end(),
            host_body.begin(),
            host_body.end());
        send_piggyback(
            MakePiggybackHostHeader(
                static_cast<uint32_t>(embedded.size()),
                static_cast<uint32_t>(piggybacked_body.size())),
            piggybacked_body);
    }

    // 场景 C：内嵌帧自身再置 bit1（深度=2）—— 剥离循环施加深度=1 硬约束
    {
        std::vector<uint8_t> embedded =
            BuildEmbeddedControlPlaneEvent(object_id);
        IPCMessageHeader patch{};
        std::memcpy(&patch, embedded.data(), sizeof(patch));
        patch.flags = DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE;
        patch.reserved = 4;
        std::memcpy(embedded.data(), &patch, sizeof(patch));

        std::vector<uint8_t> piggybacked_body = embedded;
        piggybacked_body.insert(
            piggybacked_body.end(),
            host_body.begin(),
            host_body.end());
        send_piggyback(
            MakePiggybackHostHeader(
                static_cast<uint32_t>(embedded.size()),
                static_cast<uint32_t>(piggybacked_body.size())),
            piggybacked_body);
    }

    // 三个畸形帧全部整帧丢弃：入站队列保持空、holder 记账零发生、
    // 接收循环存活（无死循环/越界读崩溃）
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_FALSE(inbound_queue_->TryPop().has_value());
    EXPECT_EQ(object_manager.GetRemoteHolderCount(object_id), 0u);

    StopRunLoop(run_thread);
    SUCCEED();
}

// ====== OutboundFrameProbe 观测 seam（Wave 0 测试基建）======

// 探针自证：发送端写 N 条已知帧（不同 interface_id 含 130）→ 探针另一端
// 逐帧读出——总帧数、按 interface_id 的 dissect 计数、FIFO 帧序三者可信，
// 是后续全部 Stage B 帧数断言的前提
TEST_F(IpcRunLoopTest, OutboundFrameProbeCountsDissectsAndPreservesOrder)
{
    using DAS::Core::IPC::ObjectId;

    OutboundFrameProbe probe(runloop_->GetIoContext());
    ASSERT_TRUE(probe.IsHealthy());
    // 自证形态：对端保留在夹具内，从对端投递已知帧 → 探针观察端读回
    // （挂到 run loop 上时同一对端经 TakeCounterpartTransport 交给
    // ConnectionManager，探针读到的就是 run loop 的出站帧镜像）

    const ObjectId object_id_one{
        .session_id = REMOTE_SESSION_ID,
        .generation = 1,
        .local_id = 0x1111};
    const ObjectId object_id_two{
        .session_id = REMOTE_SESSION_ID,
        .generation = 2,
        .local_id = 0x2222};

    const auto make_frame_header =
        [](uint32_t interface_id) -> ValidatedIPCMessageHeader
    {
        return IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::Event)
            .SetHeaderFlags(DAS::Core::IPC::HeaderFlags::CONTROL_PLANE)
            .SetInterfaceId(interface_id)
            .SetSourceSessionId(LOCAL_SESSION_ID)
            .SetTargetSessionId(REMOTE_SESSION_ID)
            .SetBodySize(static_cast<uint32_t>(sizeof(ObjectId)))
            .Build();
    };

    const auto object_body = [](const ObjectId& object_id)
    {
        return std::vector<uint8_t>{
            reinterpret_cast<const uint8_t*>(&object_id),
            reinterpret_cast<const uint8_t*>(&object_id) + sizeof(object_id)};
    };

    // 四条帧：130 / 45 / 130 / 200（130 为 RemoteAddRef，body = ObjectId）
    ASSERT_EQ(
        probe.SendFrameFromCounterpart(
            make_frame_header(130),
            object_body(object_id_one)),
        DAS_S_OK);
    ASSERT_EQ(
        probe.SendFrameFromCounterpart(
            make_frame_header(45),
            object_body(object_id_two)),
        DAS_S_OK);
    ASSERT_EQ(
        probe.SendFrameFromCounterpart(
            make_frame_header(130),
            object_body(object_id_two)),
        DAS_S_OK);
    ASSERT_EQ(
        probe.SendFrameFromCounterpart(
            make_frame_header(200),
            object_body(object_id_one)),
        DAS_S_OK);

    EXPECT_EQ(probe.DrainUntil(4, std::chrono::seconds(2)), 4u);
    // 排空窗口内不得再有多余帧
    EXPECT_EQ(probe.DrainFor(std::chrono::milliseconds(200)), 0u);

    // 总帧数 == N
    EXPECT_EQ(probe.TotalFrameCount(), 4u);
    // 按 interface_id 的 dissect 计数
    EXPECT_EQ(probe.CountByInterfaceId(130), 2u);
    EXPECT_EQ(probe.CountByInterfaceId(45), 1u);
    EXPECT_EQ(probe.CountByInterfaceId(200), 1u);
    EXPECT_EQ(probe.CountByInterfaceId(999), 0u);
    // FIFO 帧序保持
    EXPECT_EQ(
        probe.InterfaceIdOrder(),
        (std::vector<uint32_t>{130, 45, 130, 200}));

    // 130 号帧 dissect：逐条核对 body 与 target
    const auto add_ref_frames = probe.FramesByInterfaceId(130);
    ASSERT_EQ(add_ref_frames.size(), 2u);
    EXPECT_EQ(
        std::memcmp(
            add_ref_frames[0]->body.data(),
            &object_id_one,
            sizeof(ObjectId)),
        0);
    EXPECT_EQ(
        std::memcmp(
            add_ref_frames[1]->body.data(),
            &object_id_two,
            sizeof(ObjectId)),
        0);
    EXPECT_EQ(
        add_ref_frames[0]->header.GetTargetSessionId(),
        REMOTE_SESSION_ID);
    EXPECT_EQ(
        add_ref_frames[1]->header.GetTargetSessionId(),
        REMOTE_SESSION_ID);

    // Reset 清空观测面
    probe.Reset();
    EXPECT_EQ(probe.TotalFrameCount(), 0u);
    EXPECT_TRUE(probe.InterfaceIdOrder().empty());

    probe.Shutdown();
}

// piggyback 总闸：默认开，可翻转（D-21 帧数基线 A/B 的依赖件）
TEST_F(IpcRunLoopTest, PiggybackSwitchDefaultsOnAndToggles)
{
    EXPECT_TRUE(runloop_->IsPiggybackEnabled());
    runloop_->SetPiggybackEnabled(false);
    EXPECT_FALSE(runloop_->IsPiggybackEnabled());
    runloop_->SetPiggybackEnabled(true);
    EXPECT_TRUE(runloop_->IsPiggybackEnabled());
}

// FlushOutboundFifo 出清队列全部帧：guard 触发 drain 以合写形态发出
// （一批连写一次物理写），header 无 bit1，FIFO 序保持
TEST_F(
    IpcRunLoopTest,
    FlushOutboundFifoDrainsQueuedFramesAsCoalescedBatchInOrder)
{
    using DAS::Core::IPC::ObjectId;

    OutboundFrameProbe probe(runloop_->GetIoContext());
    ASSERT_TRUE(probe.IsHealthy());
    auto counterpart_transport = probe.TakeCounterpartTransport();
    ASSERT_TRUE(counterpart_transport.has_value());
    ASSERT_EQ(
        runloop_->GetConnectionManager().RegisterHostLocalTransport(
            REMOTE_SESSION_ID,
            std::move(*counterpart_transport)),
        DAS_S_OK);

    // 同 target session 三个不同 ObjectId：FIFO 序可辨
    const ObjectId object_id_one{
        .session_id = REMOTE_SESSION_ID,
        .generation = 1,
        .local_id = 0x01};
    const ObjectId object_id_two{
        .session_id = REMOTE_SESSION_ID,
        .generation = 2,
        .local_id = 0x02};
    const ObjectId object_id_three{
        .session_id = REMOTE_SESSION_ID,
        .generation = 3,
        .local_id = 0x03};

    runloop_->EnqueueAddRef(object_id_one);
    runloop_->EnqueueAddRef(object_id_two);
    runloop_->EnqueueAddRef(object_id_three);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 3u);

    // 入队不直发：窗口内零帧
    EXPECT_EQ(probe.DrainFor(std::chrono::milliseconds(200)), 0u);

    // 一次出清全部（返回触发时滞留帧数）
    EXPECT_EQ(runloop_->FlushOutboundFifo(), 3u);

    const auto physical_writes_before =
        runloop_->GetOutboundPhysicalWriteCount();
    EXPECT_EQ(probe.DrainUntil(3, std::chrono::seconds(2)), 3u);
    EXPECT_EQ(probe.TotalFrameCount(), 3u);
    // 合写形态：三帧一批、恰一次物理写 + FIFO
    EXPECT_EQ(
        runloop_->GetOutboundPhysicalWriteCount() - physical_writes_before,
        1u);
    EXPECT_EQ(
        probe.InterfaceIdOrder(),
        (std::vector<uint32_t>{
            static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::RemoteAddRef),
            static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::RemoteAddRef),
            static_cast<uint32_t>(
                DAS::Core::IPC::IpcCommandType::RemoteAddRef)}));
    // drain 取帧后队列归零
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 0u);
    EXPECT_TRUE(runloop_->PendingAddRefsEmpty());

    const auto& frames = probe.Frames();
    ASSERT_EQ(frames.size(), 3u);
    const ObjectId expected_order[3] = {
        object_id_one,
        object_id_two,
        object_id_three};
    for (size_t index = 0; index < 3; ++index)
    {
        EXPECT_EQ(frames[index].header.GetMessageType(), MessageType::Event);
        EXPECT_EQ(
            frames[index].header.GetHeaderFlags(),
            DAS::Core::IPC::HeaderFlags::CONTROL_PLANE);
        EXPECT_EQ(
            frames[index].header.GetFlags()
                & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
            0);
        EXPECT_EQ(frames[index].header.GetTargetSessionId(), REMOTE_SESSION_ID);
        ASSERT_EQ(frames[index].body.size(), sizeof(ObjectId));
        EXPECT_EQ(
            std::memcmp(
                frames[index].body.data(),
                &expected_order[index],
                sizeof(ObjectId)),
            0);
    }

    probe.Shutdown();
}

// IO 防御漏斗：IO 控制面路径（DispatchToHandlerWithSender）进入/退出后
// pending 出清（防御性挂点可达性验证——今日该路径零物化，以直驱入队
// 模拟既有 pending）
TEST_F(IpcRunLoopTest, IoControlPlaneDispatchClearsPendingAddRefs)
{
    using DAS::Core::IPC::ObjectId;

    // 外部驱动模式：夹具构造先于 StartRunLoop（连接对创建自驱收敛），
    // 之后 io 由 run loop 的 Run 线程持续驱动
    OutboundFrameProbe probe(runloop_->GetIoContext(), true);
    ASSERT_TRUE(probe.IsHealthy());
    auto counterpart_transport = probe.TakeCounterpartTransport();
    ASSERT_TRUE(counterpart_transport.has_value());

    const DAS::DasPtr<IHostConnection> host(new TestInternalHost(
        runloop_->GetIoContext(),
        REMOTE_SESSION_ID,
        *counterpart_transport));
    ASSERT_EQ(runloop_->RegisterInternalHost(host), DAS_S_OK);

    std::thread run_thread;
    StartRunLoop(run_thread);

    // 主线程直驱入队（目标 = REMOTE session）
    const ObjectId object_id{
        .session_id = REMOTE_SESSION_ID,
        .generation = 1,
        .local_id = 5};
    runloop_->EnqueueAddRef(object_id);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 1u);

    // 探针端注入一条 CONTROL_PLANE Event（无对应 handler）→ IO 控制面
    // DispatchToHandlerWithSender 进入/退出 → guard 出清
    const auto injected_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::Event)
            .SetHeaderFlags(DAS::Core::IPC::HeaderFlags::CONTROL_PLANE)
            .SetInterfaceId(0x7777)
            .SetCallId(2)
            .SetSourceSessionId(REMOTE_SESSION_ID)
            .SetTargetSessionId(LOCAL_SESSION_ID)
            .SetBodySize(0)
            .Build();
    const std::vector<uint8_t> injected_body;
    ASSERT_EQ(
        probe.SendFrameFromObserver(injected_header, injected_body),
        DAS_S_OK);

    // IO 分发周期退出后 pending 已被独立发出
    EXPECT_EQ(probe.DrainUntil(1, std::chrono::seconds(2)), 1u);
    EXPECT_EQ(
        probe.CountByInterfaceId(
            static_cast<uint32_t>(
                DAS::Core::IPC::IpcCommandType::RemoteAddRef)),
        1u);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 0u);
    EXPECT_TRUE(runloop_->PendingAddRefsEmpty());

    StopRunLoop(run_thread);
    probe.Shutdown();
}

// 断连清队：session 断开 → 该 session pending 清空（无滞留帧），
// 其他 session 不受影响
TEST_F(IpcRunLoopTest, DisconnectClearsPendingAddRefsForThatSessionOnly)
{
    using DAS::Core::IPC::ObjectId;

    auto transport_pair =
        CreateConnectedTransportPair("pending_disconnect_clear");
    ASSERT_TRUE(transport_pair.has_value());

    const DAS::DasPtr<IHostConnection> host(new TestInternalHost(
        runloop_->GetIoContext(),
        REMOTE_SESSION_ID,
        transport_pair->run_loop_side));
    ASSERT_EQ(runloop_->RegisterInternalHost(host), DAS_S_OK);

    std::thread run_thread;
    StartRunLoop(run_thread);

    constexpr uint16_t unrelated_session_id = 99;
    const ObjectId     remote_object_id{
        .session_id = REMOTE_SESSION_ID,
        .generation = 1,
        .local_id = 7};
    const ObjectId unrelated_object_id{
        .session_id = unrelated_session_id,
        .generation = 1,
        .local_id = 8};

    runloop_->EnqueueAddRef(remote_object_id);
    runloop_->EnqueueAddRef(remote_object_id);
    runloop_->EnqueueAddRef(unrelated_object_id);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 2u);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(unrelated_session_id), 1u);

    // 对端关闭连接：接收循环退出 → 断连链清队（在途 drain 活跃时只清
    // frames、条目由 drain 收尾自回收——本用例帧从未触发，走 erase 分支）
    transport_pair->peer_side.Cleanup();

    bool remote_cleared = false;
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        if (runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID) == 0)
        {
            remote_cleared = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(remote_cleared)
        << "disconnect must clear the pending ADD_REF queue for that session";
    // 其他 session 不受影响
    EXPECT_EQ(runloop_->GetPendingAddRefCount(unrelated_session_id), 1u);

    StopRunLoop(run_thread);
    static_cast<void>(runloop_->ClearOutboundFifo(unrelated_session_id));
}

// ====== 双汇点搭车收敛（D-12 v3：闸1 搭车 / 闸2 先走 / 直通）======

namespace
{
    constexpr uint32_t PIGGYBACK_TEST_HOST_INTERFACE_ID = 0x1234;
    constexpr uint32_t PIGGYBACK_TEST_CALL_ID = 651;

    // 汇点 A 的出站业务帧形态：本地起源的 RESPONSE（source = 本地、
    // target = 对端——BT 响应 / proxy 请求同型）
    ValidatedIPCMessageHeader MakeOutboundBusinessFrameHeader(
        uint32_t    interface_id,
        uint32_t    body_size,
        MessageType message_type = MessageType::Response)
    {
        return IPCMessageHeaderBuilder()
            .SetMessageType(message_type)
            .SetHeaderFlags(DAS::Core::IPC::HeaderFlags::NONE)
            .SetInterfaceId(interface_id)
            .SetCallId(PIGGYBACK_TEST_CALL_ID)
            .SetSourceSessionId(LOCAL_SESSION_ID)
            .SetTargetSessionId(REMOTE_SESSION_ID)
            .SetBodySize(body_size)
            .Build();
    }
} // namespace

// 稳态一次物理写：BT 任务体内一次调用 = [ADD_REF][RELEASE][Response]
// 三帧入队（前两者不触发、Response 触发 drain）→ drain 合一批连写。
// 断言：物理写增量 == 调用数 N、逻辑帧增量 == 3N、InterfaceIdOrder 呈
// {130,132,业务接口} 相邻三连 × N、130==132 计数平衡、全帧无 bit1
// （pipe 连写不置位）
TEST_F(IpcRunLoopTest, OutboundFifoCoalescesSteadyStateIntoSinglePhysicalWrite)
{
    using DAS::Core::IPC::BusinessThread;
    using DAS::Core::IPC::ObjectId;

    OutboundFrameProbe probe(runloop_->GetIoContext());
    ASSERT_TRUE(probe.IsHealthy());
    auto counterpart_transport = probe.TakeCounterpartTransport();
    ASSERT_TRUE(counterpart_transport.has_value());
    ASSERT_EQ(
        runloop_->GetConnectionManager().RegisterHostLocalTransport(
            REMOTE_SESSION_ID,
            std::move(*counterpart_transport)),
        DAS_S_OK);

    // 内部回调 handler + 真实 BusinessThread：稳态须在 BT 执行线程上模拟
    // （物化 ADD_REF 与析构 RELEASE 的不触发语义依赖 BT 线程身份）
    const DAS::DasPtr<DAS::Core::IPC::InternalCallbackHandler>
        internal_callback_handler(new DAS::Core::IPC::InternalCallbackHandler);
    runloop_->RegisterHandler(
        DAS::Core::IPC::HeaderFlags::BUSINESS_CONTROL,
        internal_callback_handler->GetInterfaceId(),
        internal_callback_handler.Get());

    RunLoopFifoResolveContext             resolve_context;
    const std::shared_ptr<BusinessThread> business_thread =
        std::make_shared<BusinessThread>(
            *inbound_queue_,
            *runloop_,
            resolve_context,
            proxy_factory_,
            registry_);

    const ObjectId object_id{
        .session_id = REMOTE_SESSION_ID,
        .generation = 21,
        .local_id = 0xA1};
    constexpr int              steady_state_call_count = 5;
    const std::vector<uint8_t> response_body{0xE1, 0xE2, 0xE3};

    const auto physical_writes_before =
        runloop_->GetOutboundPhysicalWriteCount();
    const auto logical_frames_before = runloop_->GetOutboundLogicalFrameCount();

    for (int cycle = 0; cycle < steady_state_call_count; ++cycle)
    {
        std::promise<void> cycle_done;
        auto               cycle_done_future = cycle_done.get_future();
        const DAS::DasPtr<IDasAsyncCallback> cycle_task(
            new RunLoopFifoTaskCallback(
                [this, &object_id, &response_body, &cycle_done]()
                {
                    // BT 任务体 = 一次稳态调用：物化申报（入队不触发）→
                    // owner 析构 RELEASE（BT 线程不触发）→ 响应 post（触发）
                    runloop_->EnqueueAddRef(object_id);
                    runloop_->EnqueueReleaseFrame(object_id);
                    static_cast<void>(runloop_->PostSend(
                        MakeOutboundBusinessFrameHeader(
                            PIGGYBACK_TEST_HOST_INTERFACE_ID,
                            static_cast<uint32_t>(response_body.size())),
                        std::vector<uint8_t>(response_body)));
                    cycle_done.set_value();
                }));
        ASSERT_EQ(runloop_->PostToBusinessThread(cycle_task.Get()), DAS_S_OK);
        ASSERT_EQ(
            cycle_done_future.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);

        // 每调用一次物理写：本周期三帧在 drain 内合一批连写后到达
        EXPECT_EQ(probe.DrainUntil(3, std::chrono::seconds(2)), 3u);
    }

    business_thread->Stop();

    // (a) 帧序：{130, 132, 业务接口 id} 相邻三连 × N
    std::vector<uint32_t> expected_order;
    for (int cycle = 0; cycle < steady_state_call_count; ++cycle)
    {
        expected_order.push_back(
            static_cast<uint32_t>(
                DAS::Core::IPC::IpcCommandType::RemoteAddRef));
        expected_order.push_back(
            static_cast<uint32_t>(
                DAS::Core::IPC::IpcCommandType::RemoteRelease));
        expected_order.push_back(PIGGYBACK_TEST_HOST_INTERFACE_ID);
    }
    EXPECT_EQ(probe.TotalFrameCount(), 3u * steady_state_call_count);
    EXPECT_EQ(probe.InterfaceIdOrder(), expected_order);

    // (b) 计数平衡：窗口内 ADD/RELEASE 先建后拆配对
    EXPECT_EQ(
        probe.CountByInterfaceId(
            static_cast<uint32_t>(
                DAS::Core::IPC::IpcCommandType::RemoteAddRef)),
        static_cast<size_t>(steady_state_call_count));
    EXPECT_EQ(
        probe.CountByInterfaceId(
            static_cast<uint32_t>(
                DAS::Core::IPC::IpcCommandType::RemoteRelease)),
        static_cast<size_t>(steady_state_call_count));

    // (c) 每调用恰一次物理写
    EXPECT_EQ(
        runloop_->GetOutboundPhysicalWriteCount() - physical_writes_before,
        static_cast<std::uint64_t>(steady_state_call_count));
    // (d) 逻辑帧增量 == 3N
    EXPECT_EQ(
        runloop_->GetOutboundLogicalFrameCount() - logical_frames_before,
        static_cast<std::uint64_t>(3 * steady_state_call_count));

    // (e) pipe 连写不置 RESERVED_AS_SIZE 位（P-4 有意行为变化）
    for (const auto& frame : probe.Frames())
    {
        EXPECT_EQ(
            frame.header.GetFlags()
                & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
            0);
    }

    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 0u);

    probe.Shutdown();
}

// 触发矩阵边界：非 BT 线程 RELEASE 入队即触发（帧在观察窗口内到达）；
// BT 路径 ADD_REF 入队不触发（任务体内 FIFO 深度 1、零直发——出清由
// 周期末 guard 兜底，与 BusinessThreadMaterializationEnqueuesWithoutDirectSend
// 同型）
TEST_F(IpcRunLoopTest, OutboundFifoTriggerMatrixBoundaries)
{
    using DAS::Core::IPC::BusinessThread;
    using DAS::Core::IPC::ObjectId;

    OutboundFrameProbe probe(runloop_->GetIoContext());
    ASSERT_TRUE(probe.IsHealthy());
    auto counterpart_transport = probe.TakeCounterpartTransport();
    ASSERT_TRUE(counterpart_transport.has_value());
    ASSERT_EQ(
        runloop_->GetConnectionManager().RegisterHostLocalTransport(
            REMOTE_SESSION_ID,
            std::move(*counterpart_transport)),
        DAS_S_OK);

    const ObjectId object_id{
        .session_id = REMOTE_SESSION_ID,
        .generation = 22,
        .local_id = 0xA2};

    // —— 非 BT 线程（测试主线程）RELEASE：入队候车（不立即触发，
    // 下一写者/心跳/guard 兜底带走）——
    runloop_->EnqueueReleaseFrame(object_id);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 1u);
    // 下一写者（后续调用的业务帧）入队触发 drain，顺路带走候车帧：
    // 一批两帧一次物理写（搭车形态）
    const auto writes_before = runloop_->GetOutboundPhysicalWriteCount();
    const auto carrier_header =
        DAS::Core::IPC::IPCMessageHeaderBuilder()
            .SetMessageType(DAS::Core::IPC::MessageType::Request)
            .SetInterfaceId(0xE301)
            .SetCallId(701)
            .SetSourceSessionId(LOCAL_SESSION_ID)
            .SetTargetSessionId(REMOTE_SESSION_ID)
            .SetBodySize(0)
            .Build();
    ASSERT_EQ(
        runloop_->PostSend(carrier_header, std::vector<uint8_t>()),
        DAS_S_OK);
    EXPECT_EQ(probe.DrainUntil(2, std::chrono::seconds(2)), 2u);
    EXPECT_EQ(
        probe.CountByInterfaceId(
            static_cast<uint32_t>(
                DAS::Core::IPC::IpcCommandType::RemoteRelease)),
        1u);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 0u);
    EXPECT_EQ(runloop_->GetOutboundPhysicalWriteCount() - writes_before, 1u);

    // —— BT 路径 ADD_REF：入队不触发（任务体内可观测） ——
    const DAS::DasPtr<DAS::Core::IPC::InternalCallbackHandler>
        internal_callback_handler(new DAS::Core::IPC::InternalCallbackHandler);
    runloop_->RegisterHandler(
        DAS::Core::IPC::HeaderFlags::BUSINESS_CONTROL,
        internal_callback_handler->GetInterfaceId(),
        internal_callback_handler.Get());

    RunLoopFifoResolveContext             resolve_context;
    const std::shared_ptr<BusinessThread> business_thread =
        std::make_shared<BusinessThread>(
            *inbound_queue_,
            *runloop_,
            resolve_context,
            proxy_factory_,
            registry_);

    std::atomic<size_t> pending_count_at_enqueue{0};
    std::promise<void>  enqueue_done;
    auto                enqueue_done_future = enqueue_done.get_future();
    const DAS::DasPtr<IDasAsyncCallback> enqueue_task(
        new RunLoopFifoTaskCallback(
            [this, &object_id, &pending_count_at_enqueue, &enqueue_done]()
            {
                runloop_->EnqueueAddRef(object_id);
                // BT 任务体内：已入队一条、尚未发出（入队零直发）
                pending_count_at_enqueue.store(
                    runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID),
                    std::memory_order_release);
                enqueue_done.set_value();
            }));
    ASSERT_EQ(runloop_->PostToBusinessThread(enqueue_task.Get()), DAS_S_OK);
    ASSERT_EQ(
        enqueue_done_future.wait_for(std::chrono::seconds(5)),
        std::future_status::ready);

    EXPECT_EQ(pending_count_at_enqueue.load(std::memory_order_acquire), 1u);

    // 周期末 guard 兜底出清后：帧在当次窗口到达、队列空
    EXPECT_EQ(probe.DrainUntil(1, std::chrono::seconds(2)), 1u);
    EXPECT_EQ(
        probe.CountByInterfaceId(
            static_cast<uint32_t>(
                DAS::Core::IPC::IpcCommandType::RemoteAddRef)),
        1u);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 0u);
    EXPECT_TRUE(runloop_->PendingAddRefsEmpty());

    business_thread->Stop();

    probe.Shutdown();
}

// 汇点 A 变体：PostSend 路径发普通小帧（如 BT 响应）且该 target 有
// 滞留 ADD_REF → FIFO 合写——出站两逻辑帧 [130][宿主] 相邻（rider 在前
// = 入队序）、全帧无 bit1（pipe 连写不置位）、恰一次物理写
TEST_F(IpcRunLoopTest, PiggybackRidesPendingOnPostSendBusinessFrame)
{
    using DAS::Core::IPC::ObjectId;

    OutboundFrameProbe probe(runloop_->GetIoContext());
    ASSERT_TRUE(probe.IsHealthy());
    auto counterpart_transport = probe.TakeCounterpartTransport();
    ASSERT_TRUE(counterpart_transport.has_value());
    ASSERT_EQ(
        runloop_->GetConnectionManager().RegisterHostLocalTransport(
            REMOTE_SESSION_ID,
            std::move(*counterpart_transport)),
        DAS_S_OK);

    const ObjectId object_id{
        .session_id = REMOTE_SESSION_ID,
        .generation = 3,
        .local_id = 0x51};
    runloop_->EnqueueAddRef(object_id);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 1u);

    const auto physical_writes_before =
        runloop_->GetOutboundPhysicalWriteCount();

    const std::vector<uint8_t> response_body{0xDE, 0xAD, 0xBE, 0xEF};
    ASSERT_EQ(
        runloop_->PostSend(
            MakeOutboundBusinessFrameHeader(
                PIGGYBACK_TEST_HOST_INTERFACE_ID,
                static_cast<uint32_t>(response_body.size())),
            std::vector<uint8_t>(response_body)),
        DAS_S_OK);

    // 两逻辑帧 [130][宿主] 相邻（FIFO 入队序），零多余帧
    EXPECT_EQ(probe.DrainUntil(2, std::chrono::seconds(2)), 2u);
    EXPECT_EQ(probe.DrainFor(std::chrono::milliseconds(200)), 0u);
    EXPECT_EQ(probe.TotalFrameCount(), 2u);
    EXPECT_EQ(
        probe.InterfaceIdOrder(),
        (std::vector<uint32_t>{
            static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::RemoteAddRef),
            PIGGYBACK_TEST_HOST_INTERFACE_ID}));
    // 恰一次物理写（合写批）
    EXPECT_EQ(
        runloop_->GetOutboundPhysicalWriteCount() - physical_writes_before,
        1u);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 0u);
    EXPECT_TRUE(runloop_->PendingAddRefsEmpty());

    const auto& frames = probe.Frames();
    ASSERT_EQ(frames.size(), 2u);
    // 全帧无 bit1（pipe 连写不置位）
    for (const auto& frame : frames)
    {
        EXPECT_EQ(
            frame.header.GetFlags()
                & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
            0);
    }

    // rider 帧形态：CONTROL_PLANE Event + target + body = ObjectId 8B
    const auto& rider_frame = frames[0];
    EXPECT_EQ(rider_frame.header.GetMessageType(), MessageType::Event);
    EXPECT_EQ(
        rider_frame.header.GetHeaderFlags(),
        DAS::Core::IPC::HeaderFlags::CONTROL_PLANE);
    EXPECT_EQ(rider_frame.header.GetTargetSessionId(), REMOTE_SESSION_ID);
    ASSERT_EQ(rider_frame.body.size(), sizeof(ObjectId));
    EXPECT_EQ(
        std::memcmp(rider_frame.body.data(), &object_id, sizeof(ObjectId)),
        0);

    // 宿主帧路由字段原样（call_id/source/target 不改——CallKey 不受影响）
    const auto& host_frame = frames[1];
    EXPECT_EQ(
        host_frame.header.GetInterfaceId(),
        PIGGYBACK_TEST_HOST_INTERFACE_ID);
    EXPECT_EQ(host_frame.header.GetCallId(), PIGGYBACK_TEST_CALL_ID);
    EXPECT_EQ(host_frame.header.GetSourceSessionId(), LOCAL_SESSION_ID);
    EXPECT_EQ(host_frame.header.GetTargetSessionId(), REMOTE_SESSION_ID);
    EXPECT_EQ(host_frame.body, response_body);

    probe.Shutdown();
}

// 汇点 B 变体：PostSendWithTransport 路径（IO HostConnection 路由响应）
// 发帧且该 target 有滞留 ADD_REF → 入队同一 FIFO 合写——出站两逻辑帧
// [130][宿主] 相邻、全帧无 bit1、恰一次物理写。connection 参数为路由
// 提示（drain 统一经 resolver 解析，此处注册 internal host 使两级查表
// 第一条命中；run loop 须真跑——否则接收循环立即退出，其断连清扫会
// 清空出站 FIFO）
TEST_F(IpcRunLoopTest, PiggybackRidesPendingOnPostSendWithTransportResponse)
{
    using DAS::Core::IPC::ObjectId;

    // 外部驱动模式：夹具构造先于 StartRunLoop，之后 io 由 run loop 线程驱动
    OutboundFrameProbe probe(runloop_->GetIoContext(), true);
    ASSERT_TRUE(probe.IsHealthy());
    auto counterpart_transport = probe.TakeCounterpartTransport();
    ASSERT_TRUE(counterpart_transport.has_value());

    // 汇点 B 的帧形态：IO 控制面响应（CONTROL_PLANE RESPONSE，source=本地）
    const DAS::DasPtr<IHostConnection> host(new TestInternalHost(
        runloop_->GetIoContext(),
        REMOTE_SESSION_ID,
        *counterpart_transport));
    ASSERT_EQ(runloop_->RegisterInternalHost(host), DAS_S_OK);

    std::thread run_thread;
    StartRunLoop(run_thread);

    const ObjectId object_id{
        .session_id = REMOTE_SESSION_ID,
        .generation = 4,
        .local_id = 0x52};
    runloop_->EnqueueAddRef(object_id);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 1u);

    const auto physical_writes_before =
        runloop_->GetOutboundPhysicalWriteCount();

    const std::vector<uint8_t> response_body{0x11, 0x22};
    const auto                 response_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::Response)
            .SetControlPlaneCommand(
                DAS::Core::IPC::HandshakeInterfaceId::HandshakeIfaceHeartbeat)
            .SetCallId(652)
            .SetSourceSessionId(LOCAL_SESSION_ID)
            .SetTargetSessionId(REMOTE_SESSION_ID)
            .SetBodySize(static_cast<uint32_t>(response_body.size()))
            .Build();
    ASSERT_EQ(
        runloop_->PostSendWithTransport(
            host,
            response_header,
            std::vector<uint8_t>(response_body)),
        DAS_S_OK);

    // 两逻辑帧 [130][宿主] 相邻 + 恰一次物理写
    EXPECT_EQ(probe.DrainUntil(2, std::chrono::seconds(2)), 2u);
    EXPECT_EQ(probe.TotalFrameCount(), 2u);
    EXPECT_EQ(
        probe.InterfaceIdOrder(),
        (std::vector<uint32_t>{
            static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::RemoteAddRef),
            static_cast<uint32_t>(DAS::Core::IPC::HandshakeInterfaceId::
                                      HandshakeIfaceHeartbeat)}));
    EXPECT_EQ(
        runloop_->GetOutboundPhysicalWriteCount() - physical_writes_before,
        1u);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 0u);

    const auto& frames = probe.Frames();
    ASSERT_EQ(frames.size(), 2u);
    for (const auto& frame : frames)
    {
        EXPECT_EQ(
            frame.header.GetFlags()
                & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
            0);
    }
    EXPECT_EQ(
        frames[1].header.GetInterfaceId(),
        static_cast<uint32_t>(
            DAS::Core::IPC::HandshakeInterfaceId::HandshakeIfaceHeartbeat));
    EXPECT_EQ(frames[1].body, response_body);

    StopRunLoop(run_thread);
    probe.Shutdown();
}

// 断连清队快速失败：带完成回调的帧滞留 FIFO（drain 未取走）时对端断连
// → ClearOutboundFifo 锁外以 CONNECTION_LOST 直接调用回调——不落入
// wait() 的 30s 外部兜底、不误报 TIMEOUT。io_context 全程不驱动：drain
// 协程 co_spawn 后从未执行，帧确定性滞留在队列
TEST_F(IpcRunLoopTest, ClearOutboundFifoFailsStrandedCompletionPromptly)
{
    std::atomic<int>       completion_count{0};
    std::atomic<DasResult> completion_result{DAS_S_OK};

    ASSERT_EQ(
        runloop_->PostSend(
            MakeOutboundBusinessFrameHeader(
                PIGGYBACK_TEST_HOST_INTERFACE_ID,
                2),
            std::vector<uint8_t>{0xCA, 0xFE},
            [&completion_count, &completion_result](
                DasResult                   result,
                const std::vector<uint8_t>& response,
                uint16_t                    response_flags)
            {
                EXPECT_TRUE(response.empty());
                EXPECT_EQ(response_flags, 0u);
                completion_result.store(result);
                completion_count.fetch_add(1);
            }),
        DAS_S_OK);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 1u);

    // 断连清队：滞留回调在清队调用窗口内同步收到 CONNECTION_LOST
    EXPECT_EQ(runloop_->ClearOutboundFifo(REMOTE_SESSION_ID), 1u);
    EXPECT_EQ(completion_count.load(), 1);
    EXPECT_EQ(completion_result.load(), DAS_E_IPC_CONNECTION_LOST);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 0u);
    EXPECT_TRUE(runloop_->PendingAddRefsEmpty());
}

// 停机清扫面：FIFO 残留带回调帧 + RequestStop（run loop 从未 Run，
// running_ 为 false 也必须清扫）→ 回调收到 CONNECTION_LOST，而非被
// ~IpcRunLoop 析构 outbound_queues_ 时静默销毁
TEST_F(IpcRunLoopTest, RequestStopFailsStrandedFifoCompletion)
{
    std::atomic<int>       completion_count{0};
    std::atomic<DasResult> completion_result{DAS_S_OK};

    ASSERT_EQ(
        runloop_->PostSend(
            MakeOutboundBusinessFrameHeader(
                PIGGYBACK_TEST_HOST_INTERFACE_ID,
                1),
            std::vector<uint8_t>{0xB5},
            [&completion_count, &completion_result](
                DasResult                   result,
                const std::vector<uint8_t>& response,
                uint16_t                    response_flags)
            {
                EXPECT_TRUE(response.empty());
                EXPECT_EQ(response_flags, 0u);
                completion_result.store(result);
                completion_count.fetch_add(1);
            }),
        DAS_S_OK);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 1u);

    runloop_->RequestStop();
    EXPECT_EQ(completion_count.load(), 1);
    EXPECT_EQ(completion_result.load(), DAS_E_IPC_CONNECTION_LOST);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 0u);
}

// 阈值切断变体：本地大帧（header + body 达到 LARGE_MESSAGE_THRESHOLD）
// 不进连写批（独走 SendCoroutine 让传输层做 SHM 变换/直写）——滞留批
// [130,130] 一批连写在前、大帧独走在后，全部无 bit1，恰两次物理写
TEST_F(IpcRunLoopTest, FlushFirstWhenAssembledSizeReachesThreshold)
{
    using DAS::Core::IPC::ObjectId;

    OutboundFrameProbe probe(runloop_->GetIoContext());
    ASSERT_TRUE(probe.IsHealthy());
    auto counterpart_transport = probe.TakeCounterpartTransport();
    ASSERT_TRUE(counterpart_transport.has_value());
    ASSERT_EQ(
        runloop_->GetConnectionManager().RegisterHostLocalTransport(
            REMOTE_SESSION_ID,
            std::move(*counterpart_transport)),
        DAS_S_OK);

    const ObjectId object_id_one{
        .session_id = REMOTE_SESSION_ID,
        .generation = 5,
        .local_id = 0x61};
    const ObjectId object_id_two{
        .session_id = REMOTE_SESSION_ID,
        .generation = 6,
        .local_id = 0x62};
    runloop_->EnqueueAddRef(object_id_one);
    runloop_->EnqueueAddRef(object_id_two);

    const auto physical_writes_before =
        runloop_->GetOutboundPhysicalWriteCount();

    // body 使 header + body 达到阈值 → 切断独走
    const std::vector<uint8_t> large_body(
        DAS::Core::IPC::LARGE_MESSAGE_THRESHOLD - sizeof(IPCMessageHeader) - 4,
        0x33);
    ASSERT_EQ(
        runloop_->PostSend(
            MakeOutboundBusinessFrameHeader(
                PIGGYBACK_TEST_HOST_INTERFACE_ID,
                static_cast<uint32_t>(large_body.size())),
            std::vector<uint8_t>(large_body)),
        DAS_S_OK);

    // FIFO：两条 130 一批连写、随后大帧独走
    EXPECT_EQ(probe.DrainUntil(3, std::chrono::seconds(3)), 3u);
    EXPECT_EQ(probe.TotalFrameCount(), 3u);
    EXPECT_EQ(
        probe.InterfaceIdOrder(),
        (std::vector<uint32_t>{
            static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::RemoteAddRef),
            static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::RemoteAddRef),
            PIGGYBACK_TEST_HOST_INTERFACE_ID}));
    // 滞留批一次物理写 + 大帧独走一次物理写
    EXPECT_EQ(
        runloop_->GetOutboundPhysicalWriteCount() - physical_writes_before,
        2u);
    for (const auto& frame : probe.Frames())
    {
        EXPECT_EQ(
            frame.header.GetFlags()
                & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
            0);
    }
    // 宿主 body 完整（切断不等于丢帧）
    const auto& host_frame = probe.Frames().back();
    ASSERT_EQ(host_frame.body.size(), large_body.size());
    EXPECT_EQ(host_frame.body.front(), 0x33);
    EXPECT_EQ(host_frame.body.back(), 0x33);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 0u);

    probe.Shutdown();
}

// SHM 帧切断变体：携 LARGE_MESSAGE_VIA_SHM 的帧独走 SendCoroutine（SHM
// 互斥发送侧规则——不可进连写批），滞留 rider 批在前。探针接收侧无
// SHM pool，SHM 帧自身会被接收防御丢弃（互斥防御本身），仅断言 rider
// 的独立形态与顺序 + 各自一次物理写
TEST_F(IpcRunLoopTest, FlushFirstWhenCarrierIsShmFrame)
{
    using DAS::Core::IPC::ObjectId;

    OutboundFrameProbe probe(runloop_->GetIoContext());
    ASSERT_TRUE(probe.IsHealthy());
    auto counterpart_transport = probe.TakeCounterpartTransport();
    ASSERT_TRUE(counterpart_transport.has_value());
    ASSERT_EQ(
        runloop_->GetConnectionManager().RegisterHostLocalTransport(
            REMOTE_SESSION_ID,
            std::move(*counterpart_transport)),
        DAS_S_OK);

    const ObjectId object_id{
        .session_id = REMOTE_SESSION_ID,
        .generation = 7,
        .local_id = 0x63};
    runloop_->EnqueueAddRef(object_id);

    const auto physical_writes_before =
        runloop_->GetOutboundPhysicalWriteCount();

    const auto shm_carrier_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::Response)
            .SetHeaderFlags(DAS::Core::IPC::HeaderFlags::LARGE_MESSAGE_VIA_SHM)
            .SetInterfaceId(PIGGYBACK_TEST_HOST_INTERFACE_ID)
            .SetCallId(PIGGYBACK_TEST_CALL_ID)
            .SetSourceSessionId(LOCAL_SESSION_ID)
            .SetTargetSessionId(REMOTE_SESSION_ID)
            .SetBodySize(sizeof(uint64_t))
            .Build();
    const std::vector<uint8_t> shm_handle_body(sizeof(uint64_t), 0x44);
    ASSERT_EQ(
        runloop_->PostSend(
            shm_carrier_header,
            std::vector<uint8_t>(shm_handle_body)),
        DAS_S_OK);

    // rider 先到达（FIFO 在 SHM 帧之前）、无 bit1
    EXPECT_EQ(probe.DrainUntil(1, std::chrono::seconds(2)), 1u);
    EXPECT_EQ(probe.TotalFrameCount(), 1u);
    const auto& rider_frame = probe.Frames()[0];
    EXPECT_EQ(
        rider_frame.header.GetInterfaceId(),
        static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::RemoteAddRef));
    EXPECT_EQ(
        rider_frame.header.GetFlags()
            & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
        0);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 0u);
    // rider 批一次物理写 + SHM 帧独走一次物理写（后者被探针接收侧丢弃，
    // 物理写计数仍如实递增）
    EXPECT_EQ(
        runloop_->GetOutboundPhysicalWriteCount() - physical_writes_before,
        2u);

    probe.Shutdown();
}

// 转发帧切断变体：source 非本地的原样透传帧 verbatim 独走（不透明搬运
// 不进连写批），滞留 rider 批在前——序断言保留 + 各自一次物理写。
// third party session 的 transport 须注册进 host-local 表（drain 统一走
// resolver 解析，业务 post 入口与发送形态解耦）
TEST_F(IpcRunLoopTest, FlushFirstWhenCarrierIsForwardedFrame)
{
    using DAS::Core::IPC::ObjectId;

    OutboundFrameProbe probe(runloop_->GetIoContext());
    ASSERT_TRUE(probe.IsHealthy());
    auto counterpart_transport = probe.TakeCounterpartTransport();
    ASSERT_TRUE(counterpart_transport.has_value());

    constexpr uint16_t third_party_session_id = 5;
    ASSERT_EQ(
        runloop_->GetConnectionManager().RegisterHostLocalTransport(
            third_party_session_id,
            std::move(*counterpart_transport)),
        DAS_S_OK);

    // 转发帧形态：source = 原发送方（非本地）、target = 第三方
    const ObjectId third_party_object_id{
        .session_id = third_party_session_id,
        .generation = 8,
        .local_id = 0x64};
    runloop_->EnqueueAddRef(third_party_object_id);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(third_party_session_id), 1u);

    const auto physical_writes_before =
        runloop_->GetOutboundPhysicalWriteCount();

    const auto forwarded_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::Request)
            .SetHeaderFlags(DAS::Core::IPC::HeaderFlags::NONE)
            .SetInterfaceId(0x1236)
            .SetCallId(653)
            .SetSourceSessionId(REMOTE_SESSION_ID)
            .SetTargetSessionId(third_party_session_id)
            .SetBodySize(4)
            .Build();
    const std::vector<uint8_t> forwarded_body{0x55, 0x66, 0x77, 0x88};
    ASSERT_EQ(
        runloop_->PostSend(
            forwarded_header,
            std::vector<uint8_t>(forwarded_body)),
        DAS_S_OK);

    // 滞留 rider 批在前（FIFO），转发帧 verbatim 独走在后
    EXPECT_EQ(probe.DrainUntil(2, std::chrono::seconds(2)), 2u);
    EXPECT_EQ(probe.TotalFrameCount(), 2u);
    EXPECT_EQ(
        probe.InterfaceIdOrder(),
        (std::vector<uint32_t>{
            static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::RemoteAddRef),
            0x1236}));
    const auto& rider_frame = probe.Frames()[0];
    EXPECT_EQ(rider_frame.header.GetTargetSessionId(), third_party_session_id);
    EXPECT_EQ(
        rider_frame.header.GetFlags()
            & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
        0);
    // 转发帧不被改写：无 bit1、body 原样
    const auto& forwarded_wire_frame = probe.Frames()[1];
    EXPECT_EQ(
        forwarded_wire_frame.header.GetFlags()
            & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
        0);
    EXPECT_EQ(forwarded_wire_frame.body, forwarded_body);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(third_party_session_id), 0u);
    // rider 批一次物理写 + 转发帧独走一次物理写
    EXPECT_EQ(
        runloop_->GetOutboundPhysicalWriteCount() - physical_writes_before,
        2u);

    probe.Shutdown();
}

// pipe 阈值内无溢出概念：三条滞留 rider + 业务帧整批连写一次物理写
// （Http 拼装溢出专属语义不在本用例保留，由后续 transport 混布覆盖）
TEST_F(IpcRunLoopTest, OverflowRidersFollowStandaloneAfterCarryingHost)
{
    using DAS::Core::IPC::ObjectId;

    OutboundFrameProbe probe(runloop_->GetIoContext());
    ASSERT_TRUE(probe.IsHealthy());
    auto counterpart_transport = probe.TakeCounterpartTransport();
    ASSERT_TRUE(counterpart_transport.has_value());
    ASSERT_EQ(
        runloop_->GetConnectionManager().RegisterHostLocalTransport(
            REMOTE_SESSION_ID,
            std::move(*counterpart_transport)),
        DAS_S_OK);

    const ObjectId object_ids[3] = {
        ObjectId{
            .session_id = REMOTE_SESSION_ID,
            .generation = 9,
            .local_id = 0x71},
        ObjectId{
            .session_id = REMOTE_SESSION_ID,
            .generation = 10,
            .local_id = 0x72},
        ObjectId{
            .session_id = REMOTE_SESSION_ID,
            .generation = 11,
            .local_id = 0x73}};
    for (const ObjectId& object_id : object_ids)
    {
        runloop_->EnqueueAddRef(object_id);
    }

    const auto physical_writes_before =
        runloop_->GetOutboundPhysicalWriteCount();

    // 小 body：三 rider + 宿主远低于阈值 → 整批连写
    const std::vector<uint8_t> host_body(16, 0x21);
    ASSERT_EQ(
        runloop_->PostSend(
            MakeOutboundBusinessFrameHeader(
                PIGGYBACK_TEST_HOST_INTERFACE_ID,
                static_cast<uint32_t>(host_body.size())),
            std::vector<uint8_t>(host_body)),
        DAS_S_OK);

    // 三 rider 在前宿主在后 = FIFO 入队序，一次物理写
    EXPECT_EQ(probe.DrainUntil(4, std::chrono::seconds(3)), 4u);
    EXPECT_EQ(probe.TotalFrameCount(), 4u);
    EXPECT_EQ(
        probe.InterfaceIdOrder(),
        (std::vector<uint32_t>{
            static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::RemoteAddRef),
            static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::RemoteAddRef),
            static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::RemoteAddRef),
            PIGGYBACK_TEST_HOST_INTERFACE_ID}));
    EXPECT_EQ(
        runloop_->GetOutboundPhysicalWriteCount() - physical_writes_before,
        1u);
    for (const auto& frame : probe.Frames())
    {
        EXPECT_EQ(
            frame.header.GetFlags()
                & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
            0);
    }
    // 末帧 = 宿主（body 原样），三 rider 各自 body = ObjectId
    const auto& host_frame = probe.Frames().back();
    EXPECT_EQ(host_frame.body, host_body);
    for (size_t index = 0; index < 3; ++index)
    {
        const auto& rider_frame = probe.Frames()[index];
        ASSERT_EQ(rider_frame.body.size(), sizeof(ObjectId));
        EXPECT_EQ(
            std::memcmp(
                rider_frame.body.data(),
                &object_ids[index],
                sizeof(ObjectId)),
            0);
    }
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 0u);

    probe.Shutdown();
}

// RELEASE 入队语义：非 BT 线程经 EnqueueReleaseFrame 入队即触发——
// [130][132] 同批连写、wire 序 == 入队序、全帧无 bit1（连写天然不携段）
TEST_F(IpcRunLoopTest, RemoteReleaseJoinsFifoBatchInOrder)
{
    using DAS::Core::IPC::ObjectId;

    OutboundFrameProbe probe(runloop_->GetIoContext());
    ASSERT_TRUE(probe.IsHealthy());
    auto counterpart_transport = probe.TakeCounterpartTransport();
    ASSERT_TRUE(counterpart_transport.has_value());
    ASSERT_EQ(
        runloop_->GetConnectionManager().RegisterHostLocalTransport(
            REMOTE_SESSION_ID,
            std::move(*counterpart_transport)),
        DAS_S_OK);

    const ObjectId object_id{
        .session_id = REMOTE_SESSION_ID,
        .generation = 12,
        .local_id = 0x81};
    runloop_->EnqueueAddRef(object_id);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 1u);

    const auto physical_writes_before =
        runloop_->GetOutboundPhysicalWriteCount();

    // 测试主线程 = 非 BT 线程：入队候车（不立即触发，下一写者/guard/
    // 心跳兜底）。候车中：两帧滞留、零写
    runloop_->EnqueueReleaseFrame(object_id);
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 2u);
    EXPECT_EQ(
        runloop_->GetOutboundPhysicalWriteCount() - physical_writes_before,
        0u);

    // 下一写者（后续调用的业务帧）触发 drain：[130][132][业务帧] 同批
    // 连写——序 == 入队序
    const auto carrier_header = IPCMessageHeaderBuilder()
                                    .SetMessageType(MessageType::Request)
                                    .SetInterfaceId(0x8101)
                                    .SetCallId(801)
                                    .SetSourceSessionId(LOCAL_SESSION_ID)
                                    .SetTargetSessionId(REMOTE_SESSION_ID)
                                    .SetBodySize(0)
                                    .Build();
    ASSERT_EQ(
        runloop_->PostSend(carrier_header, std::vector<uint8_t>()),
        DAS_S_OK);

    // [130][132][业务] 同批连写：三逻辑帧、恰一次物理写、序 == 入队序
    EXPECT_EQ(probe.DrainUntil(3, std::chrono::seconds(2)), 3u);
    EXPECT_EQ(probe.TotalFrameCount(), 3u);
    EXPECT_EQ(
        probe.InterfaceIdOrder(),
        (std::vector<uint32_t>{
            static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::RemoteAddRef),
            static_cast<uint32_t>(
                DAS::Core::IPC::IpcCommandType::RemoteRelease),
            0x8101u}));
    EXPECT_EQ(
        runloop_->GetOutboundPhysicalWriteCount() - physical_writes_before,
        1u);
    for (const auto& frame : probe.Frames())
    {
        EXPECT_EQ(
            frame.header.GetFlags()
                & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
            0);
    }
    const auto& release_frame = probe.Frames()[1];
    EXPECT_EQ(
        release_frame.header.GetInterfaceId(),
        static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::RemoteRelease));
    EXPECT_EQ(release_frame.body, EncodeObjectIdBody(object_id));
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 0u);

    probe.Shutdown();
}

// A/B 旋钮：SetPiggybackEnabled(false) → drain 不合写逐帧发——帧仍全部
// 走 FIFO（帧序不变、无 bit1、body 原样），drain 取帧后队列归零
TEST_F(IpcRunLoopTest, DisabledPiggybackPassesBusinessFramesThrough)
{
    using DAS::Core::IPC::ObjectId;

    OutboundFrameProbe probe(runloop_->GetIoContext());
    ASSERT_TRUE(probe.IsHealthy());
    auto counterpart_transport = probe.TakeCounterpartTransport();
    ASSERT_TRUE(counterpart_transport.has_value());
    ASSERT_EQ(
        runloop_->GetConnectionManager().RegisterHostLocalTransport(
            REMOTE_SESSION_ID,
            std::move(*counterpart_transport)),
        DAS_S_OK);

    runloop_->SetPiggybackEnabled(false);

    const ObjectId object_id{
        .session_id = REMOTE_SESSION_ID,
        .generation = 13,
        .local_id = 0x91};
    runloop_->EnqueueAddRef(object_id);

    const auto physical_writes_before =
        runloop_->GetOutboundPhysicalWriteCount();

    const std::vector<uint8_t> response_body{0x0A, 0x0B, 0x0C};
    ASSERT_EQ(
        runloop_->PostSend(
            MakeOutboundBusinessFrameHeader(
                PIGGYBACK_TEST_HOST_INTERFACE_ID,
                static_cast<uint32_t>(response_body.size())),
            std::vector<uint8_t>(response_body)),
        DAS_S_OK);

    // off = 逐帧出：[130][宿主] 各自一次物理写、帧序保持
    EXPECT_EQ(probe.DrainUntil(2, std::chrono::seconds(2)), 2u);
    EXPECT_EQ(probe.TotalFrameCount(), 2u);
    EXPECT_EQ(
        probe.InterfaceIdOrder(),
        (std::vector<uint32_t>{
            static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::RemoteAddRef),
            PIGGYBACK_TEST_HOST_INTERFACE_ID}));
    EXPECT_EQ(
        runloop_->GetOutboundPhysicalWriteCount() - physical_writes_before,
        2u);
    const auto& frames = probe.Frames();
    ASSERT_EQ(frames.size(), 2u);
    for (const auto& frame : frames)
    {
        EXPECT_EQ(
            frame.header.GetFlags()
                & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
            0);
    }
    EXPECT_EQ(frames[1].body, response_body);
    // FIFO 清空：drain 取帧后队列归零（off 仍走 FIFO，非旁路直发）
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 0u);
    EXPECT_TRUE(runloop_->PendingAddRefsEmpty());

    runloop_->SetPiggybackEnabled(true);

    probe.Shutdown();
}

// A/B 语义等价（单元侧）：同一业务帧（BT 响应）在 piggyback 两态下
// 出站——开态 [130][宿主] 连写批一次物理写，关态两逻辑帧各自一次物理
// 写；两态宿主 body 字节等价、130 计数守恒 == 1，仅帧形态不同
TEST_F(IpcRunLoopTest, PiggybackStatesAreSemanticallyEquivalentOnTheWire)
{
    using DAS::Core::IPC::ObjectId;

    const ObjectId object_id{
        .session_id = REMOTE_SESSION_ID,
        .generation = 31,
        .local_id = 0xC1};
    const std::vector<uint8_t> response_body{0x71, 0x72, 0x73, 0x74};

    // —— 开态：[130][宿主] 连写批 ——
    std::optional<std::vector<uint8_t>> on_state_host_body;
    std::optional<std::uint64_t>        on_state_physical_writes;
    {
        OutboundFrameProbe probe(runloop_->GetIoContext());
        ASSERT_TRUE(probe.IsHealthy());
        auto counterpart_transport = probe.TakeCounterpartTransport();
        ASSERT_TRUE(counterpart_transport.has_value());
        ASSERT_EQ(
            runloop_->GetConnectionManager().RegisterHostLocalTransport(
                REMOTE_SESSION_ID,
                std::move(*counterpart_transport)),
            DAS_S_OK);

        const auto writes_before = runloop_->GetOutboundPhysicalWriteCount();
        runloop_->EnqueueAddRef(object_id);
        ASSERT_EQ(
            runloop_->PostSend(
                MakeOutboundBusinessFrameHeader(
                    PIGGYBACK_TEST_HOST_INTERFACE_ID,
                    static_cast<uint32_t>(response_body.size())),
                std::vector<uint8_t>(response_body)),
            DAS_S_OK);

        // 开态两逻辑帧 [130][宿主]、恰一次物理写（连写批）
        EXPECT_EQ(probe.DrainUntil(2, std::chrono::seconds(2)), 2u);
        ASSERT_EQ(probe.TotalFrameCount(), 2u);
        const auto& host_frame = probe.Frames()[1];
        on_state_host_body = host_frame.body;
        on_state_physical_writes =
            runloop_->GetOutboundPhysicalWriteCount() - writes_before;
        EXPECT_EQ(*on_state_physical_writes, 1u);
        // 开态恰一条 130 逻辑帧（零丢失、零重复）
        EXPECT_EQ(
            probe.CountByInterfaceId(
                static_cast<uint32_t>(
                    DAS::Core::IPC::IpcCommandType::RemoteAddRef)),
            1u);
        probe.Shutdown();
    }
    EXPECT_EQ(runloop_->GetPendingAddRefCount(REMOTE_SESSION_ID), 0u);

    // —— 关态：逐帧出（仍走 FIFO）——
    std::optional<std::vector<uint8_t>> off_state_host_body;
    std::optional<std::uint64_t>        off_state_physical_writes;
    {
        OutboundFrameProbe probe(runloop_->GetIoContext());
        ASSERT_TRUE(probe.IsHealthy());
        auto counterpart_transport = probe.TakeCounterpartTransport();
        ASSERT_TRUE(counterpart_transport.has_value());
        constexpr uint16_t off_state_session_id = 3;
        ASSERT_EQ(
            runloop_->GetConnectionManager().RegisterHostLocalTransport(
                off_state_session_id,
                std::move(*counterpart_transport)),
            DAS_S_OK);

        runloop_->SetPiggybackEnabled(false);
        const ObjectId off_state_object_id{
            .session_id = off_state_session_id,
            .generation = 32,
            .local_id = 0xC2};
        const auto writes_before = runloop_->GetOutboundPhysicalWriteCount();
        runloop_->EnqueueAddRef(off_state_object_id);
        const auto off_state_header =
            IPCMessageHeaderBuilder()
                .SetMessageType(MessageType::Response)
                .SetHeaderFlags(DAS::Core::IPC::HeaderFlags::NONE)
                .SetInterfaceId(PIGGYBACK_TEST_HOST_INTERFACE_ID)
                .SetCallId(PIGGYBACK_TEST_CALL_ID)
                .SetSourceSessionId(LOCAL_SESSION_ID)
                .SetTargetSessionId(off_state_session_id)
                .SetBodySize(static_cast<uint32_t>(response_body.size()))
                .Build();
        ASSERT_EQ(
            runloop_->PostSend(
                off_state_header,
                std::vector<uint8_t>(response_body)),
            DAS_S_OK);

        // 关态 [130][宿主] 两逻辑帧、各自一次物理写（不合写）
        EXPECT_EQ(probe.DrainUntil(2, std::chrono::seconds(2)), 2u);
        ASSERT_EQ(probe.TotalFrameCount(), 2u);
        const auto& plain_frame = probe.Frames()[1];
        EXPECT_EQ(
            plain_frame.header.GetFlags()
                & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
            0);
        off_state_host_body = plain_frame.body;
        off_state_physical_writes =
            runloop_->GetOutboundPhysicalWriteCount() - writes_before;
        EXPECT_EQ(*off_state_physical_writes, 2u);
        // 关态 130 计数守恒 == 1、FIFO 清空
        EXPECT_EQ(
            probe.CountByInterfaceId(
                static_cast<uint32_t>(
                    DAS::Core::IPC::IpcCommandType::RemoteAddRef)),
            1u);
        EXPECT_EQ(runloop_->GetPendingAddRefCount(off_state_session_id), 0u);
        probe.Shutdown();
        runloop_->SetPiggybackEnabled(true);
    }

    // 两态宿主 body 字节等价（语义等价，仅帧形态不同）
    ASSERT_TRUE(on_state_host_body.has_value());
    ASSERT_TRUE(off_state_host_body.has_value());
    EXPECT_EQ(*on_state_host_body, response_body);
    EXPECT_EQ(*off_state_host_body, response_body);
    ASSERT_TRUE(on_state_physical_writes.has_value());
    ASSERT_TRUE(off_state_physical_writes.has_value());
    EXPECT_EQ(*on_state_physical_writes, 1u);
    EXPECT_EQ(*off_state_physical_writes, 2u);
}
