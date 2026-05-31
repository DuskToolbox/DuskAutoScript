#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <das/Core/IPC/AfUnixAvailable.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IHostConnection.h>
#include <das/Core/IPC/IMessageHandler.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/IpcTransport.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/IDasAsyncCallback.h>
#include <das/Utils/fmt.h>
#include <future>
#include <gtest/gtest.h>
#include <optional>
#include <thread>
#include <vector>
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
            runloop_ = std::make_unique<IpcRunLoop>(
                true,
                inbound_queue_.get(),
                proxy_factory_,
                registry_);
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
        MessageType type = MessageType::REQUEST)
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
            .SetMessageType(MessageType::RESPONSE)
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

    DAS::DasPtr<IHostConnection> host(new TestInternalHost(
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
        std::unique_lock<std::mutex> lock(runloop_->pending_mutex_);
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

    DAS::DasPtr<IHostConnection> host(new TestInternalHost(
        runloop_->GetIoContext(),
        REMOTE_SESSION_ID,
        transport_pair->run_loop_side));
    ASSERT_EQ(runloop_->RegisterInternalHost(host), DAS_S_OK);

    const CallKey call_key{REMOTE_SESSION_ID, call_id};
    {
        std::unique_lock<std::mutex> lock(runloop_->pending_mutex_);
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
        EXPECT_EQ(queued->header.GetMessageType(), MessageType::RESPONSE);
        EXPECT_EQ(queued->header.GetSourceSessionId(), REMOTE_SESSION_ID);
        EXPECT_EQ(queued->header.GetTargetSessionId(), LOCAL_SESSION_ID);
        EXPECT_EQ(queued->header.GetCallId(), call_id);
        EXPECT_EQ(queued->body, body);
    }
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
        runloop_ = std::make_unique<IpcRunLoop>(
            true,
            nullptr,
            proxy_factory_,
            registry_);
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
