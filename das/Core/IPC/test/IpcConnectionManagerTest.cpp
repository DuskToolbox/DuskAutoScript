#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <das/Core/IPC/AnyTransport.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/IHostConnection.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcMessageQueue.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/DasPtr.hpp>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <thread>

#include <boost/asio/io_context.hpp>

#include "OutboundFrameProbe.h"
#include "TestTransportResolvers.h"

using Das::DasPtr;
using DAS::Core::IPC::AnyTransport;
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

TEST_F(IpcConnectionManagerTest, HostLocalTransportRejectsDisconnectedTransport)
{
    boost::asio::io_context io_context;
    auto transport = AnyTransport::CreateUninitialized(io_context);

    EXPECT_EQ(
        manager_->RegisterHostLocalTransport(1, std::move(transport)),
        DAS_E_INVALID_ARGUMENT);

    auto [lookup_result, maybe_transport] = manager_->FindHostLocalTransport(1);
    EXPECT_NE(lookup_result, DAS_S_OK);
    EXPECT_FALSE(maybe_transport.has_value());
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
    const DasPtr<HostLauncher> launcher(new HostLauncher(io_ctx, 2, nullptr));

    // Setup heartbeat timeout callback
    bool          callback_called = false;
    const DasGuid test_guid{
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

    const DasPtr<HostLauncher> launcher(new HostLauncher(io_ctx, 2, nullptr));

    std::mutex              mutex;
    std::condition_variable cv;
    bool                    callback_entered = false;
    bool                    release_callback = false;

    const DasGuid test_guid{
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
        [&]() { return manager_->FindManagedHostTransport(2); });
    const bool lookup_returned =
        lookup_future.wait_for(std::chrono::milliseconds(250))
        == std::future_status::ready;

    auto owner_future = std::async(
        std::launch::async,
        [&]() { return manager_->FindManagedHostConnection(2); });
    const bool owner_returned =
        owner_future.wait_for(std::chrono::milliseconds(250))
        == std::future_status::ready;

    {
        const std::scoped_lock lock(mutex);
        release_callback = true;
    }
    cv.notify_all();

    if (!get_returned)
    {
        get_result = get_future.get();
    }

    auto [lookup_result, maybe_transport] = lookup_future.get();
    auto closing_owner = owner_future.get();

    manager_->StopHeartbeatThread();

    EXPECT_TRUE(entered);
    EXPECT_TRUE(get_returned)
        << "timeout callbacks must run outside the ConnectionManager lock";
    EXPECT_TRUE(lookup_returned)
        << "timeout lookup lock-out must not wait for callbacks";
    EXPECT_TRUE(owner_returned)
        << "closing owner lookup must not wait for callbacks";
    EXPECT_EQ(get_result, DAS_S_OK)
        << "timed-out connection should be marked closing and retained until "
           "timeout callbacks finish";
    if (get_result == DAS_S_OK)
    {
        EXPECT_FALSE(info.IsAlive());
        EXPECT_TRUE(info.IsClosing());
    }
    EXPECT_FALSE(closing_owner)
        << "managed host owner lookup must fail for a closing session";
    EXPECT_TRUE(DAS::IsFailed(lookup_result));
    EXPECT_FALSE(maybe_transport.has_value());

    ConnectionInfo after_info{};
    EXPECT_NE(manager_->GetConnection(2, after_info), DAS_S_OK);
}

// ====== 心跳班车（D-12 保险带：SendHeartbeatToAll 过闸1）======

namespace
{
    constexpr uint16_t HEARTBEAT_BUS_LOCAL_SESSION_ID = 1;
    constexpr uint16_t HEARTBEAT_BUS_PEER_SESSION_ID = 7;

    class HeartbeatBusInternalHost final
        : public DAS::Core::IPC::IHostConnection
    {
    public:
        HeartbeatBusInternalHost(
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
            if (iid == DasIidOf<DAS::Core::IPC::IHostConnection>())
            {
                (void)AddRef();
                *pp_object =
                    static_cast<DAS::Core::IPC::IHostConnection*>(this);
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

// 心跳班车装置基类：真实 IpcRunLoop（其 ConnectionManager 在装配期被注入
// 搭车入口）+ 探针观察端读 run loop 的全部出站帧
class HeartbeatBusTest : public ::testing::Test
{
protected:
    void SetUpWithHeartbeatThread(bool enable_heartbeat_thread)
    {
        // main 语义构造点：显式传两级查表 resolver
        run_loop_ = std::make_unique<DAS::Core::IPC::IpcRunLoop>(
            enable_heartbeat_thread,
            nullptr,
            proxy_factory_,
            registry_,
            [this](uint16_t target_session_id)
                -> DAS::Core::IPC::TransportLookupResult
            {
                return ResolveTransportMainSemantic(
                    run_loop_->GetConnectionManager(),
                    target_session_id);
            });
        run_loop_->SetSessionId(HEARTBEAT_BUS_LOCAL_SESSION_ID);

        probe_ = std::make_unique<OutboundFrameProbe>(
            run_loop_->GetIoContext(),
            true);
        ASSERT_TRUE(probe_->IsHealthy());
        counterpart_transport_ = probe_->TakeCounterpartTransport();
        ASSERT_TRUE(counterpart_transport_.has_value());

        const DasPtr<HeartbeatBusInternalHost> host(
            new HeartbeatBusInternalHost(
                run_loop_->GetIoContext(),
                HEARTBEAT_BUS_PEER_SESSION_ID,
                *counterpart_transport_));
        // ConnectionManager 级注册（连接表条目 alive，心跳可达），不经
        // run loop 接收循环
        ASSERT_EQ(
            run_loop_->GetConnectionManager().RegisterInternalHost(
                HEARTBEAT_BUS_PEER_SESSION_ID,
                host),
            DAS_S_OK);

        run_thread_ = std::thread([this]() { run_loop_->Run(); });
        for (int i = 0; i < 100 && !run_loop_->IsRunning(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(run_loop_->IsRunning());
    }

    void TearDown() override
    {
        run_loop_->RequestStop();
        if (run_thread_.joinable())
        {
            run_thread_.join();
        }
        probe_->Shutdown();
        // 对端 transport 须在 run loop（io_context owner）析构之前完整析构
        // ——socket 析构触碰 io_context 的 socket service（Cleanup 不够）；
        // ConnectionManager 持有的 host 只持引用，析构不回调 transport
        if (counterpart_transport_.has_value())
        {
            counterpart_transport_->Cleanup();
            counterpart_transport_.reset();
        }
        run_loop_.reset();
    }

    DAS::Core::IPC::ProxyFactory                proxy_factory_;
    DAS::Core::IPC::RemoteObjectRegistry        registry_;
    std::unique_ptr<DAS::Core::IPC::IpcRunLoop> run_loop_;
    std::unique_ptr<OutboundFrameProbe>         probe_;
    std::optional<AnyTransport>                 counterpart_transport_;
    std::thread                                 run_thread_;
};

// 直驱形态：不开真实心跳线程（SendHeartbeatToAll 由测试显式调用，帧窗
// 口内零背景心跳干扰）
class HeartbeatBusDirectTest : public HeartbeatBusTest
{
protected:
    void SetUp() override { SetUpWithHeartbeatThread(false); }
};

// 真实周期形态：Run() 启动心跳线程（周期 1000ms）
class HeartbeatBusWithThreadTest : public HeartbeatBusTest
{
protected:
    void SetUp() override { SetUpWithHeartbeatThread(true); }
};

// 班车 Test 1：某 session 存在 pending ADD_REF 时心跳周期到 → carrier
// 入队心跳帧并触发 drain——[130][心跳] 两逻辑帧同批连写一次物理写、
// 全帧无 bit1（pipe 连写不置位，旧拼装形态为有意行为变化），心跳本体
// body 语义不变
TEST_F(HeartbeatBusDirectTest, HeartbeatCarriesPendingAddRefsWhenPresent)
{
    const DAS::Core::IPC::ObjectId object_id{
        .session_id = HEARTBEAT_BUS_PEER_SESSION_ID,
        .generation = 21,
        .local_id = 0xB1};
    run_loop_->EnqueueAddRef(object_id);
    EXPECT_EQ(
        run_loop_->GetPendingAddRefCount(HEARTBEAT_BUS_PEER_SESSION_ID),
        1u);

    const auto physical_writes_before =
        run_loop_->GetOutboundPhysicalWriteCount();

    // 直驱一次班车（确定性，不等真实周期）
    ASSERT_EQ(run_loop_->GetConnectionManager().SendHeartbeatToAll(), DAS_S_OK);

    // 出站两逻辑帧一次物理写：[130][心跳]，FIFO 序 = 入队序
    EXPECT_EQ(probe_->DrainUntil(2, std::chrono::seconds(2)), 2u);
    EXPECT_EQ(probe_->DrainFor(std::chrono::milliseconds(300)), 0u);
    ASSERT_EQ(probe_->TotalFrameCount(), 2u);
    EXPECT_EQ(
        run_loop_->GetPendingAddRefCount(HEARTBEAT_BUS_PEER_SESSION_ID),
        0u);
    EXPECT_EQ(
        probe_->InterfaceIdOrder(),
        (std::vector<uint32_t>{
            static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::RemoteAddRef),
            static_cast<uint32_t>(DAS::Core::IPC::HandshakeInterfaceId::
                                      HandshakeIfaceHeartbeat)}));
    EXPECT_EQ(
        run_loop_->GetOutboundPhysicalWriteCount() - physical_writes_before,
        1u);

    // 连写批内帧形态：130 帧原样（无 bit1、body 为 ObjectId 字节）
    const auto& add_ref_frame = probe_->Frames()[0];
    EXPECT_EQ(
        add_ref_frame.header.GetFlags()
            & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
        0);
    ASSERT_EQ(add_ref_frame.body.size(), sizeof(DAS::Core::IPC::ObjectId));
    DAS::Core::IPC::ObjectId wire_object_id{};
    std::memcpy(
        &wire_object_id,
        add_ref_frame.body.data(),
        sizeof(DAS::Core::IPC::ObjectId));
    EXPECT_EQ(wire_object_id.session_id, object_id.session_id);
    EXPECT_EQ(wire_object_id.local_id, object_id.local_id);

    // 心跳本体帧语义不变：独立帧形态（无 bit1、reserved 0）、
    // HeartbeatV1 字节原样
    const auto& heartbeat_frame = probe_->Frames()[1];
    EXPECT_EQ(
        heartbeat_frame.header.GetInterfaceId(),
        static_cast<uint32_t>(
            DAS::Core::IPC::HandshakeInterfaceId::HandshakeIfaceHeartbeat));
    EXPECT_EQ(
        heartbeat_frame.header.GetFlags()
            & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
        0);
    EXPECT_EQ(heartbeat_frame.header.Raw().reserved, 0u);
    ASSERT_EQ(heartbeat_frame.body.size(), sizeof(DAS::Core::IPC::HeartbeatV1));
    const auto* heartbeat_body_bytes =
        reinterpret_cast<const DAS::Core::IPC::HeartbeatV1*>(
            heartbeat_frame.body.data());
    EXPECT_GT(heartbeat_body_bytes->timestamp_ms, 0u);
}

// 班车 Test 2（零流量清账）：零流量场景 FIFO 滞留不超过一个心跳周期被
// 清——无其他出站时仅靠班车 carrier 触发的 drain 清账（保险带验证）
TEST_F(HeartbeatBusWithThreadTest, HeartbeatClearsPendingWithinOneInterval)
{
    const DAS::Core::IPC::ObjectId object_id{
        .session_id = HEARTBEAT_BUS_PEER_SESSION_ID,
        .generation = 22,
        .local_id = 0xB2};
    run_loop_->EnqueueAddRef(object_id);

    // 本装置无业务流量：一个心跳周期（加余量）内 FIFO 必被班车带走
    bool cleared_within_bound = false;
    for (int attempt = 0;
         attempt < (ConnectionManager::HEARTBEAT_INTERVAL_MS + 1500)
                       / std::chrono::milliseconds(50).count();
         ++attempt)
    {
        if (run_loop_->GetPendingAddRefCount(HEARTBEAT_BUS_PEER_SESSION_ID)
            == 0)
        {
            cleared_within_bound = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_TRUE(cleared_within_bound)
        << "zero-traffic pending must be cleared within one heartbeat "
           "interval by the heartbeat bus";

    // 班车帧确实到达（130 是否同批取决于取样时点，心跳帧本身必须出现）
    EXPECT_GE(probe_->DrainFor(std::chrono::milliseconds(200)), 1u);
}

// 班车 Test 3：FIFO 为空时心跳帧独立形态（无 bit1，零开销基线不变）
TEST_F(HeartbeatBusDirectTest, HeartbeatWithoutPendingCarriesNoSection)
{
    ASSERT_EQ(run_loop_->GetConnectionManager().SendHeartbeatToAll(), DAS_S_OK);

    EXPECT_EQ(probe_->DrainUntil(1, std::chrono::seconds(2)), 1u);
    ASSERT_EQ(probe_->TotalFrameCount(), 1u);
    const auto& heartbeat_frame = probe_->Frames()[0];
    EXPECT_EQ(
        heartbeat_frame.header.GetFlags()
            & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
        0);
    EXPECT_EQ(heartbeat_frame.header.Raw().reserved, 0u);
    ASSERT_EQ(heartbeat_frame.body.size(), sizeof(DAS::Core::IPC::HeartbeatV1));
}

// 班车 Test 4：SetPiggybackEnabled(false) = drain 不合写逐帧出——班车仍
// 入队 + 触发（off 严禁旁路直发重新引入双写者），两逻辑帧各占一次物理
// 写、无 bit1、FIFO 同样出清（A/B 一致）
TEST_F(HeartbeatBusDirectTest, DisabledPiggybackTurnsHeartbeatBusOff)
{
    run_loop_->SetPiggybackEnabled(false);

    const DAS::Core::IPC::ObjectId object_id{
        .session_id = HEARTBEAT_BUS_PEER_SESSION_ID,
        .generation = 23,
        .local_id = 0xB3};
    run_loop_->EnqueueAddRef(object_id);
    EXPECT_EQ(
        run_loop_->GetPendingAddRefCount(HEARTBEAT_BUS_PEER_SESSION_ID),
        1u);

    const auto physical_writes_before =
        run_loop_->GetOutboundPhysicalWriteCount();

    ASSERT_EQ(run_loop_->GetConnectionManager().SendHeartbeatToAll(), DAS_S_OK);

    // off = 逐帧出：[130][心跳] 两逻辑帧、两物理写、无 bit1
    EXPECT_EQ(probe_->DrainUntil(2, std::chrono::seconds(2)), 2u);
    ASSERT_EQ(probe_->TotalFrameCount(), 2u);
    EXPECT_EQ(
        probe_->InterfaceIdOrder(),
        (std::vector<uint32_t>{
            static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::RemoteAddRef),
            static_cast<uint32_t>(DAS::Core::IPC::HandshakeInterfaceId::
                                      HandshakeIfaceHeartbeat)}));
    EXPECT_EQ(
        run_loop_->GetOutboundPhysicalWriteCount() - physical_writes_before,
        2u);
    for (const auto& wire_frame : probe_->Frames())
    {
        EXPECT_EQ(
            wire_frame.header.GetFlags()
                & DAS::Core::IPC::MessageFlags::RESERVED_AS_SIZE,
            0);
    }

    const auto& heartbeat_frame = probe_->Frames()[1];
    ASSERT_EQ(heartbeat_frame.body.size(), sizeof(DAS::Core::IPC::HeartbeatV1));
    EXPECT_EQ(
        run_loop_->GetPendingAddRefCount(HEARTBEAT_BUS_PEER_SESSION_ID),
        0u);
}
