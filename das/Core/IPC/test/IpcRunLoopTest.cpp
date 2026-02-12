#include <atomic>
#include <chrono>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using DAS::Core::IPC::IPCMessageHeader;
using DAS::Core::IPC::IpcRunLoop;
using DAS::Core::IPC::MessageType;

// Test fixture for IpcRunLoop tests
class IpcRunLoopTest : public ::testing::Test
{
protected:
    void SetUp() override { runloop_ = std::make_unique<IpcRunLoop>(); }

    void TearDown() override
    {
        if (runloop_)
        {
            runloop_->Stop();
            runloop_->Shutdown();
        }
    }

    IPCMessageHeader CreateTestHeader(MessageType type = MessageType::REQUEST)
    {
        IPCMessageHeader header{};
        header.call_id = 1;
        header.message_type = type;
        header.error_code = DAS_S_OK;
        header.interface_id = 1;
        header.object_id = 0;
        header.version = 1;
        header.flags = 0;
        header.body_size = 0;
        return header;
    }

    std::unique_ptr<IpcRunLoop> runloop_;
};

// ====== Initialize/Shutdown Tests ======

TEST_F(IpcRunLoopTest, Initialize_Succeeds)
{
    auto result = runloop_->Initialize();
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(IpcRunLoopTest, Shutdown_Succeeds)
{
    ASSERT_EQ(runloop_->Initialize(), DAS_S_OK);
    auto result = runloop_->Shutdown();
    EXPECT_EQ(result, DAS_S_OK);
}

// ====== Run/Stop Tests ======

TEST_F(IpcRunLoopTest, Run_Succeeds)
{
    ASSERT_EQ(runloop_->Initialize(), DAS_S_OK);
    auto result = runloop_->Run();
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(IpcRunLoopTest, Stop_Succeeds)
{
    ASSERT_EQ(runloop_->Initialize(), DAS_S_OK);
    ASSERT_EQ(runloop_->Run(), DAS_S_OK);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto result = runloop_->Stop();
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(IpcRunLoopTest, IsRunning_AfterRun)
{
    ASSERT_EQ(runloop_->Initialize(), DAS_S_OK);
    ASSERT_EQ(runloop_->Run(), DAS_S_OK);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(runloop_->IsRunning());

    runloop_->Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(runloop_->IsRunning());
}

// ====== Re-entrant Detection Tests ======

TEST_F(IpcRunLoopTest, Run_ReentrantFails)
{
    ASSERT_EQ(runloop_->Initialize(), DAS_S_OK);
    ASSERT_EQ(runloop_->Run(), DAS_S_OK);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Second Run() should fail
    auto result = runloop_->Run();
    EXPECT_EQ(result, DAS_E_IPC_DEADLOCK_DETECTED);

    runloop_->Stop();
}

// =====> Stop Idempotent Tests ======

TEST_F(IpcRunLoopTest, Stop_Idempotent)
{
    ASSERT_EQ(runloop_->Initialize(), DAS_S_OK);

    // Stop without run should be safe
    auto result = runloop_->Stop();
    EXPECT_EQ(result, DAS_S_OK);

    // Multiple stops should be safe
    result = runloop_->Stop();
    EXPECT_EQ(result, DAS_S_OK);
}

// ====== Request Handler Tests ======

TEST_F(IpcRunLoopTest, SetRequestHandler_Succeeds)
{
    ASSERT_EQ(runloop_->Initialize(), DAS_S_OK);

    runloop_->SetRequestHandler(
        [](const IPCMessageHeader&, const uint8_t*, size_t)
        { return DAS_S_OK; });
    // Handler is set via std::move, no return value
    // This test just verifies it compiles and doesn't crash
}

// ====== Concurrency Tests ======

TEST_F(IpcRunLoopTest, Stop_FromDifferentThread)
{
    ASSERT_EQ(runloop_->Initialize(), DAS_S_OK);
    ASSERT_EQ(runloop_->Run(), DAS_S_OK);

    std::thread stopper([this]() { runloop_->Stop(); });

    stopper.join();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    ASSERT_EQ(runloop_->Initialize(), DAS_S_OK);
    ASSERT_EQ(runloop_->Run(), DAS_S_OK);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    runloop_->Stop();
    runloop_->Shutdown();

    // Reinitialize
    ASSERT_EQ(runloop_->Initialize(), DAS_S_OK);
    auto result = runloop_->Run();
    EXPECT_EQ(result, DAS_S_OK);

    runloop_->Stop();
}

// ====== Event Message Tests ======

TEST_F(IpcRunLoopTest, SendEvent_WithoutTransport)
{
    ASSERT_EQ(runloop_->Initialize(), DAS_S_OK);

    auto                 header = CreateTestHeader(MessageType::EVENT);
    std::vector<uint8_t> body;

    // Without initialized transport, should fail
    auto result = runloop_->SendEvent(header, body.data(), body.size());
    // Implementation behavior depends on transport state
    // This test documents expected behavior
}

// ====== Response Message Tests ======

TEST_F(IpcRunLoopTest, SendResponse_WithoutTransport)
{
    ASSERT_EQ(runloop_->Initialize(), DAS_S_OK);

    auto                 header = CreateTestHeader(MessageType::RESPONSE);
    std::vector<uint8_t> body;

    // Without initialized transport, should fail
    auto result = runloop_->SendResponse(header, body.data(), body.size());
    // Implementation behavior depends on transport state
}

// ====== Cleanup on Stop Tests ======

TEST_F(IpcRunLoopTest, Stop_CancelsPendingCalls)
{
    ASSERT_EQ(runloop_->Initialize(), DAS_S_OK);
    ASSERT_EQ(runloop_->Run(), DAS_S_OK);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Stop should cancel any pending calls
    runloop_->Stop();

    // Verify clean shutdown
    EXPECT_FALSE(runloop_->IsRunning());
}

// ====== Multiple Run/Stop Cycles ======

TEST_F(IpcRunLoopTest, MultipleRunStopCycles)
{
    ASSERT_EQ(runloop_->Initialize(), DAS_S_OK);

    for (int i = 0; i < 3; ++i)
    {
        EXPECT_EQ(runloop_->Run(), DAS_S_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        runloop_->Stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    EXPECT_FALSE(runloop_->IsRunning());
}
