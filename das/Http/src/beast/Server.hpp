#ifndef DAS_HTTP_BEAST_SERVER_HPP
#define DAS_HTTP_BEAST_SERVER_HPP

#include "Request.hpp"
#include "Router.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/config.hpp>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward declare NotificationHub
namespace Das::Http
{
    class NotificationHub;
}

namespace Das::Http::Beast
{

    namespace http = boost::beast::http;
    using tcp = boost::asio::ip::tcp;

    // 检测会话，处理单个HTTP连接
    class Session : public std::enable_shared_from_this<Session>
    {
        tcp::socket                       socket_;
        http::request<http::string_body>  request_;
        http::response<http::string_body> response_;
        std::shared_ptr<Router>           router_;
        std::function<bool()>             stop_condition_;

    public:
        Session(
            tcp::socket             socket,
            std::shared_ptr<Router> router,
            std::function<bool()>   stop_condition)
            : socket_(std::move(socket)), router_(std::move(router)),
              stop_condition_(std::move(stop_condition))
        {
        }

        void Run() { DoRead(); }

        /**
         * @brief 使用已读取的请求启动会话（由 UpgradeDetector 调用）。
         *
         * 当 UpgradeDetector 已从 socket 读取 HTTP 头并判定为普通 HTTP
         * 请求时，直接复用已读取的 parser，避免重复读取。
         */
        void RunWithParser(http::request<http::string_body> request)
        {
            request_ = std::move(request);
            ProcessRequest();
        }

    private:
        void DoRead()
        {
            auto self = shared_from_this();

            http::async_read(
                socket_,
                buffer_,
                request_,
                [self](
                    boost::beast::error_code ec,
                    std::size_t              bytes_transferred)
                {
                    boost::ignore_unused(bytes_transferred);
                    if (ec)
                    {
                        return;
                    }
                    self->ProcessRequest();
                });
        }

        void ProcessRequest()
        {
            HttpRequest  req(std::move(request_));
            HttpResponse http_response = router_->Handle(req);
            response_ = http_response.Release();

            DoWrite();
        }

        void DoWrite()
        {
            auto self = shared_from_this();

            http::async_write(
                socket_,
                response_,
                [self](
                    boost::beast::error_code ec,
                    std::size_t              bytes_transferred)
                {
                    boost::ignore_unused(bytes_transferred);
                    if (ec)
                    {
                        return;
                    }

                    // 关闭连接
                    boost::beast::error_code shutdown_ec;
                    self->socket_.shutdown(
                        tcp::socket::shutdown_send,
                        shutdown_ec);
                });
        }

    private:
        boost::beast::flat_buffer buffer_;
    };

    // ── WebSocket 会话 ──

    class WsSession : public std::enable_shared_from_this<WsSession>
    {
        boost::beast::websocket::stream<tcp::socket> ws_;
        std::shared_ptr<NotificationHub>             hub_;
        std::mutex                                   write_mutex_;

    public:
        WsSession(tcp::socket socket, std::shared_ptr<NotificationHub> hub)
            : ws_(std::move(socket)), hub_(std::move(hub))
        {
        }

        /**
         * @brief 获取 executor（用于 dispatch WebSocket 写操作）。
         */
        boost::asio::any_io_executor GetExecutor()
        {
            return ws_.get_executor();
        }

        /**
         * @brief 执行 WebSocket 握手并启动读写循环。
         *
         * 必须在 Listener 接受连接、检测到 Upgrade 请求后调用。
         *
         * @param req 原始的 HTTP Upgrade 请求。
         */
        template <typename Body, typename Allocator>
        void DoHandshake(boost::beast::http::request<Body, Allocator>& req)
        {
            auto self = shared_from_this();

            ws_.set_option(
                boost::beast::websocket::stream_base::timeout::suggested(
                    boost::beast::role_type::server));

            ws_.async_accept(
                req,
                [self](boost::beast::error_code ec)
                {
                    if (ec)
                    {
                        return;
                    }

                    // 注册到 NotificationHub
                    self->hub_->Register(self);

                    // 启动读循环
                    self->DoReadLoop();
                });
        }

        /**
         * @brief 向客户端发送文本消息。
         *
         * 由 NotificationHub::Broadcast 通过 dispatch 调用。
         *
         * @param message 共享消息，避免拷贝。
         */
        void WriteMessage(std::shared_ptr<const std::string> message)
        {
            // Beast websocket::stream 不允许并发写
            std::lock_guard<std::mutex> lock(write_mutex_);

            auto self = shared_from_this();
            ws_.async_write(
                boost::asio::buffer(*message),
                [self,
                 message](boost::beast::error_code ec, std::size_t /*bytes*/)
                { boost::ignore_unused(ec); });
        }

    private:
        void DoReadLoop()
        {
            auto self = shared_from_this();

            ws_.async_read(
                read_buffer_,
                [self](boost::beast::error_code ec, std::size_t bytes_read)
                {
                    boost::ignore_unused(bytes_read);

                    if (ec == boost::beast::websocket::error::closed)
                    {
                        self->hub_->Unregister(self.get());
                        return;
                    }

                    if (ec)
                    {
                        self->hub_->Unregister(self.get());
                        return;
                    }

                    // 继续读取（客户端可发送 ping/subscribe 等消息）
                    self->DoReadLoop();
                });
        }

        boost::beast::flat_buffer read_buffer_;
    };

    // ── WebSocket/HTTP 升级检测器 ──

    /**
     * @brief 检测 HTTP 请求是否为 WebSocket 升级请求。
     *
     * 先读取 HTTP 请求头，检查是否为 Upgrade: websocket。
     * - 是 WebSocket → 创建 WsSession 并执行握手
     * - 否 → 创建普通 HTTP Session 并处理
     */
    class UpgradeDetector : public std::enable_shared_from_this<UpgradeDetector>
    {
        tcp::socket                      socket_;
        std::shared_ptr<Router>          router_;
        std::function<bool()>            stop_condition_;
        std::shared_ptr<NotificationHub> hub_;
        boost::beast::flat_buffer        buffer_;

    public:
        UpgradeDetector(
            tcp::socket                      socket,
            std::shared_ptr<Router>          router,
            std::function<bool()>            stop_condition,
            std::shared_ptr<NotificationHub> hub)
            : socket_(std::move(socket)), router_(std::move(router)),
              stop_condition_(std::move(stop_condition)), hub_(std::move(hub))
        {
        }

        void Detect()
        {
            // Read only the HTTP header — use request_parser
            auto self = shared_from_this();

            http::async_read_header(
                socket_,
                buffer_,
                parser_,
                [self](boost::beast::error_code ec, std::size_t)
                {
                    if (ec)
                    {
                        return;
                    }

                    auto& req = self->parser_.get();
                    if (boost::beast::websocket::is_upgrade(req)
                        && req.target() == "/ws")
                    {
                        // WebSocket upgrade — create WsSession
                        auto ws = std::make_shared<WsSession>(
                            std::move(self->socket_),
                            self->hub_);
                        ws->DoHandshake(req);
                    }
                    else
                    {
                        // Normal HTTP — create Session with pre-read data
                        auto session = std::make_shared<Session>(
                            std::move(self->socket_),
                            self->router_,
                            self->stop_condition_);
                        session->RunWithParser(self->parser_.release());
                    }
                });
        }

    private:
        http::request_parser<http::string_body> parser_;
    };

    // 监听器，接受传入的连接
    class Listener : public std::enable_shared_from_this<Listener>
    {
        tcp::acceptor                    acceptor_;
        std::shared_ptr<Router>          router_;
        std::shared_ptr<NotificationHub> hub_;
        std::function<bool()>            stop_condition_;

    public:
        Listener(
            boost::asio::io_context&         ioc,
            tcp::endpoint                    endpoint,
            std::shared_ptr<Router>          router,
            std::function<bool()>            stop_condition,
            std::shared_ptr<NotificationHub> hub)
            : acceptor_(boost::asio::make_strand(ioc)),
              router_(std::move(router)), hub_(std::move(hub)),
              stop_condition_(std::move(stop_condition))
        {
            boost::beast::error_code ec;

            acceptor_.open(endpoint.protocol(), ec);
            if (ec)
            {
                return;
            }

            acceptor_.set_option(
                boost::asio::socket_base::reuse_address(true),
                ec);
            if (ec)
            {
                return;
            }

            acceptor_.bind(endpoint, ec);
            if (ec)
            {
                return;
            }

            acceptor_.listen(
                boost::asio::socket_base::max_listen_connections,
                ec);
            if (ec)
            {
                return;
            }
        }

        void Run() { DoAccept(); }

        /**
         * @brief 设置 NotificationHub（延迟注入，解决初始化顺序问题）。
         */
        void SetHub(std::shared_ptr<NotificationHub> hub)
        {
            hub_ = std::move(hub);
        }

    private:
        void DoAccept()
        {
            acceptor_.async_accept(
                boost::asio::make_strand(acceptor_.get_executor()),
                [self = shared_from_this()](
                    boost::beast::error_code ec,
                    tcp::socket              socket)
                {
                    if (ec)
                    {
                        return;
                    }

                    // Use request_parser to peek at headers before
                    // deciding HTTP vs WebSocket
                    auto detector = std::make_shared<UpgradeDetector>(
                        std::move(socket),
                        self->router_,
                        self->stop_condition_,
                        self->hub_);
                    detector->Detect();

                    // Continue accepting
                    self->DoAccept();
                });
        }
    };

    // HTTP服务器
    class Server
    {
    public:
        Server(
            const std::string&               address,
            unsigned short                   port,
            std::shared_ptr<Router>          router,
            std::function<bool()>            stop_condition,
            std::shared_ptr<NotificationHub> hub = nullptr)
            : router_(std::move(router)), hub_(std::move(hub)),
              stop_condition_(std::move(stop_condition)), ioc_(4)
        {
            auto const addr = boost::asio::ip::make_address(address);
            auto const endpoint = tcp::endpoint{addr, port};

            listener_ = std::make_shared<Listener>(
                ioc_,
                endpoint,
                router_,
                stop_condition_,
                hub_);
        }

        /**
         * @brief 设置 NotificationHub（延迟注入，解决初始化顺序问题）。
         *
         * 同时传播到 Listener。
         */
        void SetHub(std::shared_ptr<NotificationHub> hub)
        {
            hub_ = std::move(hub);
            if (listener_)
            {
                listener_->SetHub(hub_);
            }
        }

        /**
         * @brief 获取 io_context 引用（用于创建 NotificationHub）。
         */
        boost::asio::io_context& IoCtx() noexcept { return ioc_; }

        /**
         * @brief Run the HTTP server with 4 workers (blocking).
         *
         * Starts the listener and runs io_context on 4 threads total:
         * 3 dedicated worker threads + the calling thread. Blocks until
         * ioc_.stop() is called (typically via Stop() from a shutdown
         * signal).
         *
         * After ioc_.run() returns on the calling thread, joins all worker
         * threads and returns. This preserves the blocking contract expected
         * by App.cpp's lifecycle (Run() -> IPC shutdown -> exit).
         *
         * @thread_safety  Must be called once from the main thread.
         * @architecture   HTTP multi-worker execution domain (Phase 52).
         */
        void Run()
        {
            listener_->Run();

            // Launch 3 worker threads (4 total with calling thread)
            constexpr unsigned int worker_count = 3;
            threads_.reserve(worker_count);
            for (unsigned int i = 0; i < worker_count; ++i)
            {
                threads_.emplace_back(
                    [this]()
                    {
                        try
                        {
                            ioc_.run();
                        }
                        catch (const std::exception& ex)
                        {
                            DAS_LOG_ERROR(
                                DAS_FMT_NS::format(
                                    "HTTP worker thread exception: {}",
                                    ex.what())
                                    .c_str());
                        }
                    });
            }

            // Calling thread also runs ioc_.run() — total 4 workers
            try
            {
                ioc_.run();
            }
            catch (const std::exception& ex)
            {
                DAS_LOG_ERROR(
                    DAS_FMT_NS::format(
                        "HTTP main thread exception: {}",
                        ex.what())
                        .c_str());
            }

            // ioc_.run() returned — join all worker threads
            for (auto& t : threads_)
            {
                if (t.joinable())
                {
                    t.join();
                }
            }
            threads_.clear();
        }

        /**
         * @brief Stop the HTTP server and join all workers.
         *
         * Calls ioc_.stop() to unblock all ioc_.run() calls (including
         * the calling thread's). Run() will then join all worker threads
         * and return.
         *
         * @architecture   Called from shutdown signal handler or test
         *                 teardown.
         */
        void Stop()
        {
            ioc_.stop();
            // Note: thread joining happens in Run() after ioc_.run()
            // returns
        }

        Router& GetRouter() { return *router_; }

        const Router& GetRouter() const { return *router_; }

    private:
        std::shared_ptr<Router>          router_;
        std::shared_ptr<NotificationHub> hub_;
        std::function<bool()>            stop_condition_;
        boost::asio::io_context          ioc_;
        std::shared_ptr<Listener>        listener_;
        std::vector<std::thread>         threads_;
    };

} // namespace Das::Http::Beast

#endif // DAS_HTTP_BEAST_SERVER_HPP
