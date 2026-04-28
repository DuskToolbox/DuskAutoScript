#ifndef DAS_HTTP_NOTIFICATION_HUB_HPP
#define DAS_HTTP_NOTIFICATION_HUB_HPP

#include <boost/asio/io_context.hpp>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

namespace Das::Http
{

    namespace Beast
    {
        class WsSession;
    }

    using Beast::WsSession;

    /**
     * @brief WebSocket 连接管理和广播中心。
     *
     * 管理所有活跃 WebSocket 连接，提供线程安全的广播能力。
     * 调度器和设置服务通过 C 回调 DasNotifySchedulerStateChanged
     * 触发广播，不直接依赖本类（避免 DasCore → DasHttp 反向依赖）。
     *
     * @thread_safety 所有公开方法均为线程安全。
     */
    class NotificationHub
    {
    public:
        /**
         * @brief 构造 Hub 并关联 io_context（用于 dispatch 写操作）。
         */
        explicit NotificationHub(boost::asio::io_context& ioc);

        ~NotificationHub();

        // 禁止拷贝/移动
        NotificationHub(const NotificationHub&) = delete;
        NotificationHub& operator=(const NotificationHub&) = delete;
        NotificationHub(NotificationHub&&) = delete;
        NotificationHub& operator=(NotificationHub&&) = delete;

        /**
         * @brief 注册一个 WebSocket 会话。
         *
         * @param session 弱引用，Hub 不延长会话生命周期。
         */
        void Register(std::weak_ptr<WsSession> session);

        /**
         * @brief 注销一个 WebSocket 会话（连接断开时调用）。
         *
         * @param session 原始指针，用于精确匹配。
         */
        void Unregister(WsSession* session);

        /**
         * @brief 广播消息到所有已连接的 WebSocket 客户端。
         *
         * 写操作通过 io_context::dispatch 在当前 io 线程执行，
         * 确保与 Beast WebSocket stream 的线程安全。
         *
         * @param message JSON 字符串，所有权转移给此函数。
         */
        void Broadcast(std::string message);

        /**
         * @brief 获取活跃连接数（用于诊断）。
         */
        size_t ActiveConnectionCount() const;

        boost::asio::io_context& IoCtx() noexcept { return ioc_; }

    private:
        void CleanupStaleConnections();

        boost::asio::io_context&              ioc_;
        mutable std::shared_mutex             mutex_;
        std::vector<std::weak_ptr<WsSession>> sessions_;
    };

} // namespace Das::Http

#endif // DAS_HTTP_NOTIFICATION_HUB_HPP
