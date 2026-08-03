#include "beast/SseSession.hpp"
#include "SseHub.hpp"

#include <boost/beast/_experimental/test/stream.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace
{
    using Das::Http::SseHub;
    using SseTestSession =
        Das::Http::Beast::SseSessionT<boost::beast::test::stream>;

    // 处理就绪的异步 handler（SSE 长连接有 pending keep-alive timer，
    // 不能 run() 阻塞，只能 poll() 驱动）。
    void PumpIo(boost::asio::io_context& ioc)
    {
        for (int i = 0; i < 50; ++i)
        {
            ioc.poll();
        }
    }

    struct SseTestFixture
    {
        boost::asio::io_context         ioc;
        std::shared_ptr<SseHub>         hub;
        boost::beast::test::stream      client;
        boost::beast::test::stream      server_stream;
        std::shared_ptr<SseTestSession> session;

        SseTestFixture()
            : hub(std::make_shared<SseHub>(ioc)), client(ioc),
              server_stream(ioc)
        {
            client.connect(server_stream);
        }

        void Start(
            const std::string&        key = "test-key",
            std::chrono::milliseconds keepalive = std::chrono::seconds(15))
        {
            session = std::make_shared<SseTestSession>(
                std::move(server_stream),
                key,
                hub,
                keepalive);
            session->DoStart();
            PumpIo(ioc);
        }

        ~SseTestFixture()
        {
            if (session)
            {
                session->Close();
            }
        }
    };

    TEST(SseSessionTest, WritesHeader_KeepsConnectionOpen)
    {
        SseTestFixture f;
        f.Start();

        const std::string header(f.client.str());
        EXPECT_NE(header.find("HTTP/1.1 200 OK"), std::string::npos);
        EXPECT_NE(header.find("text/event-stream"), std::string::npos);
        EXPECT_NE(header.find("chunked"), std::string::npos);
        EXPECT_NE(header.find("no-cache"), std::string::npos);

        // 头写完仍保持连接：会话已注册到 hub，未被注销
        EXPECT_EQ(f.hub->SubscriberCount("test-key"), 1u);
    }

    TEST(SseSessionTest, BroadcastWritesChunkFrame)
    {
        SseTestFixture f;
        f.Start();
        f.client.clear();

        f.session->WriteData(std::make_shared<const std::string>(R"({"a":1})"));
        PumpIo(f.ioc);

        const std::string data(f.client.str());
        EXPECT_NE(data.find("data: {\"a\":1}\n\n"), std::string::npos);
    }

    TEST(SseSessionTest, TwoBroadcasts_WriteOrderedChunkFrames)
    {
        SseTestFixture f;
        f.Start();
        f.client.clear();

        f.session->WriteData(
            std::make_shared<const std::string>(R"({"seq":1})"));
        f.session->WriteData(
            std::make_shared<const std::string>(R"({"seq":2})"));
        PumpIo(f.ioc);

        const std::string data(f.client.str());
        const auto        p1 = data.find(R"({"seq":1})");
        const auto        p2 = data.find(R"({"seq":2})");
        EXPECT_NE(p1, std::string::npos);
        EXPECT_NE(p2, std::string::npos);
        EXPECT_LT(p1, p2);
    }

    TEST(SseSessionTest, KeepaliveWritesCommentFrame)
    {
        SseTestFixture f;
        f.Start("test-key", std::chrono::milliseconds(20));
        f.client.clear();

        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        PumpIo(f.ioc);

        const std::string data(f.client.str());
        EXPECT_NE(data.find(": keepalive\n\n"), std::string::npos);
    }

    TEST(SseSessionTest, WriteFailure_UnregistersAndCloses)
    {
        SseTestFixture f;
        f.Start();
        ASSERT_EQ(f.hub->SubscriberCount("test-key"), 1u);

        // 销毁客户端连接 → 服务端后续写返回 connection_reset
        f.client = boost::beast::test::stream(f.ioc);
        f.session->WriteData(std::make_shared<const std::string>(R"({"x":1})"));
        PumpIo(f.ioc);

        // 写失败触发注销，hub 中无残留
        EXPECT_EQ(f.hub->SubscriberCount("test-key"), 0u);
    }
} // namespace
