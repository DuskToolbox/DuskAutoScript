#include <das/Core/IPC/HttpIpcServer.h>

#include <atomic>
#include <boost/asio/socket_base.hpp>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/fmt.h>
#include <stdexcept>

DAS_CORE_IPC_NS_BEGIN

using tcp = boost::asio::ip::tcp;
namespace websocket = boost::beast::websocket;

static constexpr const char* kUpgradePath = "/ipc/v1/transport";

struct HttpIpcServer::Impl
{
    boost::asio::io_context& io_context;
    tcp::acceptor            acceptor;
    Config                   config;
    OnHostConnected          on_connected;
    std::atomic<bool>        running{false};

    Impl(boost::asio::io_context& ioc, Config cfg, OnHostConnected cb)
        : io_context(ioc), acceptor(ioc), config(std::move(cfg)),
          on_connected(std::move(cb))
    {
        boost::beast::error_code ec;

        acceptor.open(tcp::v4(), ec);
        if (ec)
        {
            throw std::runtime_error(
                DAS_FMT_NS::format(
                    "Failed to open acceptor: {}",
                    ec.message()));
        }

        acceptor.set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (ec)
        {
            throw std::runtime_error(
                DAS_FMT_NS::format(
                    "Failed to set reuse_address: {}",
                    ec.message()));
        }

        auto endpoint = tcp::endpoint(
            boost::asio::ip::make_address(config.listen_address),
            config.listen_port);

        acceptor.bind(endpoint, ec);
        if (ec)
        {
            throw std::runtime_error(
                DAS_FMT_NS::format(
                    "Failed to bind to {}:{}: {}",
                    config.listen_address,
                    config.listen_port,
                    ec.message()));
        }

        acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec)
        {
            throw std::runtime_error(
                DAS_FMT_NS::format("Failed to listen: {}", ec.message()));
        }

        running.store(true);
        DoAccept();

        DAS_CORE_LOG_INFO(
            "HttpIpcServer listening on {}:{}",
            config.listen_address,
            config.listen_port);
    }

    void DoAccept()
    {
        acceptor.async_accept(
            [this](boost::beast::error_code ec, tcp::socket socket)
            {
                if (ec)
                {
                    if (running.load())
                    {
                        DAS_CORE_LOG_ERROR("Accept error: {}", ec.message());
                    }
                    return;
                }

                if (!running.load())
                {
                    return;
                }

                // Read HTTP header to detect WebSocket upgrade
                auto buffer = std::make_shared<boost::beast::flat_buffer>();
                auto parser =
                    std::make_shared<boost::beast::http::request_parser<
                        boost::beast::http::string_body>>();

                auto self = this;

                boost::beast::http::async_read_header(
                    socket,
                    *buffer,
                    *parser,
                    [self, socket = std::move(socket), buffer, parser](
                        boost::beast::error_code ec,
                        std::size_t) mutable
                    {
                        if (ec)
                        {
                            return;
                        }

                        auto& req = parser->get();

                        if (websocket::is_upgrade(req)
                            && req.target() == kUpgradePath)
                        {
                            self->UpgradeWebSocket(std::move(socket), req);
                        }
                        // Non-matching requests are dropped
                    });

                DoAccept();
            });
    }

    template <typename Body, typename Allocator>
    void UpgradeWebSocket(
        tcp::socket                                   socket,
        boost::beast::http::request<Body, Allocator>& req)
    {
        auto ws =
            std::make_unique<websocket::stream<tcp::socket>>(std::move(socket));

        ws->set_option(
            websocket::stream_base::timeout::suggested(
                boost::beast::role_type::server));

        auto self = this;

        ws->async_accept(
            req,
            [self, ws = std::move(ws)](boost::beast::error_code ec) mutable
            {
                if (ec)
                {
                    DAS_CORE_LOG_ERROR(
                        "WebSocket accept failed: {}",
                        ec.message());
                    return;
                }

                auto endpoint_name = fmt::format(
                    "{}:{}",
                    self->config.listen_address,
                    self->config.listen_port);

                DAS_CORE_LOG_INFO("Host connected: endpoint = {}", endpoint_name);

                if (self->on_connected)
                {
                    self->on_connected(
                        std::move(ws),
                        endpoint_name);
                }
            });
    }

    void Stop()
    {
        running.store(false);
        boost::beast::error_code ec;
        acceptor.close(ec);
        if (ec)
        {
            DAS_CORE_LOG_ERROR("Error closing acceptor: {}", ec.message());
        }
    }
};

HttpIpcServer::HttpIpcServer(
    boost::asio::io_context& io_context,
    Config                   config,
    OnHostConnected          on_connected)
    : impl_(
          std::make_unique<Impl>(
              io_context,
              std::move(config),
              std::move(on_connected)))
{
}

HttpIpcServer::~HttpIpcServer() = default;

void HttpIpcServer::Stop() { impl_->Stop(); }

uint16_t HttpIpcServer::GetPort() const { return impl_->config.listen_port; }

bool HttpIpcServer::IsRunning() const { return impl_->running.load(); }

DAS_CORE_IPC_NS_END
