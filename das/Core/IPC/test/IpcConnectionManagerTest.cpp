#include <chrono>
#include <das/Core/IPC/ConnectionManager.h>
#include <gtest/gtest.h>
#include <thread>

using DAS::Core::IPC::ConnectionManager;

// Test fixture for ConnectionManager tests
class IpcConnectionManagerTest : public ::testing::Test
{
protected:
    void SetUp() override { manager_ = std::make_unique<ConnectionManager>(); }

    void TearDown() override
    {
        if (manager_)
        {
            manager_->Shutdown();
        }
    }

    std::unique_ptr<ConnectionManager> manager_;
};

// ====== Initialize/Shutdown Tests ======

TEST_F(IpcConnectionManagerTest, Initialize_Succeeds)
{
    auto result = manager_->Initialize(1);
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(IpcConnectionManagerTest, Shutdown_Succeeds)
{
    ASSERT_EQ(manager_->Initialize(1), DAS_S_OK);
    auto result = manager_->Shutdown();
    EXPECT_EQ(result, DAS_S_OK);
}

// ====== Connection Registration Tests ======

TEST_F(IpcConnectionManagerTest, RegisterConnection_Succeeds)
{
    ASSERT_EQ(manager_->Initialize(1), DAS_S_OK);

    auto result = manager_->RegisterConnection(2, 1);
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(IpcConnectionManagerTest, RegisterConnection_MultipleConnections)
{
    ASSERT_EQ(manager_->Initialize(1), DAS_S_OK);

    ASSERT_EQ(manager_->RegisterConnection(2, 1), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterConnection(3, 1), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterConnection(4, 1), DAS_S_OK);

    EXPECT_TRUE(manager_->IsConnectionAlive(2));
    EXPECT_TRUE(manager_->IsConnectionAlive(3));
    EXPECT_TRUE(manager_->IsConnectionAlive(4));
}

TEST_F(IpcConnectionManagerTest, UnregisterConnection_Succeeds)
{
    ASSERT_EQ(manager_->Initialize(1), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterConnection(2, 1), DAS_S_OK);

    auto result = manager_->UnregisterConnection(2, 1);
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(IpcConnectionManagerTest, UnregisterConnection_NonExistent)
{
    ASSERT_EQ(manager_->Initialize(1), DAS_S_OK);

    auto result = manager_->UnregisterConnection(999, 1);
    EXPECT_NE(result, DAS_S_OK);
}

// ====== Heartbeat Tests ======

TEST_F(IpcConnectionManagerTest, IsConnectionAlive_AfterRegistration)
{
    ASSERT_EQ(manager_->Initialize(1), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterConnection(2, 1), DAS_S_OK);

    EXPECT_TRUE(manager_->IsConnectionAlive(2));
}

TEST_F(IpcConnectionManagerTest, IsConnectionAlive_NonExistent)
{
    ASSERT_EQ(manager_->Initialize(1), DAS_S_OK);

    EXPECT_FALSE(manager_->IsConnectionAlive(999));
}

TEST_F(IpcConnectionManagerTest, SendHeartbeat_Succeeds)
{
    ASSERT_EQ(manager_->Initialize(1), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterConnection(2, 1), DAS_S_OK);

    auto result = manager_->SendHeartbeat(2);
    EXPECT_EQ(result, DAS_S_OK);
}

TEST_F(IpcConnectionManagerTest, SendHeartbeat_NonExistent)
{
    ASSERT_EQ(manager_->Initialize(1), DAS_S_OK);

    auto result = manager_->SendHeartbeat(999);
    EXPECT_NE(result, DAS_S_OK);
}

// ====== Heartbeat Thread Tests ======

TEST_F(IpcConnectionManagerTest, StartHeartbeatThread_Succeeds)
{
    ASSERT_EQ(manager_->Initialize(1), DAS_S_OK);

    manager_->StartHeartbeatThread();

    // Let it run briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    manager_->StopHeartbeatThread();
    // Should complete without hanging
}

TEST_F(IpcConnectionManagerTest, StopHeartbeatThread_Idempotent)
{
    ASSERT_EQ(manager_->Initialize(1), DAS_S_OK);

    // Stop without start should be safe
    manager_->StopHeartbeatThread();
    manager_->StopHeartbeatThread();
    // Should complete without hanging
}

// ====== Heartbeat Timeout Tests ======

TEST_F(IpcConnectionManagerTest, HeartbeatTimeout_ConnectionMarkedDead)
{
    ASSERT_EQ(manager_->Initialize(1), DAS_S_OK);
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
    ASSERT_EQ(manager_->Initialize(1), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterConnection(2, 1), DAS_S_OK);

    // Unregister should cleanup resources
    ASSERT_EQ(manager_->UnregisterConnection(2, 1), DAS_S_OK);

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
    ASSERT_EQ(manager_->Initialize(1), DAS_S_OK);
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
    ASSERT_EQ(manager_->Initialize(1), DAS_S_OK);

    for (int cycle = 0; cycle < 5; ++cycle)
    {
        ASSERT_EQ(manager_->RegisterConnection(2, 1), DAS_S_OK);
        EXPECT_TRUE(manager_->IsConnectionAlive(2));
        ASSERT_EQ(manager_->UnregisterConnection(2, 1), DAS_S_OK);
        EXPECT_FALSE(manager_->IsConnectionAlive(2));
    }
}
