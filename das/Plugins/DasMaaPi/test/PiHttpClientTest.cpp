#include "../src/PiHttpClient.h"

#include <das/Plugins/DasMaaPi/MaaPiErrorCodes.h>
#include <das/_autogen/idl/abi/DasResult.h>
#include <gtest/gtest.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

        std::uint16_t Port() const { return port_; }
        std::string   LastTarget() const
        {
            const std::lock_guard<std::mutex> lock(last_target_mutex_);
            return last_target_;
        }
        int  RequestCount() const { return request_count_.load(); }
        void SetBody(std::string body) { body_ = std::move(body); }
        // 无 Content-Length：以 Transfer-Encoding: chunked 响应（流式下载
        // 无 total_bytes 阶段态场景）。
        void SetChunkedResponse(bool chunked) { chunked_ = chunked; }
        // 慢速分块 chunked 响应：逐块写、块间 sleep（chunked + 分块延迟，
        // 驱动 body 循环超时重设回归测试）。非空即生效，优先于其它模式。
        void SetSlowChunks(
            std::vector<std::string>  chunks,
            std::chrono::milliseconds delay)
        {
            slow_chunks_ = std::move(chunks);
            chunk_delay_ = delay;
        }

    private:
        void AcceptLoop()
        {
            while (running_.load())
            {
                boost::system::error_code    ec;
                boost::asio::ip::tcp::socket sock = acceptor_->accept(ec);
                if (ec || !running_.load())
                {
                    break;
                }
                std::thread([this, s = std::move(sock)]() mutable
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
                {
                    const std::lock_guard<std::mutex> lock(last_target_mutex_);
                    last_target_ = std::string(req.target());
                }

                if (!slow_chunks_.empty())
                {
                    // 慢速分块：header 先行，随后逐块写、块间 sleep。
                    std::string raw = "HTTP/1.1 200 OK\r\n";
                    raw += "Content-Type: application/octet-stream\r\n";
                    raw += "Transfer-Encoding: chunked\r\n";
                    raw += "\r\n";
                    boost::asio::write(sock, boost::asio::buffer(raw));
                    for (const auto& chunk : slow_chunks_)
                    {
                        std::this_thread::sleep_for(chunk_delay_);
                        std::ostringstream hex;
                        hex << std::hex << chunk.size();
                        const std::string piece =
                            hex.str() + "\r\n" + chunk + "\r\n";
                        boost::asio::write(sock, boost::asio::buffer(piece));
                    }
                    boost::asio::write(sock, boost::asio::buffer("0\r\n\r\n"));
                    return;
                }

                if (chunked_)
                {
                    // 手动写一个 chunked 响应（无 Content-Length）：
                    // 单 chunk + 终止块。用于无 total_bytes 阶段态测试。
                    std::ostringstream hex;
                    hex << std::hex << body_.size();
                    std::string raw = "HTTP/1.1 200 OK\r\n";
                    raw += "Content-Type: application/octet-stream\r\n";
                    raw += "Transfer-Encoding: chunked\r\n";
                    raw += "\r\n";
                    raw += hex.str() + "\r\n";
                    raw += body_;
                    raw += "\r\n0\r\n\r\n";
                    boost::asio::write(sock, boost::asio::buffer(raw));
                    return;
                }

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

        boost::asio::io_context                         io_;
        std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
        std::thread                                     thread_;
        std::atomic<bool>                               running_{false};
        std::uint16_t                                   port_ = 0;
        std::string                                     body_;
        std::atomic<int>                                request_count_{0};
        mutable std::mutex                              last_target_mutex_;
        std::string                                     last_target_;
        bool                                            chunked_ = false;
        std::vector<std::string>                        slow_chunks_;
        std::chrono::milliseconds                       chunk_delay_{0};
    };

    // Mock server that answers /redirect with a 302 + Location header and every
    // other path with a canned 200 body. With loop_redirect set, every request
    // redirects to itself so tests can exercise the hop-limit guard.
    class LocalRedirectServer
    {
    public:
        ~LocalRedirectServer() { Stop(); }

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

        std::uint16_t Port() const { return port_; }
        int           RequestCount() const { return request_count_.load(); }
        void          SetLoopRedirect(bool loop) { loop_ = loop; }
        // 302 响应不补 body 定界头（无 Content-Length / 非 chunked，语义为
        // 读至连接关闭）——真实 GitHub release 302 的形态。
        void SetUndelimitedRedirect(bool undelimited)
        {
            undelimited_ = undelimited;
        }

    private:
        void AcceptLoop()
        {
            while (running_.load())
            {
                boost::system::error_code    ec;
                boost::asio::ip::tcp::socket sock = acceptor_->accept(ec);
                if (ec || !running_.load())
                {
                    break;
                }
                std::thread([this, s = std::move(sock)]() mutable
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

                boost::beast::http::response<boost::beast::http::string_body>
                    resp;
                resp.version(11);
                if (loop_ || req.target() == "/redirect")
                {
                    resp.result(boost::beast::http::status::found);
                    const std::string location =
                        loop_ ? std::string("/redirect")
                              : "http://127.0.0.1:" + std::to_string(port_)
                                    + "/final";
                    resp.set(boost::beast::http::field::location, location);
                    if (!undelimited_)
                    {
                        resp.prepare_payload();
                    }
                }
                else
                {
                    resp.result(boost::beast::http::status::ok);
                    resp.set(
                        boost::beast::http::field::content_type,
                        "application/json");
                    resp.body() = "{\"redirected\":true}";
                    resp.prepare_payload();
                }
                boost::beast::http::write(sock, resp);
            }
            catch (...)
            {
            }
        }

        boost::asio::io_context                         io_;
        std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
        std::thread                                     thread_;
        std::atomic<bool>                               running_{false};
        std::uint16_t                                   port_ = 0;
        std::atomic<int>                                request_count_{0};
        bool                                            loop_ = false;
        bool                                            undelimited_ = false;
    };

    TEST(PiHttpClient, GetReturnsBodyAndStatusFromLocalMock)
    {
        LocalHttpServer server;
        server.Start();

        BeastHttpClient client;
        std::string     body;
        int             status = 0;
        const auto url = "http://127.0.0.1:" + std::to_string(server.Port())
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

    // ---------------------------------------------------------------------
    // Mock CONNECT proxy + local TLS server fixtures (no external network).
    // ---------------------------------------------------------------------

    // Self-signed localhost certificate used by the mock TLS server. The client
    // under test disables verification (verify_tls_certificate=false) so no
    // trust store is needed.
    constexpr std::string_view TEST_CERT_PEM = R"(-----BEGIN CERTIFICATE-----
MIIDCTCCAfGgAwIBAgIUUEtKOtvIqhW/sJzTYsbeR2O7Ar8wDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDgwMjA2MDU1MFoXDTM2MDcz
MDA2MDU1MFowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEA23HiUPdPdPVX2WgB6iPheyn4yV+0v9VckopbsoslP7Kj
/4gNUNXoH9eOnLP5vBNlG5jSZXNcI4CLTx3l7OtLnDapz4UUrQ6ewzcYjAUY3L8g
6z1mWNTCbTi8SW97GasBGz/NHnHy/5qZfe+amb3p9/StPqMXz3LG+6oU9mMG8C8M
McHEcIbw3Enqlxc31UEGoQNN6JFWggg2rm+nF7tQq7xW3ICdWWdN83BpeqO8D0nY
Aaw7hYO3mpVOuIf8M09UG2AU5PqQwWS70HRDm7aPyDLJuOK17PFzAfmztnMxkSSI
viYGsQnpbl7M6ff/WshD7z81SgvBXr8FnUBlXcfkAQIDAQABo1MwUTAdBgNVHQ4E
FgQUJCIuahX7q+d+qTOzsTzj4L+QXg4wHwYDVR0jBBgwFoAUJCIuahX7q+d+qTOz
sTzj4L+QXg4wDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAQjXv
9ycJ1Htv0SerzWuXJjaXQo2nU1AZgT4+bedX7PVYg0zxitV23SDq7NH5uKdW+xMX
k+61QzYTtde99bX4PTO47VsfPVPZ1RUmHqLehh6+eAYTHg7WZ+co7yltucijGooU
L6dCF2hwGk4eCKO7Mp9YOcxe0H7TbN+m/5vsdHo4dpXEuA/1EafFuEAEdt+VUyLh
6YyOt0BWVWrbDowvxSgDqj+B3bRRjSSB/ZANRNurgDVZbaRDUcc4XDx6T2TG9wY6
OF8yP5NabVmGeplbxcNLp9JQUg/Sb6pIPNryW9uKyiUje/kkW5Z8dDSMaXoUEvAQ
rF0xis0f6gCD7h9gVw==
-----END CERTIFICATE-----
)";

    constexpr std::string_view TEST_KEY_PEM = R"(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDbceJQ90909VfZ
aAHqI+F7KfjJX7S/1VySiluyiyU/sqP/iA1Q1egf146cs/m8E2UbmNJlc1wjgItP
HeXs60ucNqnPhRStDp7DNxiMBRjcvyDrPWZY1MJtOLxJb3sZqwEbP80ecfL/mpl9
75qZven39K0+oxfPcsb7qhT2YwbwLwwxwcRwhvDcSeqXFzfVQQahA03okVaCCDau
b6cXu1CrvFbcgJ1ZZ03zcGl6o7wPSdgBrDuFg7ealU64h/wzT1QbYBTk+pDBZLvQ
dEObto/IMsm44rXs8XMB+bO2czGRJIi+JgaxCeluXszp9/9ayEPvPzVKC8FevwWd
QGVdx+QBAgMBAAECggEACDCx76+rSYQ2qSiWmbSJH92rKWjnCBnggJ+lWH6U/1Gd
iVX8mT3Ckh2rdvHIqlpW9ba4c4zgrXqIuIU3JaaKheQMZnBjGypmhoax5dfy25pO
kjkSbtJSB12MuXjtsUGOowO4liBOn3eXrvmYXZCXAOgi5ymTI+z2vEHW0nNTuPcW
3DB5DixbI0FMQ+OQX8q0yA+Atc5+J2NZTvQN393ufGv+5YPNfPRuBGRfsrfB0n0l
ezxqm5+CJgdD8ljEdnoRuI6I0IVlrEmp2lTmvxIUoVxjE1sU9VqtrQLk24xoKjlC
h5+6Pau/S6pCiK7CkBMheV2YoyhAyXLLQ78CaT+EOQKBgQD0FcupAU874JaRsl7R
8+QDle+pJ+W2evcrOeM0VHkiWkUPapjOp3dN5T5PmYgJAxuGXvFweaXfbHVgKWvx
MCAAHqJCSpKtPlHFRtvL/h6HF7VSL+RtaUnhK8DJc/niTzvIwATNDjePoGKJAvrZ
C7nfJleRsiSXh9/3Vzrx0lACDwKBgQDmKCv7VzUO0qDEkdjQs55CmMHtdZuC8cSR
ztU0mkB4K/JubhCFJN0PP/kn/7njyzCf4b7+9DN7FKYcBbhSQsNMLUb+cz2V4wM/
zOsJTmJFCExwp4mojBDAIRmH1RNmbRbuXpmZW9zsz0mxfO/VATiDtl9e22urcMQw
IIYq4iKI7wKBgBdQjaeTDHJ3vWolU5FSbpE5HS9XNEX98OYcHWu+NePmI3qZgAcd
T/QM1arcy3Ww6kwnfYB03jc9MOkCi+D569SXGICkhuYtcboaTpxppZjZ1uQG1xbs
W7Y6a6vSfCf10YwXa1364avCcicMNdHNoO+DxhGQppYi0G3b0zGOGT5ZAoGBAKtB
hwB9cnh2F7RMhdU9YexR+DL2COIVfdu02IZMpN4GbVzoPxijRm37fyzZJvuuDJ9a
/aB+GvRhd/xv8Sgcj29WXXYmT9QA2EEX/GevUIXnV+GXeGHW3vvnZTTiQo47bA2w
jIBFj40R7NbJjTekseWnxS0y/dnSjdj1yn+MRoA3AoGACnGmuteQQGSqsz0KI6ly
ZqQFe8b5jxGYB+m8SaBzQVD/ld7LuPQJUWVxCu1wrbgzrtWjo0wFq/qCi/mNkkjq
ljJciuuy/kBzz7/ufQGwq/PFwa3GCjiFNYX4iV4H7sYoKP/yQG7DoQ0Xal2Kwv/I
v6yQpUYtQbbVSIHbNlBhfp8=
-----END PRIVATE KEY-----
)";

    // Two-way byte relay over two connected sockets on a dedicated io_context.
    // Blocks the calling (per-connection) thread until either direction closes.
    // The incoming sockets belong to other io_contexts, so their native handles
    // are re-bound to this relay's io_context first — otherwise the async pumps
    // would register on the wrong (unpolled) IOCP and never run.
    class ByteRelay
    {
    public:
        ByteRelay(
            boost::asio::ip::tcp::socket a,
            boost::asio::ip::tcp::socket b)
        {
            boost::asio::ip::tcp::socket rebind_a(io_);
            boost::asio::ip::tcp::socket rebind_b(io_);
            rebind_a.assign(boost::asio::ip::tcp::v4(), a.release());
            rebind_b.assign(boost::asio::ip::tcp::v4(), b.release());
            auto sa = std::make_shared<boost::asio::ip::tcp::socket>(
                std::move(rebind_a));
            auto sb = std::make_shared<boost::asio::ip::tcp::socket>(
                std::move(rebind_b));
            Pump(sa, sb);
            Pump(sb, sa);
            io_.run();
        }

    private:
        void Pump(
            std::shared_ptr<boost::asio::ip::tcp::socket> from,
            std::shared_ptr<boost::asio::ip::tcp::socket> to)
        {
            auto buf = std::make_shared<std::array<char, 8192>>();
            from->async_read_some(
                boost::asio::buffer(*buf),
                [this, from, to, buf](
                    const boost::system::error_code& ec,
                    std::size_t                      n)
                {
                    if (ec)
                    {
                        boost::system::error_code ignored;
                        from->close(ignored);
                        to->close(ignored);
                        return;
                    }
                    boost::asio::async_write(
                        *to,
                        boost::asio::buffer(*buf, n),
                        [this, from, to, buf](
                            const boost::system::error_code& wec,
                            std::size_t)
                        {
                            if (wec)
                            {
                                boost::system::error_code ignored;
                                from->close(ignored);
                                to->close(ignored);
                                return;
                            }
                            Pump(from, to);
                        });
                });
        }

        boost::asio::io_context io_;
    };

    // Mock HTTP proxy: accepts, inspects the first request line, answers
    // CONNECT with 200 Connection Established, then relays raw bytes to the
    // target.
    class LocalConnectProxy
    {
    public:
        ~LocalConnectProxy() { Stop(); }

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

        void          SetTargetPort(std::uint16_t port) { target_port_ = port; }
        std::uint16_t Port() const { return port_; }
        bool          SawConnect() const { return saw_connect_.load(); }
        std::string   LastFirstLine() const { return last_first_line_; }

    private:
        void AcceptLoop()
        {
            while (running_.load())
            {
                boost::system::error_code    ec;
                boost::asio::ip::tcp::socket sock = acceptor_->accept(ec);
                if (ec || !running_.load())
                {
                    break;
                }
                std::thread([this, s = std::move(sock)]() mutable
                            { Handle(std::move(s)); })
                    .detach();
            }
        }

        void Handle(boost::asio::ip::tcp::socket client)
        {
            try
            {
                boost::asio::streambuf buf;
                boost::asio::read_until(client, buf, "\r\n\r\n");
                const auto begin = boost::asio::buffers_begin(buf.data());
                const std::string headers(begin, begin + buf.size());
                const auto        first_line_end = headers.find("\r\n");
                last_first_line_ = headers.substr(0, first_line_end);

                boost::asio::ip::tcp::socket target(relay_io_);
                target.connect(
                    boost::asio::ip::tcp::endpoint(
                        boost::asio::ip::address_v4::loopback(),
                        target_port_));

                const bool is_connect =
                    last_first_line_.rfind("CONNECT ", 0) == 0;
                if (is_connect)
                {
                    saw_connect_.store(true);
                    boost::asio::write(
                        client,
                        boost::asio::buffer(
                            "HTTP/1.1 200 Connection Established\r\n\r\n"));
                    // Proxy handshake bytes are not part of the tunnel.
                    buf.consume(buf.size());
                }
                else
                {
                    // absolute-form request: forward buffered bytes as-is.
                    const std::string pending(
                        boost::asio::buffers_begin(buf.data()),
                        boost::asio::buffers_begin(buf.data()) + buf.size());
                    boost::asio::write(target, boost::asio::buffer(pending));
                    buf.consume(buf.size());
                }

                ByteRelay relay(std::move(client), std::move(target));
            }
            catch (...)
            {
            }
        }

        boost::asio::io_context                         io_;
        boost::asio::io_context                         relay_io_;
        std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
        std::thread                                     thread_;
        std::atomic<bool>                               running_{false};
        std::uint16_t                                   port_ = 0;
        std::uint16_t                                   target_port_ = 0;
        std::atomic<bool>                               saw_connect_{false};
        std::string                                     last_first_line_;
    };

    // Local TLS (https) server using the embedded self-signed certificate.
    class LocalTlsServer
    {
    public:
        ~LocalTlsServer() { Stop(); }

        void Start()
        {
            acceptor_ = std::make_unique<boost::asio::ip::tcp::acceptor>(
                io_,
                boost::asio::ip::tcp::endpoint(
                    boost::asio::ip::address_v4::loopback(),
                    0));
            port_ = acceptor_->local_endpoint().port();
            ctx_ = std::make_unique<boost::asio::ssl::context>(
                boost::asio::ssl::context::tls_server);
            ctx_->use_certificate_chain(boost::asio::buffer(TEST_CERT_PEM));
            ctx_->use_private_key(
                boost::asio::buffer(TEST_KEY_PEM),
                boost::asio::ssl::context::pem);
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

        std::uint16_t Port() const { return port_; }
        std::string   LastTarget() const { return last_target_; }

    private:
        void AcceptLoop()
        {
            while (running_.load())
            {
                boost::system::error_code    ec;
                boost::asio::ip::tcp::socket sock = acceptor_->accept(ec);
                if (ec || !running_.load())
                {
                    break;
                }
                std::thread([this, s = std::move(sock)]() mutable
                            { Handle(std::move(s)); })
                    .detach();
            }
        }

        void Handle(boost::asio::ip::tcp::socket sock)
        {
            try
            {
                boost::asio::ssl::stream<boost::asio::ip::tcp::socket> tls(
                    std::move(sock),
                    *ctx_);
                tls.handshake(boost::asio::ssl::stream_base::server);
                boost::beast::flat_buffer buffer;
                boost::beast::http::request<boost::beast::http::string_body>
                    req;
                boost::beast::http::read(tls, buffer, req);
                last_target_ = std::string(req.target());

                boost::beast::http::response<boost::beast::http::string_body>
                    resp;
                resp.result(boost::beast::http::status::ok);
                resp.version(11);
                resp.set(
                    boost::beast::http::field::content_type,
                    "application/json");
                resp.body() = "{\"secure\":true}";
                resp.prepare_payload();
                boost::beast::http::write(tls, resp);
            }
            catch (...)
            {
            }
        }

        boost::asio::io_context                         io_;
        std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
        std::unique_ptr<boost::asio::ssl::context>      ctx_;
        std::thread                                     thread_;
        std::atomic<bool>                               running_{false};
        std::uint16_t                                   port_ = 0;
        std::string                                     last_target_;
    };

    TEST(PiHttpClient, HttpsTargetThroughProxyUsesConnectTunnel)
    {
        LocalTlsServer tls;
        tls.Start();

        LocalConnectProxy proxy;
        proxy.SetTargetPort(tls.Port());
        proxy.Start();

        // TLS verification disabled so the embedded self-signed cert is
        // accepted.
        BeastHttpClient client{/*verify_tls_certificate=*/false};
        std::string     body;
        int             status = 0;
        const auto      url = "https://127.0.0.1:" + std::to_string(tls.Port())
                              + "/releases/latest";
        const auto      proxy_url =
            "http://127.0.0.1:" + std::to_string(proxy.Port());
        const auto result = client.Get(url, body, status, proxy_url);

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_EQ(status, 200);
        EXPECT_EQ(body, "{\"secure\":true}");
        EXPECT_TRUE(proxy.SawConnect());
        EXPECT_EQ(tls.LastTarget(), "/releases/latest");
    }

    TEST(PiHttpClient, HttpTargetThroughProxyUsesAbsoluteForm)
    {
        LocalHttpServer server;
        server.Start();

        LocalConnectProxy proxy;
        proxy.SetTargetPort(server.Port());
        proxy.Start();

        BeastHttpClient client;
        std::string     body;
        int             status = 0;
        const auto      url =
            "http://127.0.0.1:" + std::to_string(server.Port()) + "/latest";
        const auto proxy_url =
            "http://127.0.0.1:" + std::to_string(proxy.Port());
        const auto result = client.Get(url, body, status, proxy_url);

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_EQ(status, 200);
        EXPECT_EQ(body, "{\"ok\":true}");
        EXPECT_FALSE(proxy.SawConnect());
        // RFC 7230 5.3.2: absolute-form request line reaches the target.
        EXPECT_EQ(server.LastTarget(), url);
    }

    TEST(PiHttpClient, Socks5ProxyReturnsProxyUnsupported)
    {
        BeastHttpClient client;
        std::string     body;
        int             status = 0;
        // socks5 is rejected up front with a distinct code so callers can tell
        // proxy-target rejection from a structurally malformed URL.
        EXPECT_EQ(
            client.Get(
                "https://example.com/update",
                body,
                status,
                "socks5://127.0.0.1:1080"),
            DAS_E_MAAPI_PROXY_UNSUPPORTED);
    }

    TEST(PiHttpClient, MissingPortProxyReturnsInvalidArgument)
    {
        BeastHttpClient client;
        std::string     body;
        int             status = 0;
        // Missing port is a structural URL error, not a proxy-scheme decision,
        // and must fail rather than silently connecting directly.
        EXPECT_EQ(
            client.Get(
                "https://example.com/update",
                body,
                status,
                "http://127.0.0.1"),
            DAS_E_INVALID_ARGUMENT);
    }

    // Direct ParseProxyUrl unit tests (no network I/O). These pin the contract
    // that proxy-scheme rejection (missing scheme / socks5 / non-http scheme)
    // returns DAS_E_MAAPI_PROXY_UNSUPPORTED, while structural URL errors
    // (empty host / port) keep returning DAS_E_INVALID_ARGUMENT.

    TEST(PiHttpClient, ParseProxyUrl_Socks5ReturnsProxyUnsupported)
    {
        ProxyTarget out;
        EXPECT_EQ(
            ParseProxyUrl("socks5://127.0.0.1:1080", out),
            DAS_E_MAAPI_PROXY_UNSUPPORTED);
    }

    TEST(PiHttpClient, ParseProxyUrl_UnsupportedSchemeReturnsProxyUnsupported)
    {
        ProxyTarget out;
        EXPECT_EQ(
            ParseProxyUrl("ftp://example.com:21", out),
            DAS_E_MAAPI_PROXY_UNSUPPORTED);
    }

    TEST(PiHttpClient, ParseProxyUrl_MissingSchemeReturnsProxyUnsupported)
    {
        ProxyTarget out;
        // No "://" — treated as a proxy URL missing its scheme, not a generic
        // malformed URL, so callers can distinguish the two failure modes.
        EXPECT_EQ(
            ParseProxyUrl("example.com:8080", out),
            DAS_E_MAAPI_PROXY_UNSUPPORTED);
    }

    TEST(PiHttpClient, ParseProxyUrl_HostPortEmptyReturnsInvalidArgument)
    {
        // Regression guard: structural URL errors (empty host/port) stay on
        // DAS_E_INVALID_ARGUMENT — proxy-scheme rejection does not subsume
        // them.
        ProxyTarget out;
        EXPECT_EQ(
            ParseProxyUrl("http://127.0.0.1", out),
            DAS_E_INVALID_ARGUMENT);
    }

    TEST(PiHttpClient, ParseProxyUrl_HttpStillAccepted)
    {
        ProxyTarget out;
        EXPECT_EQ(ParseProxyUrl("http://proxy.local:8080", out), DAS_S_OK);
        EXPECT_EQ(out.host, "proxy.local");
        EXPECT_EQ(out.port, 8080);
        EXPECT_EQ(out.scheme, "http");
    }

    TEST(PiHttpClient, GetFollowsRedirectToLocation)
    {
        // CR-01: GitHub release download URLs 302 to another host; the client
        // must follow the Location header and return the final 200 body.
        LocalRedirectServer server;
        server.Start();

        BeastHttpClient client;
        std::string     body;
        int             status = 0;
        const auto      url =
            "http://127.0.0.1:" + std::to_string(server.Port()) + "/redirect";
        const auto result = client.Get(url, body, status);

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_EQ(status, 200);
        EXPECT_EQ(body, "{\"redirected\":true}");
        // One 302 hop + one 200 final.
        EXPECT_EQ(server.RequestCount(), 2);
    }

    TEST(PiHttpClient, HeadFollowsRedirectToLocation)
    {
        LocalRedirectServer server;
        server.Start();

        BeastHttpClient client;
        int             status = 0;
        const auto      url =
            "http://127.0.0.1:" + std::to_string(server.Port()) + "/redirect";
        const auto result = client.Head(url, status);

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_EQ(status, 200);
        EXPECT_EQ(server.RequestCount(), 2);
    }

    TEST(PiHttpClient, RedirectLoopStopsAfterMaxHops)
    {
        // A server that redirects to itself must not loop forever; the client
        // gives up at the hop limit and surfaces the 3xx status.
        LocalRedirectServer server;
        server.SetLoopRedirect(true);
        server.Start();

        BeastHttpClient client;
        std::string     body;
        int             status = 0;
        const auto      url =
            "http://127.0.0.1:" + std::to_string(server.Port()) + "/redirect";
        const auto result = client.Get(url, body, status);

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_EQ(status, 302);
        // Initial request + 5 followed hops.
        EXPECT_EQ(server.RequestCount(), 6);
    }

    // ---------------------------------------------------------------------
    // 流式下载 DownloadStream：有/无 Content-Length、写盘、逐块进度回调。
    // ---------------------------------------------------------------------

    TEST(PiHttpClient, DownloadStreamWithContentLengthReportsChunkedProgress)
    {
        // 200KB body > 64KB 客户端缓冲 → 多块回调（Test 1：逐块 + total）。
        const std::string body(200 * 1024, 'x');
        LocalHttpServer   server(body);
        server.Start();

        BeastHttpClient client;
        const auto      out_path = std::filesystem::temp_directory_path()
                                   / "das_pi_download_cl_test.bin";
        std::error_code ec;
        std::filesystem::remove(out_path, ec);

        std::vector<DownloadProgress> progress;
        int                           status = 0;
        const auto url = "http://127.0.0.1:" + std::to_string(server.Port())
                         + "/package.zip";
        const auto result = client.DownloadStream(
            url,
            out_path,
            [&](const DownloadProgress& p) { progress.push_back(p); },
            status);

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_EQ(status, 200);
        // 多块回调（64KB 缓冲 → 200KB body 至少 3 次）。
        EXPECT_GE(progress.size(), 3u);
        ASSERT_FALSE(progress.empty());
        const auto& last = progress.back();
        EXPECT_EQ(last.bytes_received, static_cast<std::uint64_t>(body.size()));
        ASSERT_TRUE(last.total_bytes.has_value());
        EXPECT_EQ(*last.total_bytes, static_cast<std::uint64_t>(body.size()));
        // Test 3：写盘字节与响应体一致。
        std::ifstream ifs(out_path, std::ios::binary);
        std::string   content(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
        EXPECT_EQ(content, body);
        std::filesystem::remove(out_path, ec);
    }

    TEST(PiHttpClient, DownloadStreamWithoutContentLengthReportsStageProgress)
    {
        // Test 2：无 Content-Length（chunked）→ total_bytes 空（阶段态），
        // bytes_received 递增，写盘字节一致。
        LocalHttpServer server("chunked-package-bytes");
        server.SetChunkedResponse(true);
        server.Start();

        BeastHttpClient client;
        const auto      out_path = std::filesystem::temp_directory_path()
                                   / "das_pi_download_nocl_test.bin";
        std::error_code ec;
        std::filesystem::remove(out_path, ec);

        std::vector<DownloadProgress> progress;
        int                           status = 0;
        const auto url = "http://127.0.0.1:" + std::to_string(server.Port())
                         + "/package.zip";
        const auto result = client.DownloadStream(
            url,
            out_path,
            [&](const DownloadProgress& p) { progress.push_back(p); },
            status);

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_EQ(status, 200);
        ASSERT_FALSE(progress.empty());
        for (const auto& p : progress)
        {
            EXPECT_FALSE(p.total_bytes.has_value());
        }
        EXPECT_GT(progress.back().bytes_received, 0u);
        std::ifstream ifs(out_path, std::ios::binary);
        std::string   content(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
        EXPECT_EQ(content, "chunked-package-bytes");
        std::filesystem::remove(out_path, ec);
    }

    TEST(
        PiHttpClient,
        DownloadStreamSlowChunkedBodySurvivesTotalDurationBeyondTimeout)
    {
        // 流式下载 body 循环每次读前重设超时（滑动窗口 stall 检测）：分块
        // 间延迟累计（5×500ms = 2.5s）远超单次 per-op 超时（1s）仍成功。
        // 若 body 循环只依赖 read_header 前布防的绝对截止（不重设），第二
        // 块起读必超时失败——真实网络大包回归的本地锁定。
        // timeout 用短值：默认 15s 下等价测试需真实睡 >15s，太慢。
        std::vector<std::string> chunks;
        for (int i = 0; i < 5; ++i)
        {
            chunks.emplace_back(16 * 1024, 'x');
        }
        LocalHttpServer server;
        server.SetSlowChunks(chunks, std::chrono::milliseconds(500));
        server.Start();

        BeastHttpClient client{/*verify_tls_certificate=*/true,
                               std::chrono::milliseconds(1000)};
        const auto      out_path = std::filesystem::temp_directory_path()
                                   / "das_pi_download_slow_test.bin";
        std::error_code ec;
        std::filesystem::remove(out_path, ec);

        int        status = 0;
        const auto url = "http://127.0.0.1:" + std::to_string(server.Port())
                         + "/package.zip";
        const auto result = client.DownloadStream(
            url,
            out_path,
            [](const DownloadProgress&) {},
            status);

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_EQ(status, 200);
        std::ifstream ifs(out_path, std::ios::binary);
        std::string   content(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
        EXPECT_EQ(content.size(), 5u * 16 * 1024);
        std::filesystem::remove(out_path, ec);
    }

    TEST(PiHttpClient, DownloadStreamNon200WritesNoBody)
    {
        // 非 200 响应：不写盘体（HTTP 层无 body 落盘），状态透传。
        // LocalRedirectServer 对 /redirect 返回 302 + Location；DownloadStream
        // 跟随跳转到 /final 拿到 200 体。
        LocalRedirectServer server;
        server.Start();

        BeastHttpClient client;
        const auto      out_path = std::filesystem::temp_directory_path()
                                   / "das_pi_download_redirect_test.bin";
        std::error_code ec;
        std::filesystem::remove(out_path, ec);

        std::vector<DownloadProgress> progress;
        int                           status = 0;
        const auto                    url =
            "http://127.0.0.1:" + std::to_string(server.Port()) + "/redirect";
        const auto result = client.DownloadStream(
            url,
            out_path,
            [&](const DownloadProgress& p) { progress.push_back(p); },
            status);

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_EQ(status, 200);
        EXPECT_EQ(server.RequestCount(), 2);
        ASSERT_FALSE(progress.empty());
        std::ifstream ifs(out_path, std::ios::binary);
        std::string   content(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
        EXPECT_EQ(content, "{\"redirected\":true}");
        std::filesystem::remove(out_path, ec);
    }

    // 302 无 body 定界（无 Content-Length / 非 chunked，HTTP 语义为读至
    // 连接关闭，而 keep-alive 连接不关）：真实 GitHub release 下载 302 的
    // 实测形态。客户端读到 3xx 必须跳过 body 直接换发下一跳——旧实现挂
    // 在 body 读取上直到 15s 超时（生产表现为下载零进度后超时失败）。
    // 断言 5 秒内有界完成（远小于 15s per-op 超时）。
    TEST(PiHttpClient, DownloadStreamSkipsUndelimitedRedirectBody)
    {
        LocalRedirectServer server;
        server.SetUndelimitedRedirect(true);
        server.Start();

        BeastHttpClient client;
        const auto      out_path = std::filesystem::temp_directory_path()
                                   / "das_pi_download_undelimited_test.bin";
        std::error_code ec;
        std::filesystem::remove(out_path, ec);

        const auto start = std::chrono::steady_clock::now();
        int        status = 0;
        const auto url =
            "http://127.0.0.1:" + std::to_string(server.Port()) + "/redirect";
        const auto result = client.DownloadStream(
            url,
            out_path,
            [](const DownloadProgress&) {},
            status);
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start);

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_EQ(status, 200);
        EXPECT_EQ(server.RequestCount(), 2);
        EXPECT_LT(elapsed.count(), 5);
        std::ifstream ifs(out_path, std::ios::binary);
        std::string   content(
            (std::istreambuf_iterator<char>(ifs)),
            std::istreambuf_iterator<char>());
        EXPECT_EQ(content, "{\"redirected\":true}");
        std::filesystem::remove(out_path, ec);
    }

    // ---------------------------------------------------------------------
    // RequestCancel：挂起连接上取消在途请求。断言 5 秒内有界返回（远小于
    // 15s per-op 超时——等待超时兜底即失败），并以 ASAN 干净退出验证被放弃
    // 协程帧随 io_context 析构的销毁安全。
    // ---------------------------------------------------------------------

    // 接受连接后不写任何字节的服务器：客户端阻塞在响应读取上，直到被取消
    // 或 per-op 超时。取消用例的驱动源。
    class HangingResponseServer
    {
    public:
        ~HangingResponseServer() { Stop(); }

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
            // 关闭所有挂起连接：仍阻塞的客户端读会以 EOF 结束。
            const std::lock_guard<std::mutex> parked_lock(parked_mutex_);
            parked_.clear();
        }

        std::uint16_t Port() const { return port_; }
        int           AcceptedCount() const { return accepted_count_.load(); }

    private:
        void AcceptLoop()
        {
            while (running_.load())
            {
                boost::system::error_code    ec;
                boost::asio::ip::tcp::socket sock = acceptor_->accept(ec);
                if (ec || !running_.load())
                {
                    break;
                }
                {
                    const std::lock_guard<std::mutex> parked_lock(
                        parked_mutex_);
                    parked_.push_back(std::move(sock));
                }
                accepted_count_.fetch_add(1);
            }
        }

        boost::asio::io_context                         io_;
        std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
        std::thread                                     thread_;
        std::atomic<bool>                               running_{false};
        std::uint16_t                                   port_ = 0;
        std::mutex                                      parked_mutex_;
        std::vector<boost::asio::ip::tcp::socket>       parked_;
        std::atomic<int>                                accepted_count_{0};
    };

    // 有界轮询等待谓词为真；超时返回 false，由调用方断言。
    bool WaitFor(
        const std::function<bool()>& predicate,
        std::chrono::milliseconds    timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return predicate();
    }

    TEST(PiHttpClient, RequestCancelInterruptsHangingDownloadStream)
    {
        HangingResponseServer server;
        server.Start();

        BeastHttpClient client;
        const auto      out_path = std::filesystem::temp_directory_path()
                                   / "das_pi_download_cancel_test.bin";
        std::error_code ec;
        std::filesystem::remove(out_path, ec);

        std::atomic<bool> request_returned{false};
        DasResult         result = DAS_S_OK;
        int               status = 0;
        const auto  url = "http://127.0.0.1:" + std::to_string(server.Port())
                          + "/package.zip";
        std::thread request_thread(
            [&]()
            {
                result = client.DownloadStream(
                    url,
                    out_path,
                    [](const DownloadProgress&) {},
                    status);
                request_returned.store(true);
            });

        // 服务器收到连接即证明客户端已登记在途状态并阻塞在响应读取上。
        ASSERT_TRUE(WaitFor(
            [&] { return server.AcceptedCount() >= 1; },
            std::chrono::seconds(5)));
        client.RequestCancel();

        // 5 秒内有界返回（远小于 15s per-op 超时——等待超时兜底即失败）。
        const bool returned_within_bound = WaitFor(
            [&] { return request_returned.load(); },
            std::chrono::seconds(5));
        request_thread.join();
        EXPECT_TRUE(returned_within_bound);
        // 协程被放弃，outcome 保持默认失败码。
        EXPECT_EQ(result, DAS_E_FAIL);

        // 取消后客户端正常析构、进程正常退出（ASAN 干净）即本用例收尾。
        std::filesystem::remove(out_path, ec);
    }

    TEST(PiHttpClient, RequestCancelInterruptsHangingGet)
    {
        // Get/Head 路径与流式下载共用同一登记/取消机制：挂起 GET 被
        // RequestCancel 有界打断。
        HangingResponseServer server;
        server.Start();

        BeastHttpClient client;
        std::string     body;
        int             status = 0;
        const auto url = "http://127.0.0.1:" + std::to_string(server.Port())
                         + "/releases/latest";

        std::atomic<bool> request_returned{false};
        DasResult         result = DAS_S_OK;
        std::thread       request_thread(
            [&]()
            {
                result = client.Get(url, body, status);
                request_returned.store(true);
            });

        ASSERT_TRUE(WaitFor(
            [&] { return server.AcceptedCount() >= 1; },
            std::chrono::seconds(5)));
        client.RequestCancel();

        const bool returned_within_bound = WaitFor(
            [&] { return request_returned.load(); },
            std::chrono::seconds(5));
        request_thread.join();
        EXPECT_TRUE(returned_within_bound);
        // 协程被放弃，outcome 保持默认失败码。
        EXPECT_EQ(result, DAS_E_FAIL);
    }
} // namespace
