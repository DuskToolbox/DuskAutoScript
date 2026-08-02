#include "../src/PiHttpClient.h"

#include <das/_autogen/idl/abi/DasResult.h>
#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace
{
    using namespace Das;
    using namespace Das::Plugins::DasMaaPi;

    // Minimal in-process HTTP server bound to 127.0.0.1:0 (ephemeral port).
    // Serves a canned body to every request and records the last request
    // target. Runs fully on the loopback interface — no external network.
    class LocalHttpServer
    {
    public:
        explicit LocalHttpServer(std::string body = "{\"ok\":true}")
            : body_(std::move(body))
        {
        }

        ~LocalHttpServer() { Stop(); }

        void Start()
        {
            acceptor_ = std::make_unique<boost::asio::ip::tcp::acceptor>(
                io_,
                boost::asio::ip::tcp::endpoint(
                    boost::asio::ip::address_v4::loopback(),
                    0));
            port_ = acceptor_->local_endpoint().port();
            running_.store(true);
            thread_ = std::thread([this] { AcceptLoop(); });
        }

        void Stop()
        {
            if (!running_.exchange(false))
            {
                return;
            }
            if (acceptor_)
            {
                acceptor_->close();
            }
            if (thread_.joinable())
            {
                thread_.join();
            }
        }

        std::uint16_t     Port() const { return port_; }
        std::string       LastTarget() const { return last_target_; }
        int               RequestCount() const { return request_count_.load(); }
        void              SetBody(std::string body) { body_ = std::move(body); }

    private:
        void AcceptLoop()
        {
            while (running_.load())
            {
                boost::system::error_code       ec;
                boost::asio::ip::tcp::socket    sock = acceptor_->accept(ec);
                if (ec || !running_.load())
                {
                    break;
                }
                std::thread(
                    [this, s = std::move(sock)]() mutable
                    { Handle(std::move(s)); })
                    .detach();
            }
        }

        void Handle(boost::asio::ip::tcp::socket sock)
        {
            try
            {
                boost::beast::flat_buffer buffer;
                boost::beast::http::request<boost::beast::http::string_body>
                                                                        req;
                boost::beast::http::read(sock, buffer, req);
                request_count_.fetch_add(1);
                last_target_ = std::string(req.target());

                boost::beast::http::response<boost::beast::http::string_body>
                                                                        resp;
                resp.result(boost::beast::http::status::ok);
                resp.version(11);
                resp.set(
                    boost::beast::http::field::content_type,
                    "application/json");
                resp.body() = body_;
                resp.prepare_payload();
                boost::beast::http::write(sock, resp);
            }
            catch (...)
            {
            }
        }

        boost::asio::io_context                                io_;
        std::unique_ptr<boost::asio::ip::tcp::acceptor>        acceptor_;
        std::thread                                            thread_;
        std::atomic<bool>                                      running_{false};
        std::uint16_t                                          port_ = 0;
        std::string                                            body_;
        std::atomic<int>                                       request_count_{0};
        std::string                                            last_target_;
    };

    TEST(PiHttpClient, GetReturnsBodyAndStatusFromLocalMock)
    {
        LocalHttpServer server;
        server.Start();

        BeastHttpClient client;
        std::string     body;
        int             status = 0;
        const auto      url =
            "http://127.0.0.1:" + std::to_string(server.Port())
            + "/releases/latest";
        const auto result = client.Get(url, body, status);

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_EQ(status, 200);
        EXPECT_EQ(body, "{\"ok\":true}");
        EXPECT_EQ(server.LastTarget(), "/releases/latest");
        EXPECT_EQ(server.RequestCount(), 1);
    }

    TEST(PiHttpClient, HeadReturnsStatusFromLocalMock)
    {
        LocalHttpServer server;
        server.Start();

        BeastHttpClient client;
        int             status = 0;
        const auto      url =
            "http://127.0.0.1:" + std::to_string(server.Port()) + "/latest";
        const auto result = client.Head(url, status);

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_EQ(status, 200);
        EXPECT_EQ(server.LastTarget(), "/latest");
    }

    TEST(PiHttpClient, InvalidUrlReturnsInvalidUrl)
    {
        BeastHttpClient client;
        std::string     body;
        int             status = 0;
        EXPECT_EQ(client.Get("not-a-url", body, status), DAS_E_INVALID_URL);
        EXPECT_EQ(
            client.Get("ftp://example.com/file", body, status),
            DAS_E_INVALID_URL);
    }
} // namespace
