#ifndef DAS_HTTP_BEAST_SERVER_HPP
#define DAS_HTTP_BEAST_SERVER_HPP

#include "../Config.h"
#include "Request.hpp"
#include "Router.hpp"
#include "SseSession.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/config.hpp>
#include <chrono>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
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
        // 响应写完成且停止条件已置位时回调（触发 Server::BeginShutdown）。
        // 在写完成处触发保证响应字节已进内核缓冲，退出路径不影响送达。
        std::function<void()> on_server_stop_;

    public:
        Session(
            tcp::socket             socket,
            std::shared_ptr<Router> router,
            std::function<bool()>   stop_condition,
            std::function<void()>   on_server_stop = nullptr)
            : socket_(std::move(socket)), router_(std::move(router)),
              stop_condition_(std::move(stop_condition)),
              on_server_stop_(std::move(on_server_stop))
        {
        }

        void Run() { DoRead(); }

        /**
         * @brief 使用 UpgradeDetector 已读取的完整请求启动会话。
         *
         * 当 UpgradeDetector 已从 socket 读取完整 HTTP 请求并判定为
         * 普通 HTTP 请求时，直接复用已读取的请求进行处理。
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

                    // 响应已交付（字节进内核缓冲）：request_shutdown 置位过
                    // 停止条件时在此触发服务器关闭——写完成回调是唯一能
                    // 保证「响应送达」与「停止服务器」先后次序的位置。
                    if (self->on_server_stop_ && self->stop_condition_
                        && !self->stop_condition_())
                    {
                        self->on_server_stop_();
                    }
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
        // 待发消息队列（shared_ptr 零拷贝传递；strand 内访问，无锁）。
        std::queue<std::shared_ptr<const std::string>> write_queue_;
        // 当"有新消息"的唤醒开关（不计时，expires_at max，仅 cancel_one
        // 触发）。
        boost::asio::steady_timer wakeup_timer_;

    public:
        WsSession(tcp::socket socket, std::shared_ptr<NotificationHub> hub)
            : ws_(std::move(socket)), hub_(std::move(hub)),
              wakeup_timer_(ws_.get_executor())
        {
            wakeup_timer_.expires_at(
                std::chrono::steady_clock::time_point::max());
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

                    // 启动写协程（co_spawn 在 session strand：串行化
                    // async_write， 防 beast websocket soft_mutex 重入断言）
                    boost::asio::co_spawn(
                        self->ws_.get_executor(),
                        [self]() -> boost::asio::awaitable<void>
                        { co_await self->WriteLoop(); },
                        boost::asio::detached);

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
            // dispatch 进 session strand：push queue + cancel_one 唤醒写协程。
            // 调用者无需关心是否已在 strand（dispatch 同步/异步自适应）。
            boost::asio::dispatch(
                ws_.get_executor(),
                [self = shared_from_this(), message]()
                {
                    self->write_queue_.push(message);
                    self->wakeup_timer_.cancel_one();
                });
        }

    private:
        // 写协程：串行化 async_write（co_await 保证 completion 后才下一条，
        // 防 beast websocket soft_mutex 重入断言）。msg 在协程帧局部，
        // co_await 期间存活。队列空时挂起 async_wait，WriteMessage cancel_one
        // 唤醒。
        boost::asio::awaitable<void> WriteLoop()
        {
            auto self = shared_from_this();
            while (true)
            {
                if (!self->write_queue_.empty())
                {
                    auto msg = self->write_queue_.front();
                    self->write_queue_.pop();
                    // redirect_error：连接断开时把异常转成 ec，避免
                    // detached 协程未捕获异常 → std::terminate 进程崩溃
                    // （点击停止调度时 UI 刷新/关闭 WS，服务端仍在广播 →
                    // async_write 打到已死连接）。失败即注销 hub 并退出
                    // 协程，与 DoReadLoop 清理路径一致。
                    boost::system::error_code write_ec;
                    co_await self->ws_.async_write(
                        boost::asio::buffer(*msg),
                        boost::asio::redirect_error(
                            boost::asio::use_awaitable,
                            write_ec));
                    if (write_ec)
                    {
                        self->hub_->Unregister(self.get());
                        co_return;
                    }
                    continue;
                }
                boost::system::error_code ec;
                co_await self->wakeup_timer_.async_wait(
                    boost::asio::redirect_error(
                        boost::asio::use_awaitable,
                        ec));
            }
            co_return;
        }

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
     * 读取完整的 HTTP 请求，检查是否为 Upgrade: websocket。
     * - 是 WebSocket → 创建 WsSession 并执行握手
     * - 否 → 创建普通 HTTP Session 并处理
     */
    class UpgradeDetector : public std::enable_shared_from_this<UpgradeDetector>
    {
        tcp::socket                      socket_;
        std::shared_ptr<Router>          router_;
        std::function<bool()>            stop_condition_;
        std::function<void()>            on_server_stop_;
        std::shared_ptr<NotificationHub> hub_;
        std::shared_ptr<SseHub>          sse_hub_;
        boost::beast::flat_buffer        buffer_;

    public:
        UpgradeDetector(
            tcp::socket                      socket,
            std::shared_ptr<Router>          router,
            std::function<bool()>            stop_condition,
            std::shared_ptr<NotificationHub> hub,
            std::function<void()>            on_server_stop = nullptr,
            std::shared_ptr<SseHub>          sse_hub = nullptr)
            : socket_(std::move(socket)), router_(std::move(router)),
              stop_condition_(std::move(stop_condition)),
              on_server_stop_(std::move(on_server_stop)), hub_(std::move(hub)),
              sse_hub_(std::move(sse_hub))
        {
        }

        /**
         * @brief 设置 SseHub（延迟注入，解决初始化顺序问题）。
         */
        void SetSseHub(std::shared_ptr<SseHub> sse_hub)
        {
            sse_hub_ = std::move(sse_hub);
        }

        void Detect()
        {
            auto self = shared_from_this();

            http::async_read(
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

                    // SSE 长响应：路径前缀 {base}authoring/events/{key} 分派
                    // 到 SseSession（不经同步 RouteHandler，RESEARCH Pitfall
                    // 1）
                    const boost::beast::string_view sse_prefix =
                        "/" DAS_HTTP_API_PREFIX "authoring/events/";
                    if (self->sse_hub_ && req.target().starts_with(sse_prefix))
                    {
                        const boost::beast::string_view key =
                            req.target().substr(sse_prefix.size());
                        if (!key.empty())
                        {
                            auto sse = std::make_shared<SseSession>(
                                std::move(self->socket_),
                                std::string(key),
                                self->sse_hub_);
                            sse->DoStart();
                            return;
                        }
                    }

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
                        // Normal HTTP — create Session with complete request
                        auto session = std::make_shared<Session>(
                            std::move(self->socket_),
                            self->router_,
                            self->stop_condition_,
                            self->on_server_stop_);
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
        std::shared_ptr<SseHub>          sse_hub_;
        std::function<bool()>            stop_condition_;
        std::function<void()>            on_server_stop_;

    public:
        Listener(
            boost::asio::io_context&         ioc,
            tcp::endpoint                    endpoint,
            std::shared_ptr<Router>          router,
            std::function<bool()>            stop_condition,
            std::shared_ptr<NotificationHub> hub,
            std::function<void()>            on_server_stop = nullptr,
            std::shared_ptr<SseHub>          sse_hub = nullptr)
            : acceptor_(boost::asio::make_strand(ioc)),
              router_(std::move(router)), hub_(std::move(hub)),
              sse_hub_(std::move(sse_hub)),
              stop_condition_(std::move(stop_condition)),
              on_server_stop_(std::move(on_server_stop))
        {
            // open/bind/listen 失败必须 fail-fast 抛异常：否则 acceptor_
            // 处于未监听状态，DoAccept 的 async_accept 立即以错误完成且不
            // 重试，io_context 没有任何挂起 work，server.Run() 会立即返回，
            // 进程静默 exit 0——外部（如 DasNativeUI）只见神秘的「连接失败」。
            // 典型诱因：监听端口落在 Windows 动态排除端口范围
            // （netsh excludedportrange），bind 返回 WSAEACCES。抛异常让
            // 上层打印明确的端口错误并以非零码退出，便于排查。
            boost::system::error_code ec;

            acceptor_.open(endpoint.protocol(), ec);
            if (ec)
            {
                throw std::runtime_error(
                    "打开监听 socket 失败: " + ec.message());
            }

            acceptor_.set_option(
                boost::asio::socket_base::reuse_address(true),
                ec);
            // reuse_address 失败非致命，忽略继续

            acceptor_.bind(endpoint, ec);
            if (ec)
            {
                throw std::runtime_error(
                    "bind " + endpoint.address().to_string() + ":"
                    + std::to_string(endpoint.port())
                    + " 失败: " + ec.message());
            }

            acceptor_.listen(
                boost::asio::socket_base::max_listen_connections,
                ec);
            if (ec)
            {
                throw std::runtime_error("listen 失败: " + ec.message());
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

        /**
         * @brief 设置 SseHub（延迟注入，解决初始化顺序问题）。
         */
        void SetSseHub(std::shared_ptr<SseHub> sse_hub)
        {
            sse_hub_ = std::move(sse_hub);
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
                    // deciding HTTP vs WebSocket / SSE
                    auto detector = std::make_shared<UpgradeDetector>(
                        std::move(socket),
                        self->router_,
                        self->stop_condition_,
                        self->hub_,
                        self->on_server_stop_,
                        self->sse_hub_);
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
            std::shared_ptr<NotificationHub> hub = nullptr,
            std::shared_ptr<SseHub>          sse_hub = nullptr)
            : router_(std::move(router)), hub_(std::move(hub)),
              sse_hub_(std::move(sse_hub)),
              stop_condition_(std::move(stop_condition)), ioc_(4)
        {
            const auto addr = boost::asio::ip::make_address(address);
            const auto endpoint = tcp::endpoint{addr, port};

            listener_ = std::make_shared<Listener>(
                ioc_,
                endpoint,
                router_,
                stop_condition_,
                hub_,
                // 停止回调绑定到本 Server：由完成 request_shutdown 响应写出
                // 的 Session 在写完成处触发（见 Session::DoWrite）。
                [this]() { BeginShutdown(); },
                sse_hub_);
        }

        /// 停止服务器：SSE 失效 → ioc.stop() 解除全部 ioc_.run() 阻塞
        /// （idle keep-alive 读 / WS / 日志 tick / pending accept 一并扫掉，
        /// acceptor 由析构关闭）。由 Session 写完成回调同步调用（ioc 工作
        /// 线程；此时响应字节已进内核缓冲，退出路径不影响送达）。
        /// 无需自加幂等标志：Invalidate 是原子置位、ioc.stop() 天然幂等，
        /// 且首个 stop() 后其余排队 handler 不再执行——多 Session 并发触发
        /// 的重复调用全部落到原语自身的 no-op 上。ioc 停止后 Server::Run
        /// 返回，App 既有的 ipc RequestStop + CoreServices::Shutdown
        /// 收尾链条自然接上。
        void BeginShutdown()
        {
            if (sse_hub_)
            {
                sse_hub_->Invalidate();
            }
            ioc_.stop();
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
         * @brief 设置 SseHub（延迟注入，解决初始化顺序问题）。
         *
         * 同时传播到 Listener。
         */
        void SetSseHub(std::shared_ptr<SseHub> sse_hub)
        {
            sse_hub_ = std::move(sse_hub);
            if (listener_)
            {
                listener_->SetSseHub(sse_hub_);
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
        std::shared_ptr<SseHub>          sse_hub_;
        std::function<bool()>            stop_condition_;
        boost::asio::io_context          ioc_;
        std::shared_ptr<Listener>        listener_;
        std::vector<std::thread>         threads_;
    };

} // namespace Das::Http::Beast

#endif // DAS_HTTP_BEAST_SERVER_HPP
