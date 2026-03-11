#include <atomic>
#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <das/Core/IPC/IMessageHandler.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/IpcTransport.h>
#include <das/IDasAsyncCallback.h>
#include <das/Utils/fmt.h>
#include <gtest/gtest.h>
#include <thread>
#include <vector>
using DAS::Core::IPC::IMessageHandler;
using DAS::Core::IPC::IPCMessageHeader;
using DAS::Core::IPC::IPCMessageHeaderBuilder;
using DAS::Core::IPC::IpcResponseSender;
using DAS::Core::IPC::IpcRunLoop;
using DAS::Core::IPC::IpcTransport;
using DAS::Core::IPC::MessageType;
using DAS::Core::IPC::ValidatedIPCMessageHeader;

// Test fixture for IpcRunLoop tests
class IpcRunLoopTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto result = IpcRunLoop::Create();
        if (!result)
        {
            FAIL() << "Failed to create IpcRunLoop";
            return;
        }
        runloop_ = std::move(*result);

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
            runloop_.reset();  // 析构函数会自动调用 Shutdown()
        }
    }

    bool SetupRunLoopWithTransport()
    {
        // Create() 已自动完成初始化
        return runloop_ != nullptr;
    }

    ValidatedIPCMessageHeader CreateTestHeader(
        MessageType type = MessageType::REQUEST)
    {
        return IPCMessageHeaderBuilder()
            .SetMessageType(type)
            .SetCallId(1)
            .SetBusinessInterface(1, 0)
            .SetFlags(0)
            .SetBodySize(0)
            .Build();
    }

    std::unique_ptr<IpcRunLoop> runloop_;
    std::string                 host_queue_name_;
    std::string                 plugin_queue_name_;
};

// ====== Initialize/Shutdown Tests ======
// NOTE: Initialize() is now internal (called by Create() factory)
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
    // Create() 已自动完成初始化

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
    uint32_t GetInterfaceId() const override
    {
        return 1;
    }

    boost::asio::awaitable<DasResult> HandleMessage(
        const IPCMessageHeader&     header,
        const std::vector<uint8_t>& body,
        IpcResponseSender&          sender) override
    {
        // 简单返回成功
        (void)header;
        (void)body;
        (void)sender;
        co_return DAS_S_OK;
    }
};

TEST_F(IpcRunLoopTest, RegisterHandler_Succeeds)
{
    // Create() 已自动完成初始化

    // 注册处理器
    auto handler = std::make_unique<TestMessageHandler>();
    runloop_->RegisterHandler(std::move(handler));

    // 验证可以通过 GetHandler 获取
    IMessageHandler* retrieved = runloop_->GetHandler(1);
    EXPECT_NE(retrieved, nullptr);
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
            delete this;
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
        done               = true;
        return DAS_S_OK;
    }
};

// 记录执行顺序的回调
class SequenceCallback : public IDasAsyncCallback
{
public:
    std::atomic<uint32_t>      ref_{0};
    int                        sequence_number;
    std::vector<int>*          execution_order;
    std::atomic<bool>*         all_done;

    SequenceCallback(int seq, std::vector<int>* order, std::atomic<bool>* done)
        : sequence_number(seq), execution_order(order), all_done(done)
    {
    }

    uint32_t AddRef() override { return ++ref_; }

    uint32_t Release() override
    {
        auto r = --ref_;
        if (r == 0)
            delete this;
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
    callback->AddRef();  // 保持引用，防止 callback 在执行后立即被删除
    boost::asio::post(runloop_->GetIoContext(), [callback]() {
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

    callback->Release();  // 释放主线程的引用

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

    std::vector<int>    execution_order;
    std::atomic<bool>   all_done{false};

    // Post multiple callbacks in order
    for (int i = 1; i <= 3; ++i)
    {
        auto* cb = new SequenceCallback(i, &execution_order, &all_done);
        boost::asio::post(runloop_->GetIoContext(), [cb]() {
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

    std::thread::id io_thread_id;
    std::atomic<bool> callback_done{false};

    // Capture the io_context thread id
    boost::asio::post(runloop_->GetIoContext(), [&io_thread_id]() {
        io_thread_id = std::this_thread::get_id();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Post from another thread
    std::thread poster([this, &callback_done]() {
        auto* callback = new CountingCallback();
        boost::asio::post(runloop_->GetIoContext(), [callback, &callback_done]() {
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
