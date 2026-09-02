#ifndef DAS_HTTP_BEAST_SERVER_HPP
#define DAS_HTTP_BEAST_SERVER_HPP

#include "../Config.h"
#include "../NotificationHub.hpp"
#include "Request.hpp"
#include "Router.hpp"
#include "SseSession.hpp"
#include <algorithm>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
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
#include <das/Core/IPC/IpcErrors.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
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

    // SSE 订阅路径前缀（Session::TryHandOffToSse 单点判定用）。SSE 是长
    // 推送，必须脱离请求-响应循环接管连接。
    inline constexpr std::string_view kSseRequestPathPrefix =
        "/" DAS_HTTP_API_PREFIX "authoring/events/";

    // ── 入站 HTTP / 出站 WS 交换日志 ──
    // 与 Router::Handle 的请求行互补：这里补齐请求头/请求体/响应头/响应体
    // 与 WS 推送内容，排障时能把每次交互的内容直接对上。两条静默规则：
    // /logs/ 端点不打（防「处理日志请求 → 写日志 → 日志 sink 广播 → 又拉
    // 日志」反馈循环）；/alive 探活不打明细（高频零排查价值，请求行由
    // Router 照常记录）。
    inline constexpr std::size_t kExchangeLogBodyLimit = 512;

    inline bool IsSensitiveHeaderNameForLog(std::string_view name)
    {
        auto equals_ci = [](std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                const auto ca = static_cast<unsigned char>(a[i] | 0x20);
                const auto cb = static_cast<unsigned char>(b[i] | 0x20);
                if (ca != cb)
                {
                    return false;
                }
            }
            return true;
        };
        return equals_ci(name, "authorization")
               || equals_ci(name, "proxy-authorization")
               || equals_ci(name, "cookie") || equals_ci(name, "set-cookie");
    }

    // target（path + query）里 cdk 值打码：用户 CDK 是长期秘密。
    inline std::string RedactTargetForLog(std::string_view target)
    {
        std::string result(target);
        for (std::size_t pos = result.find("cdk="); pos != std::string::npos;
             pos = result.find("cdk=", pos))
        {
            const std::size_t value_start = pos + 4;
            const std::size_t value_end = result.find('&', value_start);
            result.replace(
                value_start,
                (value_end == std::string::npos ? result.size() : value_end)
                    - value_start,
                "***");
            pos = value_start + 3;
        }
        return result;
    }

    // JSON body 里 cdk / mirrorchyanCdk 字符串字段值打码（apply 的
    // settings change 会携带用户 CDK）。
    inline void RedactJsonSecretValues(std::string& body)
    {
        for (const std::string_view key :
             {"\"cdk\":\"", "\"mirrorchyanCdk\":\""})
        {
            for (std::size_t pos = body.find(key); pos != std::string::npos;
                 pos = body.find(key, pos))
            {
                const std::size_t value_start = pos + key.size();
                const std::size_t value_end = body.find('"', value_start);
                if (value_end == std::string::npos)
                {
                    break;
                }
                body.replace(value_start, value_end - value_start, "***");
                pos = value_start + 3;
            }
        }
    }

    // body 交换日志展示串：先打码秘密值，再截断到上限并把不可打印字节
    // 替换为 '.'。
    inline std::string FormatBodyForExchangeLog(std::string_view body)
    {
        std::string redacted(body);
        RedactJsonSecretValues(redacted);
        const std::size_t shown =
            std::min(redacted.size(), kExchangeLogBodyLimit);
        std::string printable;
        printable.reserve(shown);
        for (std::size_t index = 0; index < shown; ++index)
        {
            const auto byte = static_cast<unsigned char>(redacted[index]);
            printable.push_back(
                (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.');
        }
        return DAS_FMT_NS::format(
            "{} ({}/{} bytes)",
            printable,
            shown,
            redacted.size());
    }

    // 交换明细是否跳过（反馈循环端点 / 高频探活）。
    inline bool ShouldSkipExchangeLog(std::string_view target)
    {
        return target.find("/logs/") != std::string_view::npos
               || target.find("/alive") != std::string_view::npos;
    }

    template <typename Message>
    inline void LogHttpMessageForExchange(
        std::string_view direction,
        const Message&   message)
    {
        const std::string target =
            RedactTargetForLog(std::string_view(message.target()));
        DAS_LOG_INFO(
            DAS_FMT_NS::format(
                "[HTTP] {} {} {} (body {} bytes)",
                direction,
                std::string_view(message.method_string()),
                target,
                message.body().size())
                .c_str());
        for (const auto& field : message)
        {
            const std::string name(field.name_string());
            const std::string value =
                IsSensitiveHeaderNameForLog(field.name_string())
                    ? "***"
                    : std::string(field.value());
            DAS_LOG_INFO(
                DAS_FMT_NS::format("[HTTP] {}   {}: {}", direction, name, value)
                    .c_str());
        }
        if (!message.body().empty())
        {
            DAS_LOG_INFO(
                DAS_FMT_NS::format(
                    "[HTTP] {}   body {}",
                    direction,
                    FormatBodyForExchangeLog(message.body()))
                    .c_str());
        }
    }

    inline void LogHttpResponseForExchange(
        const http::response<http::string_body>& response)
    {
        DAS_LOG_INFO(
            DAS_FMT_NS::format(
                "[HTTP] < {} {} (body {} bytes)",
                std::to_string(static_cast<unsigned>(response.result_int())),
                std::string(response.reason()),
                response.body().size())
                .c_str());
        for (const auto& field : response)
        {
            const std::string name(field.name_string());
            const std::string value =
                IsSensitiveHeaderNameForLog(field.name_string())
                    ? "***"
                    : std::string(field.value());
            DAS_LOG_INFO(
                DAS_FMT_NS::format("[HTTP] <   {}: {}", name, value).c_str());
        }
        if (!response.body().empty())
        {
            DAS_LOG_INFO(
                DAS_FMT_NS::format(
                    "[HTTP] <   body {}",
                    FormatBodyForExchangeLog(response.body()))
                    .c_str());
        }
    }

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
        // SSE 分流（keep-alive 跟随请求命中订阅路径时移交 SseSession）。
        std::shared_ptr<SseHub> sse_hub_;
        // SSE 订阅 key 门（返回 false 拒绝订阅：落普通路由 404）。空 = 放行
        // （向后兼容）。
        std::function<bool(const std::string&)> sse_key_gate_;
        // SSE 会话注册完成回调（订阅建立处快照重放注入点）。
        std::function<void(SseSessionBase&)> sse_on_registered_;

    public:
        Session(
            tcp::socket                             socket,
            std::shared_ptr<Router>                 router,
            std::function<bool()>                   stop_condition,
            std::function<void()>                   on_server_stop = nullptr,
            std::shared_ptr<SseHub>                 sse_hub = nullptr,
            std::function<bool(const std::string&)> sse_key_gate = nullptr,
            std::function<void(SseSessionBase&)>    sse_on_registered = nullptr)
            : socket_(std::move(socket)), router_(std::move(router)),
              stop_condition_(std::move(stop_condition)),
              on_server_stop_(std::move(on_server_stop)),
              sse_hub_(std::move(sse_hub)),
              sse_key_gate_(std::move(sse_key_gate)),
              sse_on_registered_(std::move(sse_on_registered))
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
            // 连接首请求（UpgradeDetector 判定非 WS 升级后移交）与 keep-alive
            // 跟随请求（DoRead 读取）统一在此分流：SSE 订阅路径移交长推送
            // 会话，其余进普通路由。SSE 判定全服务单一位置。
            if (TryHandOffToSse())
            {
                return;
            }
            ProcessRequest();
        }

    private:
        // SSE 订阅路径分流（全服务单一判定点）：命中前缀且 key 非空时把
        // 连接移交给 SseSession（长推送接管，脱离请求-响应循环）。连接的
        // 第一个请求经 RunWithParser、keep-alive 跟随请求经 DoRead，都汇
        // 聚到这里——.NET HttpClient 连接池会把 SSE GET 作为同连接的第二个
        // 请求发出（首个请求通常是刚完成的 authoring/apply），只在首请求
        // 分流时 SSE 请求会落进普通路由以 404 结束，客户端静默无订阅。
        bool TryHandOffToSse()
        {
            if (!sse_hub_
                || !request_.target().starts_with(kSseRequestPathPrefix))
            {
                return false;
            }
            const auto key =
                request_.target().substr(kSseRequestPathPrefix.size());
            if (key.empty())
            {
                return false;
            }
            // key 门：既无快照也无活跃上报者的 key 拒绝订阅——返回 false
            // 落普通路由（events 路径无注册路由 → 404）。空门 = 放行
            // （向后兼容）。
            if (sse_key_gate_ && !sse_key_gate_(std::string(key)))
            {
                return false;
            }
            auto sse = std::make_shared<SseSession>(
                std::move(socket_),
                std::string(key),
                sse_hub_,
                std::chrono::seconds(15),
                sse_on_registered_);
            sse->DoStart();
            return true;
        }

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
                    if (self->TryHandOffToSse())
                    {
                        return;
                    }
                    self->ProcessRequest();
                });
        }

        void ProcessRequest()
        {
            // keep-alive 跟随请求：响应宣称的连接语义与实际行为一致
            const bool keep_alive = request_.keep_alive();
            // 交换日志的跳过判定在请求被 move 前完成，请求/响应两侧共用。
            const bool log_exchange = !ShouldSkipExchangeLog(request_.target());
            if (log_exchange)
            {
                LogHttpMessageForExchange(">", request_);
            }
            HttpRequest req(std::move(request_));

            if (router_->DispatchToBusiness(req))
            {
                // sender 捕获要求可拷贝，HttpRequest 为 move-only
                auto self = shared_from_this();
                auto request_holder =
                    std::make_shared<HttpRequest>(std::move(req));
                router_->SpawnBusiness(
                    stdexec::schedule(router_->GetBusinessScheduler())
                    | stdexec::then(
                        [self, request_holder, keep_alive, log_exchange]()
                        {
                            HttpResponse http_response =
                                self->router_->Handle(*request_holder);
                            auto response_holder =
                                std::make_shared<HttpResponse>(
                                    std::move(http_response));
                            boost::asio::post(
                                self->socket_.get_executor(),
                                [self,
                                 response_holder,
                                 keep_alive,
                                 log_exchange]()
                                {
                                    self->FinishRequest(
                                        std::move(*response_holder),
                                        keep_alive,
                                        log_exchange);
                                });
                        }));
                return;
            }

            FinishRequest(router_->Handle(req), keep_alive, log_exchange);
        }

        // 内联路径与派发完成回调共用的写回路径（io 线程）
        void FinishRequest(
            HttpResponse http_response,
            bool         keep_alive,
            bool         log_exchange)
        {
            response_ = http_response.Release();
            response_.keep_alive(keep_alive);
            if (log_exchange)
            {
                LogHttpResponseForExchange(response_);
            }

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

                    // 响应已交付（字节进内核缓冲）：request_shutdown 置位过
                    // 停止条件时在此触发服务器关闭——写完成回调是唯一能
                    // 保证「响应送达」与「停止服务器」先后次序的位置。
                    if (self->on_server_stop_ && self->stop_condition_
                        && !self->stop_condition_())
                    {
                        self->on_server_stop_();
                    }

                    if (self->response_.keep_alive())
                    {
                        // keep-alive：继续等本连接的下一个请求。此前每响应
                        // 一次就 shutdown(send) 是 HTTP/1.1 违约——响应头宣称
                        // keep-alive 却立即断开，客户端（.NET HttpClient）把
                        // 连接放回池子后下一个请求撞上 FIN/RST，间歇性报
                        // "error occurred while sending the request"。
                        self->request_ = {};
                        self->DoRead();
                        return;
                    }

                    // Connection: close —— 显式关闭
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
                    // WS 推送内容进日志（截断 + 秘密值打码）：推送内容此前
                    // 是日志盲区，出问题无法对账客户端收到了什么。日志端点
                    // 的推送静默（消息体 api 字段携带 api/v1/logs/）：该推送
                    // 高频（前端拉调试日志的响应经 WS 回程），打了只会刷屏。
                    if (msg->find("api/v1/logs/") == std::string::npos)
                    {
                        DAS_LOG_INFO(
                            DAS_FMT_NS::format(
                                "[WS] push {}",
                                FormatBodyForExchangeLog(*msg))
                                .c_str());
                    }
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
        tcp::socket                             socket_;
        std::shared_ptr<Router>                 router_;
        std::function<bool()>                   stop_condition_;
        std::function<void()>                   on_server_stop_;
        std::shared_ptr<NotificationHub>        hub_;
        std::shared_ptr<SseHub>                 sse_hub_;
        std::function<bool(const std::string&)> sse_key_gate_;
        std::function<void(SseSessionBase&)>    sse_on_registered_;
        boost::beast::flat_buffer               buffer_;

    public:
        UpgradeDetector(
            tcp::socket                             socket,
            std::shared_ptr<Router>                 router,
            std::function<bool()>                   stop_condition,
            std::shared_ptr<NotificationHub>        hub,
            std::function<void()>                   on_server_stop = nullptr,
            std::shared_ptr<SseHub>                 sse_hub = nullptr,
            std::function<bool(const std::string&)> sse_key_gate = nullptr,
            std::function<void(SseSessionBase&)>    sse_on_registered = nullptr)
            : socket_(std::move(socket)), router_(std::move(router)),
              stop_condition_(std::move(stop_condition)),
              on_server_stop_(std::move(on_server_stop)), hub_(std::move(hub)),
              sse_hub_(std::move(sse_hub)),
              sse_key_gate_(std::move(sse_key_gate)),
              sse_on_registered_(std::move(sse_on_registered))
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

                    // 连接首请求：WebSocket 升级在此分流（升级语义只可能
                    // 出现在连接建立后的第一个请求）；SSE 订阅与普通请求
                    // 一并移交 Session，由 Session::TryHandOffToSse 统一
                    // 分流（判定单一位置，keep-alive 跟随请求同路径）。
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
                            self->on_server_stop_,
                            self->sse_hub_,
                            self->sse_key_gate_,
                            self->sse_on_registered_);
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
        tcp::acceptor                           acceptor_;
        std::shared_ptr<Router>                 router_;
        std::shared_ptr<NotificationHub>        hub_;
        std::shared_ptr<SseHub>                 sse_hub_;
        std::function<bool()>                   stop_condition_;
        std::function<void()>                   on_server_stop_;
        std::function<bool(const std::string&)> sse_key_gate_;
        std::function<void(SseSessionBase&)>    sse_on_registered_;

    public:
        Listener(
            boost::asio::io_context&                ioc,
            tcp::endpoint                           endpoint,
            std::shared_ptr<Router>                 router,
            std::function<bool()>                   stop_condition,
            std::shared_ptr<NotificationHub>        hub,
            std::function<void()>                   on_server_stop = nullptr,
            std::shared_ptr<SseHub>                 sse_hub = nullptr,
            std::function<bool(const std::string&)> sse_key_gate = nullptr,
            std::function<void(SseSessionBase&)>    sse_on_registered = nullptr)
            : acceptor_(boost::asio::make_strand(ioc)),
              router_(std::move(router)), hub_(std::move(hub)),
              sse_hub_(std::move(sse_hub)),
              stop_condition_(std::move(stop_condition)),
              on_server_stop_(std::move(on_server_stop)),
              sse_key_gate_(std::move(sse_key_gate)),
              sse_on_registered_(std::move(sse_on_registered))
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

        /**
         * @brief 设置 SSE 订阅 key 门（延迟注入）。
         */
        void SetSseKeyGate(std::function<bool(const std::string&)> gate)
        {
            sse_key_gate_ = std::move(gate);
        }

        /**
         * @brief 设置 SSE 会话注册完成回调（延迟注入）。
         */
        void SetSseOnRegistered(std::function<void(SseSessionBase&)> callback)
        {
            sse_on_registered_ = std::move(callback);
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
                        self->sse_hub_,
                        self->sse_key_gate_,
                        self->sse_on_registered_);
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
         * @brief 设置 SSE 订阅 key 门（延迟注入）。
         *
         * gate 对既无快照也无活跃上报者的 key 返回 false → 拒绝订阅，
         * 落普通路由（events 路径无注册路由 → 404）；空 gate 放行。
         * 同时传播到 Listener。
         */
        void SetSseKeyGate(std::function<bool(const std::string&)> gate)
        {
            sse_key_gate_ = std::move(gate);
            if (listener_)
            {
                listener_->SetSseKeyGate(sse_key_gate_);
            }
        }

        /**
         * @brief 设置 SSE 会话注册完成回调（延迟注入）。
         *
         * 会话注册完成后调用（头写完 + hub Register 之后），供订阅建立
         * 处补发快照重放。同时传播到 Listener。
         */
        void SetSseOnRegistered(std::function<void(SseSessionBase&)> callback)
        {
            sse_on_registered_ = std::move(callback);
            if (listener_)
            {
                listener_->SetSseOnRegistered(sse_on_registered_);
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
        std::shared_ptr<Router>                 router_;
        std::shared_ptr<NotificationHub>        hub_;
        std::shared_ptr<SseHub>                 sse_hub_;
        std::function<bool()>                   stop_condition_;
        std::function<bool(const std::string&)> sse_key_gate_;
        std::function<void(SseSessionBase&)>    sse_on_registered_;
        boost::asio::io_context                 ioc_;
        std::shared_ptr<Listener>               listener_;
        std::vector<std::thread>                threads_;
    };

} // namespace Das::Http::Beast

#endif // DAS_HTTP_BEAST_SERVER_HPP
