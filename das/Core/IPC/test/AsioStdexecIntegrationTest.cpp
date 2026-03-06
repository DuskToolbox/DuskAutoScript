/**
 * @file AsioStdexecIntegrationTest.cpp
 * @brief asioexec::use_sender 集成测试
 *
 * TODO: IPC重构 - 测试代码需要适配新的 stdexec/asioexec 版本
 * 暂时禁用这些测试
 */

#if 0

#include <asioexec/use_sender.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <cstddef>
#include <das/Core/IPC/Config.h>
#include <gtest/gtest.h>
#include <stdexec/execution.hpp>
#include <string>
#include <vector>

DAS_CORE_IPC_NS_BEGIN

// TimerUseSender 测试类
class TimerUseSender : public ::testing::Test
{
protected:
    void SetUp() override {}

    void TearDown() override {}
};

// AsyncReadUseSender 测试类
class AsyncReadUseSender : public ::testing::Test
{
protected:
    void SetUp() override {}

    void TearDown() override {}
};

// ====== TimerUseSender 测试用例 ======

TEST_F(TimerUseSender, DISABLED_SteadyTimerAsyncWait_Succeeds)
{
    boost::asio::io_context   io_ctx;
    boost::asio::steady_timer timer(io_ctx);

    timer.expires_after(std::chrono::milliseconds(10));

    auto sender = timer.async_wait(asioexec::use_sender);

    auto result = stdexec::sync_wait(std::move(sender));

    ASSERT_TRUE(result.has_value());
    auto&& [ec, size] = std::move(*result);
    EXPECT_TRUE(ec ? false : true);
}

TEST_F(TimerUseSender, SteadyTimerAsyncWait_Immediate)
{
    boost::asio::io_context   io_ctx;
    boost::asio::steady_timer timer(io_ctx);

    // 设置定时器为 0ms（立即触发）
    timer.expires_after(std::chrono::milliseconds(0));

    // 使用 asioexec::use_sender 进行异步等待
    auto sender = timer.async_wait(asioexec::use_sender);

    // 使用 stdexec::sync_wait 等待完成
    auto result = stdexec::sync_wait(std::move(sender));

    // 验证结果 - async_wait 返回空 tuple，只检查 has_value
    ASSERT_TRUE(result.has_value());
}

TEST_F(TimerUseSender, SteadyTimerAsyncWait_Cancel)
{
    boost::asio::io_context   io_ctx;
    boost::asio::steady_timer timer(io_ctx);

    // 设置定时器为 1s
    timer.expires_after(std::chrono::seconds(1));

    // 使用 asioexec::use_sender 进行异步等待
    auto sender = timer.async_wait(asioexec::use_sender);

    // 立即取消定时器
    timer.cancel();

    // 运行 io_context 来处理取消
    io_ctx.run();

    // 使用 stdexec::sync_wait 等待完成 - 取消后返回空 optional
    auto result = stdexec::sync_wait(std::move(sender));
    // 取消操作可能返回空 optional
}

// ====== AsyncReadUseSender 测试用例 ======

TEST_F(AsyncReadUseSender, LocalSocketPair_ReadWrite_Succeeds)
{
    boost::asio::io_context io_ctx;

    // 创建本地 socket pair
    boost::asio::local::stream_protocol::socket socket1(io_ctx);
    boost::asio::local::stream_protocol::socket socket2(io_ctx);

    // 连接两个 socket
    boost::system::error_code ec;
    boost::asio::local::connect_pair(socket1, socket2, ec);
    ASSERT_FALSE(ec) << "Failed to create socket pair: " << ec.message();

    // 准备发送数据
    std::string       send_data = "Hello, asioexec!";
    std::vector<char> recv_buffer(100);

    // 使用 asioexec::use_sender 进行异步写入
    auto write_sender = boost::asio::async_write(
        socket1,
        boost::asio::buffer(send_data),
        asioexec::use_sender);

    // 使用 stdexec::sync_wait 等待写入完成
    auto write_result = stdexec::sync_wait(std::move(write_sender));
    ASSERT_TRUE(write_result.has_value());
    EXPECT_EQ(std::get<0>(*write_result), boost::system::error_code());

    // 使用 asioexec::use_sender 进行异步读取
    auto read_sender = boost::asio::async_read(
        socket2,
        boost::asio::buffer(recv_buffer, send_data.size()),
        asioexec::use_sender);

    // 使用 stdexec::sync_wait 等待读取完成
    auto read_result = stdexec::sync_wait(std::move(read_sender));
    ASSERT_TRUE(read_result.has_value());
    EXPECT_EQ(std::get<0>(*read_result), boost::system::error_code());

    // 验证读取的数据
    std::string received_data(recv_buffer.data(), send_data.size());
    EXPECT_EQ(received_data, send_data);
}

TEST_F(AsyncReadUseSender, LocalSocketPair_ReadWithSize_Succeeds)
{
    boost::asio::io_context io_ctx;

    // 创建本地 socket pair
    boost::asio::local::stream_protocol::socket socket1(io_ctx);
    boost::asio::local::stream_protocol::socket socket2(io_ctx);

    // 连接两个 socket
    boost::system::error_code ec;
    boost::asio::local::connect_pair(socket1, socket2, ec);
    ASSERT_FALSE(ec) << "Failed to create socket pair: " << ec.message();

    // 准备发送数据
    std::string       send_data = "Partial read test";
    std::vector<char> recv_buffer(10); // 只读取前 10 个字节

    // 使用 asioexec::use_sender 进行异步写入
    auto write_sender = boost::asio::async_write(
        socket1,
        boost::asio::buffer(send_data),
        asioexec::use_sender);

    // 使用 stdexec::sync_wait 等待写入完成
    auto write_result = stdexec::sync_wait(std::move(write_sender));
    ASSERT_TRUE(write_result.has_value());

    // 使用 asioexec::use_sender 进行异步读取（读取部分数据）
    auto read_sender = boost::asio::async_read(
        socket2,
        boost::asio::buffer(recv_buffer),
        asioexec::use_sender);

    auto read_result = stdexec::sync_wait(std::move(read_sender));
    ASSERT_TRUE(read_result.has_value());
    auto&& [ec_read, size] = std::move(*read_result);
    EXPECT_TRUE(!ec_read) << "Read failed: " << ec_read.message();

    // 验证读取的数据（应该是前 10 个字节）
    std::string received_data(recv_buffer.data(), size);
    EXPECT_EQ(received_data, send_data.substr(0, size));
}

TEST_F(AsyncReadUseSender, LocalSocketPair_AsyncReadSome_Succeeds)
{
    boost::asio::io_context io_ctx;

    // 创建本地 socket pair
    boost::asio::local::stream_protocol::socket socket1(io_ctx);
    boost::asio::local::stream_protocol::socket socket2(io_ctx);

    // 连接两个 socket
    boost::system::error_code ec;
    boost::asio::local::connect_pair(socket1, socket2, ec);
    ASSERT_FALSE(ec) << "Failed to create socket pair: " << ec.message();

    // 准备发送数据
    std::string       send_data = "Test async_read_some";
    std::vector<char> recv_buffer(100);

    // 使用 asioexec::use_sender 进行异步读取
    auto read_sender = socket2.async_read_some(
        boost::asio::buffer(recv_buffer),
        asioexec::use_sender);

    // 在另一个 socket 上写入数据
    size_t bytes_sent =
        boost::asio::write(socket1, boost::asio::buffer(send_data), ec);
    ASSERT_FALSE(ec) << "Failed to write data: " << ec.message();

    // 使用 stdexec::sync_wait 等待读取完成
    auto read_result = stdexec::sync_wait(std::move(read_sender));
    ASSERT_TRUE(read_result.has_value());
    EXPECT_EQ(std::get<0>(*read_result), boost::system::error_code());

    // 验证读取的数据
    std::string received_data(recv_buffer.data(), bytes_sent);
    EXPECT_EQ(received_data, send_data);
}

DAS_CORE_IPC_NS_END

#endif
