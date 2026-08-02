#include "../src/PiHttpClient.h"

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
#include <cstdint>
#include <filesystem>
#include <fstream>
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

        std::uint16_t Port() const { return port_; }
        std::string   LastTarget() const { return last_target_; }
        int           RequestCount() const { return request_count_.load(); }
        void          SetBody(std::string body) { body_ = std::move(body); }

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

        boost::asio::io_context                         io_;
        std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
        std::thread                                     thread_;
        std::atomic<bool>                               running_{false};
        std::uint16_t                                   port_ = 0;
        std::string                                     body_;
        std::atomic<int>                                request_count_{0};
        std::string                                     last_target_;
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
    constexpr std::string_view kTestCertPem = R"(-----BEGIN CERTIFICATE-----
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

    constexpr std::string_view kTestKeyPem = R"(-----BEGIN PRIVATE KEY-----
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
            ctx_->use_certificate_chain(boost::asio::buffer(kTestCertPem));
            ctx_->use_private_key(
                boost::asio::buffer(kTestKeyPem),
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

    TEST(PiHttpClient, Socks5ProxyReturnsInvalidArgument)
    {
        BeastHttpClient client;
        std::string     body;
        int             status = 0;
        // socks5 is rejected up front with no network I/O.
        EXPECT_EQ(
            client.Get(
                "https://example.com/update",
                body,
                status,
                "socks5://127.0.0.1:1080"),
            DAS_E_INVALID_ARGUMENT);
    }

    TEST(PiHttpClient, MalformedProxyReturnsInvalidArgument)
    {
        BeastHttpClient client;
        std::string     body;
        int             status = 0;
        // Missing port must fail rather than silently connecting directly.
        EXPECT_EQ(
            client.Get(
                "https://example.com/update",
                body,
                status,
                "http://127.0.0.1"),
            DAS_E_INVALID_ARGUMENT);
        EXPECT_EQ(
            client.Get(
                "https://example.com/update",
                body,
                status,
                "ftp://127.0.0.1:21"),
            DAS_E_INVALID_ARGUMENT);
    }

    TEST(PiHttpClient, ReadManifestProxyUrlReadsTopLevelField)
    {
        const auto manifest =
            std::filesystem::temp_directory_path() / "DasMaaPi_proxy_test.json";

        const auto WriteManifest = [&](const std::string& content)
        {
            std::ofstream file(manifest, std::ios::binary);
            file << content;
        };

        WriteManifest(
            R"({"name":"libDasMaaPi","proxyUrl":"http://user:pass@proxy.example:8080"})");
        EXPECT_EQ(
            ReadManifestProxyUrl(manifest),
            "http://user:pass@proxy.example:8080");

        // Missing field -> empty string (direct connection).
        WriteManifest(R"({"name":"libDasMaaPi"})");
        EXPECT_EQ(ReadManifestProxyUrl(manifest), "");

        // Missing file -> empty string.
        std::filesystem::remove(manifest);
        EXPECT_EQ(ReadManifestProxyUrl(manifest), "");

        std::filesystem::remove(manifest);
    }
} // namespace
