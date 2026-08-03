#ifndef DAS_HTTP_BEAST_SSE_SESSION_HPP
#define DAS_HTTP_BEAST_SSE_SESSION_HPP

#include "../SseHub.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace Das::Http::Beast
{
    namespace http = boost::beast::http;
    using tcp = boost::asio::ip::tcp;

    /**
     * @brief SSE 会话抽象基类（供 SseHub 持弱引用 + 虚调写路径）。
     *
     * 与具体流类型解耦：SseSessionT<Stream> 继承本基类，hub 只依赖基类
     * 接口（GetExecutor / WriteData / Key），使测试可用 beast::test::stream
     * 确定性驱动写失败与字节断言。
     */
    class SseSessionBase
    {
    public:
        virtual ~SseSessionBase() = default;

        /**
         * @brief 获取会话 executor（供 hub dispatch 写操作到 strand）。
         */
        virtual boost::asio::any_io_executor GetExecutor() = 0;

        /**
         * @brief 写一个 SSE 事件帧（"data: {json}\n\n" chunk）。
         *
         * @param payload 事件负载，共享指针避免拷贝。
         */
        virtual void WriteData(std::shared_ptr<const std::string> payload) = 0;

        /**
         * @brief 会话注册的业务 key。
         */
        virtual const std::string& Key() const noexcept = 0;
    };

    /**
     * @brief SSE 流式会话（boost::beast 长连接，chunked 分块写）。
     *
     * 写 200 头（text/event-stream + chunked + no-cache）后保持连接，
     * 每事件写一个 "data: {json}\n\n" chunk 帧。写失败 / keep-alive
     * watchdog 检测到死连接时注销并关闭 socket。
     *
     * 头帧经 response_serializer<buffer_body> async_write_header 写出
     * （RESEARCH Pitfall 2：禁裸 http::write(make_chunk)）。事件帧手写
     * chunked 帧（"<hex>\r\n<data>\r\n"）经 boost::asio::async_write 写出
     * —— 生产参考 gbmcweb 直接写原始 body 字节，规避 buffer_body
     * serializer 每 buffer 两次 get() 的 need_buffer 状态机对多帧流的干扰。
     * 所有写经 write_mutex_ + 队列串行化，防并发写 chunk。
     */
    template <class Stream>
    class SseSessionT : public SseSessionBase,
                        public std::enable_shared_from_this<SseSessionT<Stream>>
    {
    public:
        /**
         * @param socket 已接受、带 strand executor 的连接 socket。
         * @param key 业务任务标识（如更新任务的 entry id）。
         * @param hub 会话所属 SseHub（强引用，会话内保活）。
         * @param keepalive_interval keep-alive comment 帧间隔（可注入测试）。
         */
        SseSessionT(
            Stream                             socket,
            std::string                        key,
            std::shared_ptr<Das::Http::SseHub> hub,
            std::chrono::milliseconds          keepalive_interval =
                std::chrono::seconds(15))
            : socket_(std::move(socket)), key_(std::move(key)),
              hub_(std::move(hub)), keepalive_interval_(keepalive_interval),
              keepalive_timer_(socket_.get_executor())
        {
        }

        // 禁止拷贝/移动
        SseSessionT(const SseSessionT&) = delete;
        SseSessionT& operator=(const SseSessionT&) = delete;

        /**
         * @brief 启动会话：写 200 头，完成后注册到 hub + 启动 keep-alive。
         */
        void DoStart()
        {
            auto self = this->shared_from_this();

            response_.version(11);
            response_.result(http::status::ok);
            response_.set(http::field::content_type, "text/event-stream");
            response_.set(http::field::cache_control, "no-cache");
            response_.set(http::field::connection, "keep-alive");
            response_.chunked(true);

            http::async_write_header(
                socket_,
                serializer_,
                [self](boost::beast::error_code ec, std::size_t /*bytes*/)
                {
                    if (ec)
                    {
                        self->Close();
                        return;
                    }
                    if (self->hub_)
                    {
                        self->hub_->Register(
                            self->shared_from_this(),
                            self->key_);
                    }
                    self->StartKeepalive();
                });
        }

        /**
         * @brief 获取会话 executor（供 hub dispatch 写操作）。
         */
        boost::asio::any_io_executor GetExecutor() override
        {
            return socket_.get_executor();
        }

        /**
         * @brief 写一个 SSE 事件帧（"data: {json}\n\n" chunk）。
         *
         * 由 SseHub::Broadcast 经 dispatch 调用；所有写经队列串行化。
         *
         * @param payload 事件负载，共享指针避免拷贝。
         */
        void WriteData(std::shared_ptr<const std::string> payload) override
        {
            EnqueueFrame("data: " + *payload + "\n\n");
        }

        /**
         * @brief 会话注册的业务 key。
         */
        const std::string& Key() const noexcept override { return key_; }

        /**
         * @brief 关闭会话（幂等）：取消 keep-alive、关 socket、注销 hub。
         */
        void Close()
        {
            {
                std::lock_guard<std::mutex> lock(write_mutex_);
                if (closed_)
                {
                    return;
                }
                closed_ = true;
                write_queue_.clear();
                write_in_flight_ = false;
            }
            boost::beast::close_socket(socket_);
            keepalive_timer_.cancel();
            if (hub_)
            {
                hub_->Unregister(this, key_);
            }
        }

    private:
        /**
         * @brief 入队一个完整 SSE 帧，若无非在途写则立即开始出队。
         *
         * @param frame 完整帧文本（"data: ...\n\n" 或 ": keepalive\n\n"）。
         */
        void EnqueueFrame(std::string frame)
        {
            bool need_start = false;
            {
                std::lock_guard<std::mutex> lock(write_mutex_);
                if (closed_)
                {
                    return;
                }
                write_queue_.push_back(std::move(frame));
                if (write_in_flight_)
                {
                    return;
                }
                write_in_flight_ = true;
                need_start = true;
            }
            if (need_start)
            {
                DoWriteNext();
            }
        }

        /**
         * @brief 取队列下一帧并异步写；队列空时标记无在途写。
         */
        void DoWriteNext()
        {
            {
                std::lock_guard<std::mutex> lock(write_mutex_);
                if (closed_)
                {
                    return;
                }
                if (write_queue_.empty())
                {
                    write_in_flight_ = false;
                    return;
                }
                current_frame_ = std::move(write_queue_.front());
                write_queue_.pop_front();
            }

            // 组装 chunked 帧：<hex 长度>\r\n<data>\r\n
            chunk_buffer_.clear();
            AppendHexSize(chunk_buffer_, current_frame_.size());
            chunk_buffer_.append("\r\n");
            chunk_buffer_.append(current_frame_);
            chunk_buffer_.append("\r\n");

            auto self = this->shared_from_this();
            boost::asio::async_write(
                socket_,
                boost::asio::buffer(chunk_buffer_),
                [self](boost::beast::error_code ec, std::size_t /*bytes*/)
                {
                    if (ec)
                    {
                        // 写失败：注销 + 关连接（Pitfall 4）
                        self->Close();
                        return;
                    }
                    self->DoWriteNext();
                });
        }

        /**
         * @brief keep-alive watchdog：周期性写 ": keepalive\n\n" comment
         * chunk，写失败即清理（防客户端静默挂起，RESEARCH Open Question 1）。
         */
        void StartKeepalive()
        {
            keepalive_timer_.expires_after(keepalive_interval_);
            keepalive_timer_.async_wait(
                [self = this->shared_from_this()](boost::beast::error_code ec)
                {
                    if (ec)
                    {
                        return; // 已取消（会话关闭）
                    }
                    self->EnqueueFrame(": keepalive\n\n");
                    self->StartKeepalive();
                });
        }

        /**
         * @brief 追加无符号整数的小写十六进制表示（chunk 长度前缀）。
         */
        static void AppendHexSize(std::string& out, std::size_t n)
        {
            static constexpr char kHexDigits[] = "0123456789abcdef";
            if (n == 0)
            {
                out.push_back('0');
                return;
            }
            char  buf[2 * sizeof(std::size_t)];
            char* p = buf + sizeof(buf);
            while (n > 0)
            {
                *--p = kHexDigits[n & 0xF];
                n >>= 4;
            }
            out.append(p, buf + sizeof(buf));
        }

        Stream                                       socket_;
        std::string                                  key_;
        std::shared_ptr<Das::Http::SseHub>           hub_;
        std::mutex                                   write_mutex_;
        std::deque<std::string>                      write_queue_;
        bool                                         write_in_flight_ = false;
        bool                                         closed_ = false;
        std::chrono::milliseconds                    keepalive_interval_;
        boost::asio::steady_timer                    keepalive_timer_;
        http::response<http::buffer_body>            response_;
        http::response_serializer<http::buffer_body> serializer_{response_};
        std::string                                  current_frame_;
        std::string                                  chunk_buffer_;
    };

    using SseSession = SseSessionT<tcp::socket>;

} // namespace Das::Http::Beast

#endif // DAS_HTTP_BEAST_SSE_SESSION_HPP
