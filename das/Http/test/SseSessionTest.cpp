#include "beast/SseSession.hpp"
#include "SseHub.hpp"
#include "beast/Server.hpp"

#include <boost/beast/_experimental/test/stream.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace
{
    using Das::Http::SseHub;
    using SseTestSession =
        Das::Http::Beast::SseSessionT<boost::beast::test::stream>;
    using SseOnRegistered =
        std::function<void(Das::Http::Beast::SseSessionBase&)>;

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
            std::chrono::milliseconds keepalive = std::chrono::seconds(15),
            SseOnRegistered           on_registered = {})
        {
            session = std::make_shared<SseTestSession>(
                std::move(server_stream),
                key,
                hub,
                keepalive,
                std::move(on_registered));
            session->DoStart();
            PumpIo(ioc);
        }

        ~SseTestFixture()
        {
            if (session)
            {
                session->Close();
                // Close 异步写 chunked 终止块后 ShutdownSocket，pump 驱动完成
                // （否则 ioc 析构时 async_write 挂起引用已销毁对象）。
                PumpIo(ioc);
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

    // 回调时机：on_registered 在头写完后恰好调用一次，且执行时 Register
    // 已完成（订阅计数可见为 1）——注册前重放会与头写入并发交错损坏流。
    TEST(SseSessionTest, OnRegisteredFiresOnceAfterRegistration)
    {
        SseTestFixture f;
        int            call_count = 0;
        std::size_t    subscribers_at_callback = 0;
        f.Start(
            "test-key",
            std::chrono::seconds(15),
            [&call_count, &subscribers_at_callback, &f](
                Das::Http::Beast::SseSessionBase& /*session*/)
            {
                ++call_count;
                subscribers_at_callback = f.hub->SubscriberCount("test-key");
            });

        EXPECT_EQ(call_count, 1);
        EXPECT_EQ(subscribers_at_callback, 1u);
    }

    // 字节序：200 头 → 注册后回调内补发的重放帧 → 注册后的直播帧，
    // 三者顺序不可颠倒（strand 串行保证重放先于直播）。
    TEST(SseSessionTest, ReplayThenLive_WriteOrderedAfterHeader)
    {
        SseTestFixture f;
        f.Start(
            "test-key",
            std::chrono::seconds(15),
            [](Das::Http::Beast::SseSessionBase& session)
            {
                // 模拟订阅建立处重放：注册完成后立即补发一帧。
                session.WriteData(
                    std::make_shared<const std::string>(R"({"replay":1})"));
            });

        // 注册后的直播帧。
        f.hub->Broadcast(
            "test-key",
            std::make_shared<const std::string>(R"({"live":2})"));
        PumpIo(f.ioc);

        const std::string data(f.client.str());
        const auto        header_pos = data.find("HTTP/1.1 200 OK");
        const auto        replay_pos = data.find(R"({"replay":1})");
        const auto        live_pos = data.find(R"({"live":2})");
        ASSERT_NE(header_pos, std::string::npos);
        ASSERT_NE(replay_pos, std::string::npos);
        ASSERT_NE(live_pos, std::string::npos);
        EXPECT_LT(header_pos, replay_pos);
        EXPECT_LT(replay_pos, live_pos);
    }

    // key gate 拒绝：events 路径请求不进 SSE 分流，落普通路由（生产中
    // events 路径无注册路由 → 404）；gate 缺省/放行时分流照旧。
    TEST(SseSessionTest, KeyGateRejectionFallsThroughToRouter)
    {
        boost::asio::io_context ioc;
        auto                    hub = std::make_shared<SseHub>(ioc);
        auto router = std::make_shared<Das::Http::Beast::Router>();

        const std::string target =
            std::string(Das::Http::Beast::kSseRequestPathPrefix) + "dead-key";

        // 观察点：路由被命中即证明请求落进了普通路由（响应 404 对齐生产
        // events 路径无注册路由的语义）。注册路径不带前导斜杠（生产路由
        // 均以 DAS_HTTP_API_PREFIX 开头注册，Handle 查找时剥离请求 target
        // 的前导斜杠）。
        bool routed = false;
        router->Get(
            target.substr(1),
            [&routed](const Das::Http::Beast::HttpRequest&)
            {
                routed = true;
                return Das::Http::Beast::HttpResponse(
                    boost::beast::http::status::not_found);
            });

        boost::beast::http::request<boost::beast::http::string_body> request;
        request.method(boost::beast::http::verb::get);
        request.target(target);
        request.version(11);
        request.keep_alive(false);

        // gate 返回 false → 拒绝订阅，落普通路由。
        {
            auto session = std::make_shared<Das::Http::Beast::Session>(
                boost::asio::ip::tcp::socket(ioc),
                router,
                [] { return false; },
                nullptr,
                hub,
                [](const std::string&) { return false; });
            session->RunWithParser(request);
            PumpIo(ioc);
            EXPECT_TRUE(routed);
        }

        // 无 gate（缺省放行）→ SSE 分流接管，不落路由。
        {
            routed = false;
            auto session = std::make_shared<Das::Http::Beast::Session>(
                boost::asio::ip::tcp::socket(ioc),
                router,
                [] { return false; },
                nullptr,
                hub);
            session->RunWithParser(request);
            PumpIo(ioc);
            EXPECT_FALSE(routed);
        }
    }
} // namespace
