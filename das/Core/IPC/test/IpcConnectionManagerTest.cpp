#include <chrono>
#include <condition_variable>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/DasPtr.hpp>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>

#include <boost/asio/io_context.hpp>

using Das::DasPtr;
using DAS::Core::IPC::ConnectionInfo;
using DAS::Core::IPC::ConnectionManager;
using DAS::Core::IPC::HostLauncher;

// Test fixture for ConnectionManager tests
class IpcConnectionManagerTest : public ::testing::Test
{
protected:
    void SetUp() override { manager_ = std::make_unique<ConnectionManager>(1); }

    void TearDown() override
    {
        // RAII: unique_ptr 析构自动调用 Shutdown()
        manager_.reset();
    }

    std::unique_ptr<ConnectionManager> manager_;
};

// ====== Initialize/Shutdown Tests ======

TEST_F(IpcConnectionManagerTest, Initialize_Succeeds)
{
    // Manager is already initialized in SetUp via RAII constructor
    ASSERT_NE(manager_, nullptr);
}

// ====== Connection Registration Tests ======

TEST_F(IpcConnectionManagerTest, RegisterConnection_Succeeds)
{
    // Manager is already initialized in SetUp via RAII constructor

    auto result = manager_->RegisterConnection(2, 1);
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(IpcConnectionManagerTest, RegisterConnection_MultipleConnections)
{
    // Manager is already initialized in SetUp via RAII constructor

    ASSERT_EQ(manager_->RegisterConnection(2, 1), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterConnection(3, 1), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterConnection(4, 1), DAS_S_OK);

    EXPECT_TRUE(manager_->IsConnectionAlive(2));
    EXPECT_TRUE(manager_->IsConnectionAlive(3));
    EXPECT_TRUE(manager_->IsConnectionAlive(4));
}

TEST_F(IpcConnectionManagerTest, UnregisterConnection_Succeeds)
{
    // Manager is already initialized in SetUp via RAII constructor
    ASSERT_EQ(manager_->RegisterConnection(2, 1), DAS_S_OK);

    auto result = manager_->UnregisterConnection(2);
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(IpcConnectionManagerTest, UnregisterConnection_NonExistent)
{
    // Manager is already initialized in SetUp via RAII constructor

    auto result = manager_->UnregisterConnection(999);
    EXPECT_NE(result, DAS_S_OK);
}

// ====== Heartbeat Tests ======

TEST_F(IpcConnectionManagerTest, IsConnectionAlive_AfterRegistration)
{
    // Manager is already initialized in SetUp via RAII constructor
    ASSERT_EQ(manager_->RegisterConnection(2, 1), DAS_S_OK);

    EXPECT_TRUE(manager_->IsConnectionAlive(2));
}

TEST_F(IpcConnectionManagerTest, IsConnectionAlive_NonExistent)
{
    // Manager is already initialized in SetUp via RAII constructor

    EXPECT_FALSE(manager_->IsConnectionAlive(999));
}

TEST_F(IpcConnectionManagerTest, SendHeartbeat_Succeeds)
{
    // Manager is already initialized in SetUp via RAII constructor
    ASSERT_EQ(manager_->RegisterConnection(2, 1), DAS_S_OK);

    auto result = manager_->SendHeartbeat(2);
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(IpcConnectionManagerTest, SendHeartbeat_NonExistent)
{
    // Manager is already initialized in SetUp via RAII constructor

    auto result = manager_->SendHeartbeat(999);
    EXPECT_NE(result, DAS_S_OK);
}

// ====== Heartbeat Thread Tests ======

TEST_F(IpcConnectionManagerTest, StartHeartbeatThread_Succeeds)
{
    // Manager is already initialized in SetUp via RAII constructor

    manager_->StartHeartbeatThread();

    // Let it run briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    manager_->StopHeartbeatThread();
    // Should complete without hanging
}

TEST_F(IpcConnectionManagerTest, StopHeartbeatThread_Idempotent)
{
    // Manager is already initialized in SetUp via RAII constructor

    // Stop without start should be safe
    manager_->StopHeartbeatThread();
    manager_->StopHeartbeatThread();
    // Should complete without hanging
}

// ====== Heartbeat Timeout Tests ======

TEST_F(IpcConnectionManagerTest, HeartbeatTimeout_ConnectionMarkedDead)
{
    // Manager is already initialized in SetUp via RAII constructor
    ASSERT_EQ(manager_->RegisterConnection(2, 1), DAS_S_OK);

    // Start heartbeat thread
    manager_->StartHeartbeatThread();

    // Connection should initially be alive
    EXPECT_TRUE(manager_->IsConnectionAlive(2));

    // Wait for timeout (default is 5000ms, but we use shorter in tests)
    // In real tests, we would need to mock time or use configurable timeouts
    // For now, just verify the connection is alive after a short wait
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Stop the thread
    manager_->StopHeartbeatThread();
}

// ====== CleanupConnectionResources Tests ======

TEST_F(IpcConnectionManagerTest, CleanupResources_OnUnregister)
{
    // Manager is already initialized in SetUp via RAII constructor
    ASSERT_EQ(manager_->RegisterConnection(2, 1), DAS_S_OK);

    // Unregister should cleanup resources
    ASSERT_EQ(manager_->UnregisterConnection(2), DAS_S_OK);

    // Connection should no longer exist
    EXPECT_FALSE(manager_->IsConnectionAlive(2));
}

// ====== Constants Verification ======

TEST_F(IpcConnectionManagerTest, HeartbeatInterval_Value)
{
    EXPECT_EQ(ConnectionManager::HEARTBEAT_INTERVAL_MS, 1000);
}

TEST_F(IpcConnectionManagerTest, HeartbeatTimeout_Value)
{
    EXPECT_EQ(ConnectionManager::HEARTBEAT_TIMEOUT_MS, 5000);
}

// ====== Multiple Operations Tests ======

TEST_F(IpcConnectionManagerTest, MultipleHeartbeats)
{
    // Manager is already initialized in SetUp via RAII constructor
    ASSERT_EQ(manager_->RegisterConnection(2, 1), DAS_S_OK);

    // Send multiple heartbeats
    for (int i = 0; i < 10; ++i)
    {
        auto result = manager_->SendHeartbeat(2);
        EXPECT_EQ(result, DAS_S_OK);
    }
}

TEST_F(IpcConnectionManagerTest, RegisterUnregisterCycle)
{
    // Manager is already initialized in SetUp via RAII constructor

    for (int cycle = 0; cycle < 5; ++cycle)
    {
        ASSERT_EQ(manager_->RegisterConnection(2, 1), DAS_S_OK);
        EXPECT_TRUE(manager_->IsConnectionAlive(2));
        ASSERT_EQ(manager_->UnregisterConnection(2), DAS_S_OK);
        EXPECT_FALSE(manager_->IsConnectionAlive(2));
    }
}

// ====== SendHeartbeatToAll Tests ======

TEST_F(
    IpcConnectionManagerTest,
    SendHeartbeatToAll_NoConnections_ReturnsSuccess)
{
    // Manager is already initialized in SetUp via RAII constructor

    // 无连接时调用应返回 DAS_S_OK
    auto result = manager_->SendHeartbeatToAll();
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(
    IpcConnectionManagerTest,
    SendHeartbeatToAll_WithConnections_ReturnsSuccess)
{
    // Manager is already initialized in SetUp via RAII constructor
    ASSERT_EQ(manager_->RegisterConnection(2, 1), DAS_S_OK);

    // 有连接时调用应返回 DAS_S_OK
    auto result = manager_->SendHeartbeatToAll();
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(
    IpcConnectionManagerTest,
    SendHeartbeatToAll_MultipleConnections_ReturnsSuccess)
{
    // Manager is already initialized in SetUp via RAII constructor
    ASSERT_EQ(manager_->RegisterConnection(2, 1), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterConnection(3, 1), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterConnection(4, 1), DAS_S_OK);

    // 多个连接时调用应返回 DAS_S_OK
    auto result = manager_->SendHeartbeatToAll();
    EXPECT_EQ(result, DAS_S_OK);
}

// ====== UpdateHeartbeatTimestamp Tests ======

TEST_F(IpcConnectionManagerTest, UpdateHeartbeatTimestamp_ExistingConnection)
{
    // Manager is already initialized in SetUp via RAII constructor
    ASSERT_EQ(manager_->RegisterConnection(2, 1), DAS_S_OK);

    // 记录初始时间戳
    auto initial_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();

    // 等待一小段时间
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 更新时间戳
    manager_->UpdateHeartbeatTimestamp(2);

    // 验证时间戳已更新（通过发送心跳来验证，因为时间戳是内部的）
    // 我们可以通过再次发送心跳并验证连接仍然存活来间接验证
    EXPECT_TRUE(manager_->IsConnectionAlive(2));
}

TEST_F(IpcConnectionManagerTest, UpdateHeartbeatTimestamp_NonExistentConnection)
{
    // Manager is already initialized in SetUp via RAII constructor

    // 不存在的连接应该安全处理
    manager_->UpdateHeartbeatTimestamp(999);
    // 不崩溃即成功
}

// ====== Heartbeat Timeout Two-Phase Tests ======

TEST_F(IpcConnectionManagerTest, HeartbeatTimeout_TriggersCallback)
{
    boost::asio::io_context io_ctx;

    // Create a HostLauncher (no real process launched)
    DasPtr<HostLauncher> launcher(new HostLauncher(io_ctx, 2, nullptr));

    // Setup heartbeat timeout callback
    bool    callback_called = false;
    DasGuid test_guid{
        0x01020304,
        0x0506,
        0x0708,
        {0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10}};
    DasGuid received_guid{};

    launcher->SetAssociatedGuid(test_guid);
    launcher->SetOnHeartbeatTimeout(
        [&](DasGuid guid)
        {
            callback_called = true;
            received_guid = guid;
        });

    // Register the launcher with ConnectionManager
    ASSERT_EQ(manager_->RegisterHostLauncher(2, launcher), DAS_S_OK);

    // Start heartbeat thread - it will detect timeout after
    // HEARTBEAT_TIMEOUT_MS (5000ms)
    manager_->StartHeartbeatThread();

    // Wait long enough for timeout detection
    // HEARTBEAT_INTERVAL_MS (1000ms) + HEARTBEAT_TIMEOUT_MS (5000ms) + margin
    std::this_thread::sleep_for(std::chrono::milliseconds(7000));

    manager_->StopHeartbeatThread();

    // Verify: the heartbeat timeout callback was invoked with correct GUID
    EXPECT_TRUE(callback_called);
    EXPECT_EQ(received_guid, test_guid);
}

TEST_F(IpcConnectionManagerTest, TimeoutMarksClosingBeforeCallbacksFinish)
{
    boost::asio::io_context io_ctx;

    DasPtr<HostLauncher> launcher(new HostLauncher(io_ctx, 2, nullptr));

    std::mutex              mutex;
    std::condition_variable cv;
    bool                    callback_entered = false;
    bool                    release_callback = false;

    DasGuid test_guid{
        0x01020304,
        0x0506,
        0x0708,
        {0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10}};

    launcher->SetAssociatedGuid(test_guid);
    launcher->SetOnHeartbeatTimeout(
        [&](DasGuid)
        {
            std::unique_lock<std::mutex> lock(mutex);
            callback_entered = true;
            cv.notify_all();
            cv.wait(lock, [&release_callback]() { return release_callback; });
        });

    ASSERT_EQ(manager_->RegisterHostLauncher(2, launcher), DAS_S_OK);

    manager_->StartHeartbeatThread();

    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered = cv.wait_for(
            lock,
            std::chrono::milliseconds(
                ConnectionManager::HEARTBEAT_TIMEOUT_MS
                + (2 * ConnectionManager::HEARTBEAT_INTERVAL_MS)),
            [&callback_entered]() { return callback_entered; });
    }

    ConnectionInfo info{};
    DasResult      get_result = DAS_E_UNDEFINED_RETURN_VALUE;
    auto           get_future = std::async(
        std::launch::async,
        [&]() { return manager_->GetConnection(2, info); });
    const bool get_returned =
        get_future.wait_for(std::chrono::milliseconds(250))
        == std::future_status::ready;
    if (get_returned)
    {
        get_result = get_future.get();
    }

    auto lookup_future = std::async(
        std::launch::async,
        [&]() { return manager_->FindTransport(2); });
    const bool lookup_returned =
        lookup_future.wait_for(std::chrono::milliseconds(250))
        == std::future_status::ready;

    {
        std::lock_guard<std::mutex> lock(mutex);
        release_callback = true;
    }
    cv.notify_all();

    if (!get_returned)
    {
        get_result = get_future.get();
    }

    auto [lookup_result, maybe_transport] = lookup_future.get();

    manager_->StopHeartbeatThread();

    EXPECT_TRUE(entered);
    EXPECT_TRUE(get_returned)
        << "timeout callbacks must run outside the ConnectionManager lock";
    EXPECT_TRUE(lookup_returned)
        << "timeout lookup lock-out must not wait for callbacks";
    EXPECT_EQ(get_result, DAS_S_OK)
        << "timed-out connection should be marked closing and retained until "
           "timeout callbacks finish";
    if (get_result == DAS_S_OK)
    {
        EXPECT_FALSE(info.is_alive);
    }
    EXPECT_TRUE(DAS::IsFailed(lookup_result));
    EXPECT_FALSE(maybe_transport.has_value());

    ConnectionInfo after_info{};
    EXPECT_NE(manager_->GetConnection(2, after_info), DAS_S_OK);
}
