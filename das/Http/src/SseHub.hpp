#ifndef DAS_HTTP_SSE_HUB_HPP
#define DAS_HTTP_SSE_HUB_HPP

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Das::Http
{
    namespace Beast
    {
        class SseSessionBase;
    }

    using Beast::SseSessionBase;

    /**
     * @brief per-key SSE 连接管理和广播中心。
     *
     * 管理所有活跃 SSE 会话，按业务 key 分组，提供线程安全的广播能力。
     * hub 不感知任何进度语义（业务中立）：只按 key 维护
     * weak_ptr<SseSessionBase> 集合，某 key 订阅归零时触发 key-empty 回调
     * （接入方接 IDasProgressService::OnAllSubscribersDisconnected）。
     * key-empty 带宽限期：归零后先挂起，宽限期到点复查仍归零才触发——
     * 宽限期内订阅者回归（断线重连）则什么都没发生，业务侧快照与生产
     * 不被抖动误清。必须经 shared_ptr 构造（宽限定时器经
     * enable_shared_from_this 延长 hub 生命周期）。
     *
     * @thread_safety 所有公开方法均为线程安全。
     */
    class SseHub : public std::enable_shared_from_this<SseHub>
    {
    public:
        /**
         * @brief 构造 Hub 并关联 io_context（供 dispatch 写操作）。
         */
        explicit SseHub(boost::asio::io_context& ioc);

        ~SseHub();

        // 禁止拷贝/移动
        SseHub(const SseHub&) = delete;
        SseHub& operator=(const SseHub&) = delete;
        SseHub(SseHub&&) = delete;
        SseHub& operator=(SseHub&&) = delete;

        /**
         * @brief 注册一个 SSE 会话到指定 key。
         *
         * @param session 弱引用，Hub 不延长会话生命周期。
         * @param key 业务任务标识（如更新任务的 entry id）。
         */
        void Register(std::weak_ptr<SseSessionBase> session, std::string key);

        /**
         * @brief 注销一个 SSE 会话（连接断开时调用）。
         *
         * 精确匹配移除；移除后该 key 订阅归零时触发 key-empty 回调。
         *
         * @param session 原始指针，用于精确匹配。
         * @param key 会话所在 key。
         */
        void Unregister(SseSessionBase* session, const std::string& key);

        /**
         * @brief 按 key 广播事件 payload 到该 key 的所有订阅者。
         *
         * 写操作通过 dispatch 到各会话的 executor（strand）串行执行，
         * 确保与 SSE 流并发写安全（对齐 NotificationHub::Broadcast）。
         *
         * @param key 目标 key。
         * @param payload 事件负载（会话组装成 "data: {json}\n\n" chunk）。
         */
        void Broadcast(
            const std::string&                 key,
            std::shared_ptr<const std::string> payload);

        /**
         * @brief 获取某 key 当前订阅会话数（用于诊断）。
         */
        size_t SubscriberCount(const std::string& key) const;

        /**
         * @brief 设置 key 订阅归零回调（业务中立，只带 key，不感知进度）。
         */
        void SetOnKeyEmpty(
            std::function<void(const std::string&)> on_key_empty);

        /**
         * @brief 设置 key-empty 宽限期（缺省 60s；测试注入毫秒级驱动）。
         *
         * 归零到触发之间留重连窗口：期间订阅者回归即取消清理。
         */
        void SetKeyEmptyGrace(std::chrono::milliseconds grace);

        /**
         * @brief 移除已失效（weak_ptr expired）的会话，不触发 key-empty。
         */
        void CleanupStaleConnections();

        /**
         * @brief 使 hub 失效：此后 Register/Unregister/Broadcast 全部 no-op。
         *
         * 关闭流程中先无效化 hub 再析构 HTTP 资源（io_context），确保 IPC
         * 线程晚到的 Report 经 Broadcast 不再 dispatch 到已析构的 io_context。
         */
        void Invalidate() noexcept { invalidated_.store(true); }

        boost::asio::io_context& IoCtx() noexcept { return ioc_; }

    private:
        /**
         * @brief 归零宽限定时器排程（fire-and-check，动作幂等无需 cancel
         * 簿记）。
         */
        void ScheduleKeyEmptyNotification(const std::string& key);

        boost::asio::io_context&  ioc_;
        std::atomic<bool>         invalidated_{false};
        mutable std::shared_mutex mutex_;
        std::unordered_map<
            std::string,
            std::vector<std::weak_ptr<SseSessionBase>>>
                                                sessions_by_key_;
        std::function<void(const std::string&)> on_key_empty_;
        std::chrono::milliseconds key_empty_grace_{std::chrono::seconds(60)};
        // 待触发的宽限定时器（同 key 排程去重）。定时器经 handler 持有
        // 自身 shared_ptr 存活，map 条目仅做去重与身份比对。
        std::unordered_map<
            std::string,
            std::shared_ptr<boost::asio::steady_timer>>
            pending_key_empty_;
    };

} // namespace Das::Http

#endif // DAS_HTTP_SSE_HUB_HPP
