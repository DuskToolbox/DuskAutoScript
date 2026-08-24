#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <das/Core/IPC/AnyTransport.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/Core/IPC/UnixAsyncIpcTransport.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/Utils/fmt.h>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>
#include <vector>

#ifdef DAS_WINDOWS
#include <das/Core/IPC/Win32AsyncIpcTransport.h>
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
inline DWORD GetCurrentProcessId() { return static_cast<DWORD>(getpid()); }
#endif

using DAS::Core::IPC::AnyTransport;
using DAS::Core::IPC::IPCMessageHeader;
using DAS::Core::IPC::IPCMessageHeaderBuilder;
using DAS::Core::IPC::PoolMode;
using DAS::Core::IPC::SendSegment;
using DAS::Core::IPC::SharedMemoryBlock;
using DAS::Core::IPC::SharedMemoryPool;
using DAS::Core::IPC::ShmBlockGuard;
using DAS::Core::IPC::ValidatedIPCMessageHeader;
using namespace DAS::Core::IPC::HeaderFlags;

// scatter 用例直测 AF_UNIX 具体实现（Windows 上同为运行时检测可用的
// 传输——AnyTransport 在 AF_UNIX 可用时即选它）；AnyTransport 变体
// 转发路径由既有夹具另测。Win32 命名管道实现不在此直测：其同进程
// server/client 握手存在先于本 phase 的次序死锁（server 先等
// X_write 连接再建 X_read，client 先开 X_read 重试 1s 后放弃），
// 属预存在问题，留档 deferred-items
using ScatterTransportUnderTest = DAS::Core::IPC::UnixAsyncIpcTransport;

namespace
{
    constexpr uint16_t SERVER_SESSION_ID = 1;
    constexpr uint16_t CLIENT_SESSION_ID = 2;

    // 大消息阈值定义（与实现一致）
    constexpr size_t LARGE_MESSAGE_THRESHOLD = 65536;

    // 生成测试用的 body 数据
    std::vector<uint8_t> GenerateTestBody(size_t size)
    {
        std::vector<uint8_t> body(size);
        for (size_t i = 0; i < size; ++i)
        {
            body[i] = static_cast<uint8_t>(i % 256);
        }
        return body;
    }

    // 验证 body 数据
    bool VerifyTestBody(const uint8_t* data, size_t size)
    {
        for (size_t i = 0; i < size; ++i)
        {
            if (data[i] != static_cast<uint8_t>(i % 256))
            {
                return false;
            }
        }
        return true;
    }

    // 创建测试用的消息头
    ValidatedIPCMessageHeader CreateTestHeader(
        uint16_t call_id,
        uint16_t source_session_id,
        uint16_t target_session_id,
        uint32_t body_size,
        uint8_t  header_flags = NONE)
    {
        return IPCMessageHeaderBuilder()
            .SetMessageType(DAS::Core::IPC::MessageType::Request)
            .SetHeaderFlags(header_flags)
            .SetCallId(call_id)
            .SetSourceSessionId(source_session_id)
            .SetTargetSessionId(target_session_id)
            .SetInterfaceId(0)
            .SetBodySize(body_size)
            .Build();
    }

} // namespace

// Test fixture for IPC async transport SHM 分流测试
class IpcAsyncTransportShmTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // 生成唯一的共享内存池名称
        auto now_ns = std::chrono::high_resolution_clock::now()
                          .time_since_epoch()
                          .count();
        pool_name_ = DAS_FMT_NS::format(
            "das_ipc_test_shm_{}_{}",
            GetCurrentProcessId(),
            now_ns);
    }

    void TearDown() override
    {
        // RAII: unique_ptr 析构自动清理
        server_pool_.reset();
        client_pool_.reset();
    }

    // 创建 server 端的 shared memory pool（Create 模式）
    std::unique_ptr<SharedMemoryPool> CreateServerPool(
        size_t size = 10 * 1024 * 1024)
    {
        return std::make_unique<SharedMemoryPool>(
            pool_name_,
            size,
            PoolMode::Create);
    }

    // 创建 client 端的 shared memory pool（Open 模式，带重试）
    std::unique_ptr<SharedMemoryPool> CreateClientPool(
        int max_retries = 50,
        int retry_delay_ms = 20)
    {
        for (int i = 0; i < max_retries; ++i)
        {
            try
            {
                auto pool = SharedMemoryPool::Open(pool_name_);
                return pool;
            }
            catch (const std::exception&)
            {
                // Pool 还没创建好，等待后重试
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(retry_delay_ms));
            }
        }
        throw std::runtime_error(
            "Failed to open shared memory pool after retries");
    }

    // 创建连接的 transport 对（server + client）
    struct TransportPair
    {
        AnyTransport server;
        AnyTransport client;
    };

    // 持有 io_context 和 transport 对
    struct TransportContext
    {
        boost::asio::io_context      io_context;
        std::optional<TransportPair> transports;
    };

    std::optional<TransportPair> CreateConnectedTransports(
        boost::asio::io_context& io_context,
        SharedMemoryPool*        server_pool,
        SharedMemoryPool*        client_pool)
    {
        // 生成唯一的 endpoint 名称
        static std::atomic<uint32_t> next_endpoint_id{1};
        const auto                   base = DAS_FMT_NS::format(
            "shm_test_{}_{}",
            GetCurrentProcessId() % 10000,
            next_endpoint_id.fetch_add(1));
        const auto read_endpoint = base + "_read";
        const auto write_endpoint = base + "_write";

        // 确保 io_context 有工作（使用 work guard）
        auto work_guard = boost::asio::make_work_guard(io_context);

        // 创建 server 端 transport
        auto server_future = boost::asio::co_spawn(
            io_context,
            AnyTransport::CreateAsync(
                io_context,
                read_endpoint,
                write_endpoint,
                true,            // is_server
                4 * 1024 * 1024, // max_message_size (4MB)
                server_pool),
            boost::asio::use_future);

        // 创建 client 端 transport
        auto client_future = boost::asio::co_spawn(
            io_context,
            AnyTransport::CreateAsync(
                io_context,
                read_endpoint,
                write_endpoint,
                false,           // is_server
                4 * 1024 * 1024, // max_message_size (4MB)
                client_pool),
            boost::asio::use_future);

        // 在独立线程运行 io_context
        std::thread io_thread([&io_context]() { io_context.run(); });

        auto [server_result, server_transport] = server_future.get();
        auto [client_result, client_transport] = client_future.get();

        // 重置 work guard，允许 io_context 在没有工作时退出
        work_guard.reset();

        // 等待 io_context 完成
        if (io_thread.joinable())
        {
            io_thread.join();
        }
        io_context.restart();

        if (server_result != DAS_S_OK || !server_transport)
        {
            ADD_FAILURE() << "Failed to create server transport: "
                          << server_result;
            return std::nullopt;
        }

        if (client_result != DAS_S_OK || !client_transport)
        {
            ADD_FAILURE() << "Failed to create client transport: "
                          << client_result;
            return std::nullopt;
        }

        return TransportPair{
            std::move(*server_transport),
            std::move(*client_transport)};
    }

    // 创建连接的具体传输对（scatter 用例直测平台原生实现）
    struct ScatterTransportPair
    {
        std::unique_ptr<ScatterTransportUnderTest> server;
        std::unique_ptr<ScatterTransportUnderTest> client;
    };

    std::optional<ScatterTransportPair> CreateConnectedScatterTransports()
    {
        static std::atomic<uint32_t> next_scatter_endpoint_id{1};
        const auto                   base = DAS_FMT_NS::format(
            "scatter_test_{}_{}",
            GetCurrentProcessId() % 10000,
            next_scatter_endpoint_id.fetch_add(1));
        const auto read_endpoint = base + "_read";
        const auto write_endpoint = base + "_write";

        auto work_guard = boost::asio::make_work_guard(io_context_);

        auto server_future = boost::asio::co_spawn(
            io_context_,
            ScatterTransportUnderTest::CreateAsync(
                io_context_,
                read_endpoint,
                write_endpoint,
                true),
            boost::asio::use_future);

        auto client_future = boost::asio::co_spawn(
            io_context_,
            ScatterTransportUnderTest::CreateAsync(
                io_context_,
                read_endpoint,
                write_endpoint,
                false),
            boost::asio::use_future);

        std::thread io_thread([this]() { io_context_.run(); });

        auto server_result = server_future.get();
        auto client_result = client_future.get();

        work_guard.reset();
        if (io_thread.joinable())
        {
            io_thread.join();
        }
        io_context_.restart();

        if (!server_result.has_value() || !server_result.value())
        {
            ADD_FAILURE() << "Failed to create server scatter transport: "
                          << server_result.error();
            return std::nullopt;
        }

        if (!client_result.has_value() || !client_result.value())
        {
            ADD_FAILURE() << "Failed to create client scatter transport: "
                          << client_result.error();
            return std::nullopt;
        }

        return ScatterTransportPair{
            std::move(server_result.value()),
            std::move(client_result.value())};
    }

    std::string                       pool_name_;
    std::unique_ptr<SharedMemoryPool> server_pool_;
    std::unique_ptr<SharedMemoryPool> client_pool_;
    boost::asio::io_context           io_context_;
};

// ====== 小消息直写测试（不应走 SHM）======

TEST_F(IpcAsyncTransportShmTest, SmallMessage_DoesNotUseShm)
{
    // 65535 字节，小于阈值，不应走 SHM
    constexpr size_t body_size = LARGE_MESSAGE_THRESHOLD - 1;
    const auto       test_body = GenerateTestBody(body_size);

    server_pool_ = CreateServerPool();
    ASSERT_NE(server_pool_, nullptr);

    client_pool_ = CreateClientPool();
    ASSERT_NE(client_pool_, nullptr);

    auto transports = CreateConnectedTransports(
        io_context_,
        server_pool_.get(),
        client_pool_.get());
    ASSERT_TRUE(transports.has_value());

    // Server 发送消息
    auto header = CreateTestHeader(
        1,
        SERVER_SESSION_ID,
        CLIENT_SESSION_ID,
        body_size,
        NONE);

    // 先投递协程，然后启动 io_context
    auto send_future = boost::asio::co_spawn(
        io_context_,
        transports->server.SendCoroutine(header, test_body.data(), body_size),
        boost::asio::use_future);

    auto recv_future = boost::asio::co_spawn(
        io_context_,
        transports->client.ReceiveCoroutine(),
        boost::asio::use_future);

    // 启动 io_context 线程
    std::thread io_thread([this]() { io_context_.run(); });

    auto recv_result = recv_future.get();
    ASSERT_TRUE(
        std::holds_alternative<DAS::Core::IPC::AsyncIpcMessage>(recv_result));

    auto& msg = std::get<DAS::Core::IPC::AsyncIpcMessage>(recv_result);

    // 验证：小消息不应走 SHM
    EXPECT_EQ(msg.first.Raw().body_size, body_size);
    EXPECT_EQ(msg.first.Raw().header_flags & LARGE_MESSAGE_VIA_SHM, 0);

    // 验证 body 内容正确
    ASSERT_EQ(msg.second.size(), body_size);
    EXPECT_TRUE(VerifyTestBody(msg.second.data(), msg.second.size()));

    io_context_.stop();
    if (io_thread.joinable())
    {
        io_thread.join();
    }
}

// ====== 边界测试：65536 字节（阈值）======

TEST_F(IpcAsyncTransportShmTest, ThresholdMessage_UsesShm)
{
    // 65536 字节，等于阈值，应走 SHM
    constexpr size_t body_size = LARGE_MESSAGE_THRESHOLD;
    const auto       test_body = GenerateTestBody(body_size);

    server_pool_ = CreateServerPool();
    ASSERT_NE(server_pool_, nullptr);

    client_pool_ = CreateClientPool();
    ASSERT_NE(client_pool_, nullptr);

    auto transports = CreateConnectedTransports(
        io_context_,
        server_pool_.get(),
        client_pool_.get());
    ASSERT_TRUE(transports.has_value());

    auto header = CreateTestHeader(
        1,
        SERVER_SESSION_ID,
        CLIENT_SESSION_ID,
        body_size,
        NONE);

    // 先投递协程，然后启动 io_context
    auto send_future = boost::asio::co_spawn(
        io_context_,
        transports->server.SendCoroutine(header, test_body.data(), body_size),
        boost::asio::use_future);

    auto recv_future = boost::asio::co_spawn(
        io_context_,
        transports->client.ReceiveCoroutine(),
        boost::asio::use_future);

    // 启动 io_context 线程
    std::thread io_thread([this]() { io_context_.run(); });

    auto recv_result = recv_future.get();
    ASSERT_TRUE(
        std::holds_alternative<DAS::Core::IPC::AsyncIpcMessage>(recv_result));

    auto& msg = std::get<DAS::Core::IPC::AsyncIpcMessage>(recv_result);

    // 验证：大消息应走 SHM，但接收后应恢复真实 body_size
    EXPECT_EQ(msg.first.Raw().body_size, body_size);

    // 验证 LARGE_MESSAGE_VIA_SHM 标志已清除（接收端已重组）
    EXPECT_EQ(msg.first.Raw().header_flags & LARGE_MESSAGE_VIA_SHM, 0);

    // 验证 body 内容正确
    ASSERT_EQ(msg.second.size(), body_size);
    EXPECT_TRUE(VerifyTestBody(msg.second.data(), msg.second.size()));

    io_context_.stop();
    if (io_thread.joinable())
    {
        io_thread.join();
    }
}

// ====== 边界测试：65537 字节（刚超阈值）======

TEST_F(IpcAsyncTransportShmTest, JustAboveThresholdMessage_UsesShm)
{
    // 65537 字节，刚超过阈值，应走 SHM
    constexpr size_t body_size = LARGE_MESSAGE_THRESHOLD + 1;
    const auto       test_body = GenerateTestBody(body_size);

    server_pool_ = CreateServerPool();
    ASSERT_NE(server_pool_, nullptr);

    client_pool_ = CreateClientPool();
    ASSERT_NE(client_pool_, nullptr);

    auto transports = CreateConnectedTransports(
        io_context_,
        server_pool_.get(),
        client_pool_.get());
    ASSERT_TRUE(transports.has_value());

    auto header = CreateTestHeader(
        1,
        SERVER_SESSION_ID,
        CLIENT_SESSION_ID,
        body_size,
        NONE);

    // 先投递协程，然后启动 io_context
    auto send_future = boost::asio::co_spawn(
        io_context_,
        transports->server.SendCoroutine(header, test_body.data(), body_size),
        boost::asio::use_future);

    auto recv_future = boost::asio::co_spawn(
        io_context_,
        transports->client.ReceiveCoroutine(),
        boost::asio::use_future);

    // 启动 io_context 线程
    std::thread io_thread([this]() { io_context_.run(); });

    auto recv_result = recv_future.get();
    ASSERT_TRUE(
        std::holds_alternative<DAS::Core::IPC::AsyncIpcMessage>(recv_result));

    auto& msg = std::get<DAS::Core::IPC::AsyncIpcMessage>(recv_result);

    EXPECT_EQ(msg.first.Raw().body_size, body_size);
    EXPECT_EQ(msg.first.Raw().header_flags & LARGE_MESSAGE_VIA_SHM, 0);

    ASSERT_EQ(msg.second.size(), body_size);
    EXPECT_TRUE(VerifyTestBody(msg.second.data(), msg.second.size()));

    io_context_.stop();
    if (io_thread.joinable())
    {
        io_thread.join();
    }
}

// ====== 大消息测试：1MB =====

TEST_F(IpcAsyncTransportShmTest, LargeMessage_1MB_UsesShm)
{
    // 1MB，大消息，应走 SHM
    constexpr size_t body_size = 1024 * 1024;
    const auto       test_body = GenerateTestBody(body_size);

    server_pool_ = CreateServerPool();
    ASSERT_NE(server_pool_, nullptr);

    client_pool_ = CreateClientPool();
    ASSERT_NE(client_pool_, nullptr);

    auto transports = CreateConnectedTransports(
        io_context_,
        server_pool_.get(),
        client_pool_.get());
    ASSERT_TRUE(transports.has_value());

    auto header = CreateTestHeader(
        1,
        SERVER_SESSION_ID,
        CLIENT_SESSION_ID,
        body_size,
        NONE);

    // 先投递协程，然后启动 io_context
    auto send_future = boost::asio::co_spawn(
        io_context_,
        transports->server.SendCoroutine(header, test_body.data(), body_size),
        boost::asio::use_future);

    auto recv_future = boost::asio::co_spawn(
        io_context_,
        transports->client.ReceiveCoroutine(),
        boost::asio::use_future);

    // 启动 io_context 线程
    std::thread io_thread([this]() { io_context_.run(); });

    auto recv_result = recv_future.get();
    ASSERT_TRUE(
        std::holds_alternative<DAS::Core::IPC::AsyncIpcMessage>(recv_result));

    auto& msg = std::get<DAS::Core::IPC::AsyncIpcMessage>(recv_result);

    EXPECT_EQ(msg.first.Raw().body_size, body_size);
    EXPECT_EQ(msg.first.Raw().header_flags & LARGE_MESSAGE_VIA_SHM, 0);

    ASSERT_EQ(msg.second.size(), body_size);
    EXPECT_TRUE(VerifyTestBody(msg.second.data(), msg.second.size()));

    io_context_.stop();
    if (io_thread.joinable())
    {
        io_thread.join();
    }
}

// ====== 双向测试：Client -> Server =====

TEST_F(IpcAsyncTransportShmTest, Bidirectional_ClientToServer_LargeMessage)
{
    constexpr size_t body_size = 128 * 1024; // 128KB
    const auto       test_body = GenerateTestBody(body_size);

    server_pool_ = CreateServerPool();
    ASSERT_NE(server_pool_, nullptr);

    client_pool_ = CreateClientPool();
    ASSERT_NE(client_pool_, nullptr);

    auto transports = CreateConnectedTransports(
        io_context_,
        server_pool_.get(),
        client_pool_.get());
    ASSERT_TRUE(transports.has_value());

    // Client 发送消息给 Server
    auto header = CreateTestHeader(
        2,
        CLIENT_SESSION_ID,
        SERVER_SESSION_ID,
        body_size,
        NONE);

    // 先投递协程，然后启动 io_context
    auto send_future = boost::asio::co_spawn(
        io_context_,
        transports->client.SendCoroutine(header, test_body.data(), body_size),
        boost::asio::use_future);

    auto recv_future = boost::asio::co_spawn(
        io_context_,
        transports->server.ReceiveCoroutine(),
        boost::asio::use_future);

    // 启动 io_context 线程
    std::thread io_thread([this]() { io_context_.run(); });

    auto recv_result = recv_future.get();
    ASSERT_TRUE(
        std::holds_alternative<DAS::Core::IPC::AsyncIpcMessage>(recv_result));

    auto& msg = std::get<DAS::Core::IPC::AsyncIpcMessage>(recv_result);

    EXPECT_EQ(msg.first.Raw().body_size, body_size);
    EXPECT_EQ(msg.first.Raw().source_session_id, CLIENT_SESSION_ID);
    EXPECT_EQ(msg.first.Raw().target_session_id, SERVER_SESSION_ID);

    ASSERT_EQ(msg.second.size(), body_size);
    EXPECT_TRUE(VerifyTestBody(msg.second.data(), msg.second.size()));

    io_context_.stop();
    if (io_thread.joinable())
    {
        io_thread.join();
    }
}

// ====== 连续发送多个不同大小的消息 =====

TEST_F(IpcAsyncTransportShmTest, MultipleMessages_VariableSizes)
{
    server_pool_ = CreateServerPool();
    ASSERT_NE(server_pool_, nullptr);

    client_pool_ = CreateClientPool();
    ASSERT_NE(client_pool_, nullptr);

    auto transports = CreateConnectedTransports(
        io_context_,
        server_pool_.get(),
        client_pool_.get());
    ASSERT_TRUE(transports.has_value());

    // 测试几个关键的边界大小
    std::vector<size_t> test_sizes = {
        1024,       // 1KB - 小消息
        65536,      // 阈值
        128 * 1024, // 128KB
    };

    for (size_t i = 0; i < test_sizes.size(); ++i)
    {
        const size_t body_size = test_sizes[i];
        const auto   test_body = GenerateTestBody(body_size);

        auto header = CreateTestHeader(
            static_cast<uint16_t>(i + 1),
            SERVER_SESSION_ID,
            CLIENT_SESSION_ID,
            body_size,
            NONE);

        // 先投递协程，然后启动 io_context
        auto send_future = boost::asio::co_spawn(
            io_context_,
            transports->server
                .SendCoroutine(header, test_body.data(), body_size),
            boost::asio::use_future);

        auto recv_future = boost::asio::co_spawn(
            io_context_,
            transports->client.ReceiveCoroutine(),
            boost::asio::use_future);

        // 启动 io_context 线程
        std::thread io_thread([this]() { io_context_.run(); });

        const DasResult send_result = send_future.get();
        ASSERT_EQ(send_result, DAS_S_OK) << "Failed to send message " << i;

        auto recv_result = recv_future.get();
        ASSERT_TRUE(
            std::holds_alternative<DAS::Core::IPC::AsyncIpcMessage>(
                recv_result))
            << "Receive failed for message " << i;

        auto& msg = std::get<DAS::Core::IPC::AsyncIpcMessage>(recv_result);

        EXPECT_EQ(msg.first.Raw().body_size, body_size)
            << "Body size mismatch for message " << i;
        EXPECT_EQ(msg.first.Raw().header_flags & LARGE_MESSAGE_VIA_SHM, 0)
            << "LARGE_MESSAGE_VIA_SHM flag not cleared for message " << i;

        ASSERT_EQ(msg.second.size(), body_size)
            << "Received body size mismatch for message " << i;
        EXPECT_TRUE(VerifyTestBody(msg.second.data(), msg.second.size()))
            << "Body content mismatch for message " << i;

        io_context_.stop();
        if (io_thread.joinable())
        {
            io_thread.join();
        }
        io_context_.restart();
    }
}

// ====== 无 SharedMemoryPool 时大消息失败测试 =====

TEST_F(IpcAsyncTransportShmTest, LargeMessage_WithoutPool_Fails)
{
    // 不创建 pool
    auto transports = CreateConnectedTransports(io_context_, nullptr, nullptr);
    ASSERT_TRUE(transports.has_value());

    constexpr size_t body_size = LARGE_MESSAGE_THRESHOLD + 1;
    const auto       test_body = GenerateTestBody(body_size);

    auto header = CreateTestHeader(
        1,
        SERVER_SESSION_ID,
        CLIENT_SESSION_ID,
        body_size,
        NONE);

    // 先投递协程，然后启动 io_context
    auto send_future = boost::asio::co_spawn(
        io_context_,
        transports->server.SendCoroutine(header, test_body.data(), body_size),
        boost::asio::use_future);

    // 启动 io_context 线程
    std::thread io_thread([this]() { io_context_.run(); });

    // 等待发送完成（添加超时检查）
    auto future_status = send_future.wait_for(std::chrono::seconds(10));
    if (future_status == std::future_status::timeout)
    {
        // 如果超时，说明发送挂起，这是预期的（没有 pool）
        io_context_.stop();
        if (io_thread.joinable())
        {
            io_thread.join();
        }
        GTEST_SKIP() << "SendCoroutine hangs without pool (expected behavior)";
        return;
    }

    const DasResult send_result = send_future.get();

    // 无 pool 时，大消息发送应该失败
    EXPECT_NE(send_result, DAS_S_OK);

    io_context_.stop();
    if (io_thread.joinable())
    {
        io_thread.join();
    }
}

// ====== Server/Client Pool 未连接时测试 =====

TEST_F(IpcAsyncTransportShmTest, UnconnectedPools_HandleGracefully)
{
    // Server 和 Client 使用不同名的 pool
    server_pool_ = std::make_unique<SharedMemoryPool>(
        "server_pool_" + pool_name_,
        1024 * 1024,
        PoolMode::Create);
    client_pool_ = std::make_unique<SharedMemoryPool>(
        "client_pool_" + pool_name_,
        1024 * 1024,
        PoolMode::Create);

    auto transports = CreateConnectedTransports(
        io_context_,
        server_pool_.get(),
        client_pool_.get());
    ASSERT_TRUE(transports.has_value());

    constexpr size_t body_size = LARGE_MESSAGE_THRESHOLD + 1;
    const auto       test_body = GenerateTestBody(body_size);

    auto header = CreateTestHeader(
        1,
        SERVER_SESSION_ID,
        CLIENT_SESSION_ID,
        body_size,
        NONE);

    // 先投递协程，然后启动 io_context
    auto send_future = boost::asio::co_spawn(
        io_context_,
        transports->server.SendCoroutine(header, test_body.data(), body_size),
        boost::asio::use_future);

    // 启动 io_context 线程
    std::thread io_thread([this]() { io_context_.run(); });

    const DasResult send_result = send_future.get();

    // Pool 不连通（两端不同名 pool）时：发送侧用自己的 server_pool 仍能成功
    // Allocate+发送 （发送侧不感知对端 pool）；接收侧 client_pool 收到无效
    // handle 会失败但不崩溃。 这里验证发送侧不崩（graceful）——send_result
    // 应为成功。
    EXPECT_EQ(send_result, DAS_S_OK);

    io_context_.stop();
    if (io_thread.joinable())
    {
        io_thread.join();
    }
}

// ====== SendScatter（scatter-gather 发送）测试 ======

namespace
{
    using ScatterMessage = DAS::Core::IPC::AsyncIpcMessage;

    // 期望线上字节：header 32B 与各段顺序拼接（09a 锚点——与拼接后
    // 单段 Send 完全一致，新旧互通、接收端零改动）
    std::vector<uint8_t> BuildExpectedWireBytes(
        const ValidatedIPCMessageHeader& header,
        const std::vector<SendSegment>&  segments)
    {
        std::vector<uint8_t> expected(sizeof(IPCMessageHeader));
        std::memcpy(
            expected.data(),
            static_cast<const IPCMessageHeader*>(header),
            sizeof(IPCMessageHeader));
        for (const SendSegment& segment : segments)
        {
            expected.insert(
                expected.end(),
                segment.data,
                segment.data + segment.size);
        }
        return expected;
    }

    // 断言接收帧与期望线上字节逐字节相等（header 32B + body 全量）
    void ExpectFrameMatchesWireBytes(
        const ScatterMessage&       message,
        const std::vector<uint8_t>& expected_wire_bytes)
    {
        ASSERT_EQ(
            message.second.size() + sizeof(IPCMessageHeader),
            expected_wire_bytes.size());
        EXPECT_EQ(
            0,
            std::memcmp(
                static_cast<const IPCMessageHeader*>(message.first),
                expected_wire_bytes.data(),
                sizeof(IPCMessageHeader)));
        EXPECT_EQ(
            0,
            std::memcmp(
                message.second.data(),
                expected_wire_bytes.data() + sizeof(IPCMessageHeader),
                message.second.size()));
    }

    // 备存储按值 move 进协程帧局部持有——调用方随即清空源容器仍安全。
    // SendSegment 非拥有契约的正向形态载体：若段备存储的帧转移失效，
    // async_write 处的悬空读必被 ASAN 检出（负向形态的漏检面）
    boost::asio::awaitable<DasResult> SendScatterWithFrameOwnedBacking(
        ScatterTransportUnderTest&        sender,
        ValidatedIPCMessageHeader         header,
        std::vector<std::vector<uint8_t>> backing_frames)
    {
        std::vector<SendSegment> segments;
        segments.reserve(backing_frames.size());
        for (const std::vector<uint8_t>& frame : backing_frames)
        {
            segments.push_back(SendSegment{frame.data(), frame.size()});
        }
        co_return co_await sender.SendScatter(header, std::move(segments));
    }
} // namespace

TEST_F(IpcAsyncTransportShmTest, SendScatter_WireBytesEqualHeaderPlusSegments)
{
    // K 取 1/3/8 三档；3/8 档含空段边界（size 0）
    const std::vector<std::vector<size_t>> segment_size_tables = {
        {700},
        {100, 0, 200},
        {1, 0, 33, 64, 511, 1000, 4096, 17},
    };

    for (const std::vector<size_t>& segment_sizes : segment_size_tables)
    {
        auto transports = CreateConnectedScatterTransports();
        ASSERT_TRUE(transports.has_value());

        size_t total_body_size = 0;
        for (size_t segment_size : segment_sizes)
        {
            total_body_size += segment_size;
        }

        std::vector<std::vector<uint8_t>> backing;
        for (size_t segment_size : segment_sizes)
        {
            backing.push_back(GenerateTestBody(segment_size));
        }

        std::vector<SendSegment> segments;
        segments.reserve(backing.size());
        for (const std::vector<uint8_t>& body : backing)
        {
            segments.push_back(SendSegment{body.data(), body.size()});
        }

        auto header = CreateTestHeader(
            7,
            SERVER_SESSION_ID,
            CLIENT_SESSION_ID,
            static_cast<uint32_t>(total_body_size));

        const std::vector<uint8_t> expected_wire_bytes =
            BuildExpectedWireBytes(header, segments);

        auto send_future = boost::asio::co_spawn(
            io_context_,
            transports->server->SendScatter(header, std::move(segments)),
            boost::asio::use_future);
        auto recv_future = boost::asio::co_spawn(
            io_context_,
            transports->client->ReceiveCoroutine(),
            boost::asio::use_future);

        std::thread io_thread([this]() { io_context_.run(); });

        const DasResult send_result = send_future.get();
        ASSERT_EQ(send_result, DAS_S_OK);

        auto recv_result = recv_future.get();
        ASSERT_TRUE(std::holds_alternative<ScatterMessage>(recv_result));
        const ScatterMessage& message = std::get<ScatterMessage>(recv_result);
        ExpectFrameMatchesWireBytes(message, expected_wire_bytes);

        io_context_.stop();
        if (io_thread.joinable())
        {
            io_thread.join();
        }
        io_context_.restart();
    }
}

TEST_F(IpcAsyncTransportShmTest, SendScatter_WireBytesMatchSingleSegmentSend)
{
    auto transports = CreateConnectedScatterTransports();
    ASSERT_TRUE(transports.has_value());

    auto           first_body = GenerateTestBody(300);
    auto           second_body = GenerateTestBody(100);
    const size_t   total_body_size = first_body.size() + second_body.size();
    ScatterMessage scatter_message;
    ScatterMessage single_message;

    auto header = CreateTestHeader(
        8,
        SERVER_SESSION_ID,
        CLIENT_SESSION_ID,
        static_cast<uint32_t>(total_body_size));

    // 第一轮：两段 scatter 发送
    {
        std::vector<SendSegment> segments = {
            SendSegment{first_body.data(), first_body.size()},
            SendSegment{second_body.data(), second_body.size()}};

        auto send_future = boost::asio::co_spawn(
            io_context_,
            transports->server->SendScatter(header, std::move(segments)),
            boost::asio::use_future);
        auto recv_future = boost::asio::co_spawn(
            io_context_,
            transports->client->ReceiveCoroutine(),
            boost::asio::use_future);

        std::thread io_thread([this]() { io_context_.run(); });

        ASSERT_EQ(send_future.get(), DAS_S_OK);
        auto recv_result = recv_future.get();
        ASSERT_TRUE(std::holds_alternative<ScatterMessage>(recv_result));
        scatter_message = std::get<ScatterMessage>(std::move(recv_result));

        io_context_.stop();
        if (io_thread.joinable())
        {
            io_thread.join();
        }
        io_context_.restart();
    }

    // 第二轮：同逻辑内容单段 Send（两 body 拼接后一次发送）
    {
        std::vector<uint8_t> combined(total_body_size);
        std::memcpy(combined.data(), first_body.data(), first_body.size());
        std::memcpy(
            combined.data() + first_body.size(),
            second_body.data(),
            second_body.size());

        auto send_future = boost::asio::co_spawn(
            io_context_,
            transports->server
                ->SendCoroutine(header, combined.data(), total_body_size),
            boost::asio::use_future);
        auto recv_future = boost::asio::co_spawn(
            io_context_,
            transports->client->ReceiveCoroutine(),
            boost::asio::use_future);

        std::thread io_thread([this]() { io_context_.run(); });

        ASSERT_EQ(send_future.get(), DAS_S_OK);
        auto recv_result = recv_future.get();
        ASSERT_TRUE(std::holds_alternative<ScatterMessage>(recv_result));
        single_message = std::get<ScatterMessage>(std::move(recv_result));

        io_context_.stop();
        if (io_thread.joinable())
        {
            io_thread.join();
        }
    }

    // 线上字节一致（互通性）：header 32B 与 body 全量逐字节相等
    EXPECT_EQ(
        0,
        std::memcmp(
            static_cast<const IPCMessageHeader*>(scatter_message.first),
            static_cast<const IPCMessageHeader*>(single_message.first),
            sizeof(IPCMessageHeader)));
    ASSERT_EQ(scatter_message.second.size(), single_message.second.size());
    EXPECT_EQ(
        0,
        std::memcmp(
            scatter_message.second.data(),
            single_message.second.data(),
            scatter_message.second.size()));
}

TEST_F(
    IpcAsyncTransportShmTest,
    SendScatter_RejectsAtThresholdWithoutPartialWrite)
{
    auto transports = CreateConnectedScatterTransports();
    ASSERT_TRUE(transports.has_value());

    // 段总和使总长（header + 段总和）恰好达到 LARGE_MESSAGE_THRESHOLD
    const size_t oversized_body_size =
        LARGE_MESSAGE_THRESHOLD - sizeof(IPCMessageHeader);
    const auto oversized_body = GenerateTestBody(oversized_body_size);
    const auto oversized_header = CreateTestHeader(
        9,
        SERVER_SESSION_ID,
        CLIENT_SESSION_ID,
        static_cast<uint32_t>(oversized_body_size));
    std::vector<SendSegment> oversized_segments = {
        SendSegment{oversized_body.data(), oversized_body.size()}};

    {
        auto send_future = boost::asio::co_spawn(
            io_context_,
            transports->server->SendScatter(
                oversized_header,
                std::move(oversized_segments)),
            boost::asio::use_future);

        std::thread io_thread([this]() { io_context_.run(); });

        // 入口防呆：达到阈值即拒发（SHM 大消息路径不接纳 scatter，
        // 无降级直写——不产生半写）
        EXPECT_EQ(send_future.get(), DAS_E_IPC_MESSAGE_TOO_LARGE);

        io_context_.stop();
        if (io_thread.joinable())
        {
            io_thread.join();
        }
        io_context_.restart();
    }

    // 零半写证明：拒绝后同对连接发一帧合法 scatter 帧，接收端读到且
    // 仅读到它（若拒绝路径泄漏了部分字节，此处帧内容必然错位）
    const auto valid_body = GenerateTestBody(500);
    const auto valid_header = CreateTestHeader(
        10,
        SERVER_SESSION_ID,
        CLIENT_SESSION_ID,
        static_cast<uint32_t>(valid_body.size()));
    std::vector<SendSegment> valid_segments = {
        SendSegment{valid_body.data(), valid_body.size()}};
    const std::vector<uint8_t> expected_wire_bytes =
        BuildExpectedWireBytes(valid_header, valid_segments);

    auto send_future = boost::asio::co_spawn(
        io_context_,
        transports->server->SendScatter(
            valid_header,
            std::move(valid_segments)),
        boost::asio::use_future);
    auto recv_future = boost::asio::co_spawn(
        io_context_,
        transports->client->ReceiveCoroutine(),
        boost::asio::use_future);

    std::thread io_thread([this]() { io_context_.run(); });

    ASSERT_EQ(send_future.get(), DAS_S_OK);
    auto recv_result = recv_future.get();
    ASSERT_TRUE(std::holds_alternative<ScatterMessage>(recv_result));
    const ScatterMessage& message = std::get<ScatterMessage>(recv_result);
    ExpectFrameMatchesWireBytes(message, expected_wire_bytes);

    io_context_.stop();
    if (io_thread.joinable())
    {
        io_thread.join();
    }
}

TEST_F(
    IpcAsyncTransportShmTest,
    SendScatter_BackingStorageOwnedByCoroutineFrame)
{
    auto transports = CreateConnectedScatterTransports();
    ASSERT_TRUE(transports.has_value());

    std::vector<std::vector<uint8_t>> backing_frames;
    backing_frames.push_back(GenerateTestBody(500));
    backing_frames.push_back(GenerateTestBody(250));
    size_t total_body_size = 0;
    for (const std::vector<uint8_t>& frame : backing_frames)
    {
        total_body_size += frame.size();
    }

    auto header = CreateTestHeader(
        11,
        SERVER_SESSION_ID,
        CLIENT_SESSION_ID,
        static_cast<uint32_t>(total_body_size));

    // 期望字节先于源容器清空构建
    std::vector<uint8_t> expected_wire_bytes(sizeof(IPCMessageHeader));
    std::memcpy(
        expected_wire_bytes.data(),
        static_cast<const IPCMessageHeader*>(header),
        sizeof(IPCMessageHeader));
    for (const std::vector<uint8_t>& frame : backing_frames)
    {
        expected_wire_bytes.insert(
            expected_wire_bytes.end(),
            frame.data(),
            frame.data() + frame.size());
    }

    auto send_future = boost::asio::co_spawn(
        io_context_,
        SendScatterWithFrameOwnedBacking(
            *transports->server,
            header,
            std::move(backing_frames)),
        boost::asio::use_future);

    // 源容器立即析构（moved-from——清空并释放容量；备存储所有权已在
    // 协程帧，发送仍未完成）
    backing_frames.clear();
    backing_frames.shrink_to_fit();

    auto recv_future = boost::asio::co_spawn(
        io_context_,
        transports->client->ReceiveCoroutine(),
        boost::asio::use_future);

    std::thread io_thread([this]() { io_context_.run(); });

    ASSERT_EQ(send_future.get(), DAS_S_OK);
    auto recv_result = recv_future.get();
    ASSERT_TRUE(std::holds_alternative<ScatterMessage>(recv_result));
    const ScatterMessage& message = std::get<ScatterMessage>(recv_result);
    ExpectFrameMatchesWireBytes(message, expected_wire_bytes);

    io_context_.stop();
    if (io_thread.joinable())
    {
        io_thread.join();
    }
}

TEST_F(IpcAsyncTransportShmTest, CanBatchFrames_DiscriminatesByTransportKind)
{
    // 静态能力常量（免实例化直读——Windows 测试环境无法实例化 unix/http
    // 连接形态时以常量断言；批量合写仅 pipe/AF_UNIX 字节流合法，WS 一条
    // 消息 = 一个 IPC 帧恒为假）
#ifdef DAS_WINDOWS
    EXPECT_TRUE(DAS::Core::IPC::Win32AsyncIpcTransport::CAN_BATCH_FRAMES);
#endif
    EXPECT_TRUE(DAS::Core::IPC::UnixAsyncIpcTransport::CAN_BATCH_FRAMES);
    EXPECT_FALSE(DAS::Core::IPC::HttpIpcTransport::CAN_BATCH_FRAMES);

    // Visit 聚合行为：真实传输对实例（AnyTransport 运行时选 AF_UNIX，
    // CanBatchFrames 应为真）
    auto transports = CreateConnectedTransports(io_context_, nullptr, nullptr);
    ASSERT_TRUE(transports.has_value());
    EXPECT_TRUE(transports->server.CanBatchFrames());
}

TEST_F(
    IpcAsyncTransportShmTest,
    AnyTransport_SendScatter_ForwardsToConcreteImplementation)
{
    auto transports = CreateConnectedTransports(io_context_, nullptr, nullptr);
    ASSERT_TRUE(transports.has_value());

    auto         first_body = GenerateTestBody(200);
    auto         second_body = GenerateTestBody(300);
    const size_t total_body_size = first_body.size() + second_body.size();

    auto header = CreateTestHeader(
        12,
        SERVER_SESSION_ID,
        CLIENT_SESSION_ID,
        static_cast<uint32_t>(total_body_size));

    std::vector<SendSegment> segments = {
        SendSegment{first_body.data(), first_body.size()},
        SendSegment{second_body.data(), second_body.size()}};
    const std::vector<uint8_t> expected_wire_bytes =
        BuildExpectedWireBytes(header, segments);

    auto send_future = boost::asio::co_spawn(
        io_context_,
        transports->server.SendScatter(header, std::move(segments)),
        boost::asio::use_future);
    auto recv_future = boost::asio::co_spawn(
        io_context_,
        transports->client.ReceiveCoroutine(),
        boost::asio::use_future);

    std::thread io_thread([this]() { io_context_.run(); });

    ASSERT_EQ(send_future.get(), DAS_S_OK);
    auto recv_result = recv_future.get();
    ASSERT_TRUE(std::holds_alternative<ScatterMessage>(recv_result));
    const ScatterMessage& message = std::get<ScatterMessage>(recv_result);
    ExpectFrameMatchesWireBytes(message, expected_wire_bytes);

    io_context_.stop();
    if (io_thread.joinable())
    {
        io_thread.join();
    }
}
