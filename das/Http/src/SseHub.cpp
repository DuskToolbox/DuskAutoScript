#include "SseHub.hpp"
#include "beast/SseSession.hpp" // for SseSessionBase full definition

#include <das/DasApi.h>
#include <das/Utils/fmt.h>

#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>

namespace Das::Http
{
    SseHub::SseHub(boost::asio::io_context& ioc) : ioc_(ioc) {}

    SseHub::~SseHub() = default;

    void SseHub::Register(
        std::weak_ptr<SseSessionBase> session,
        std::string                   key)
    {
        if (invalidated_.load())
        {
            return;
        }
        DAS_LOG_INFO(
            DAS_FMT_NS::format(
                "SseHub: subscriber registered key={} session={}",
                key,
                static_cast<const void*>(session.lock().get()))
                .c_str());
        const std::unique_lock lock(mutex_);
        sessions_by_key_[std::move(key)].push_back(std::move(session));
    }

    void SseHub::Unregister(SseSessionBase* session, const std::string& key)
    {
        if (invalidated_.load())
        {
            return;
        }
        DAS_LOG_INFO(
            DAS_FMT_NS::format(
                "SseHub: subscriber unregistered key={} session={}",
                key,
                static_cast<const void*>(session))
                .c_str());
        bool key_empty = false;
        {
            const std::unique_lock lock(mutex_);
            auto                   it = sessions_by_key_.find(key);
            if (it == sessions_by_key_.end())
            {
                return;
            }
            auto& sessions = it->second;
            sessions.erase(
                std::remove_if(
                    sessions.begin(),
                    sessions.end(),
                    [session](const std::weak_ptr<SseSessionBase>& weak)
                    {
                        auto sp = weak.lock();
                        return !sp || sp.get() == session;
                    }),
                sessions.end());
            if (sessions.empty())
            {
                sessions_by_key_.erase(it);
                key_empty = true;
            }
        }
        if (key_empty)
        {
            ScheduleKeyEmptyNotification(key);
        }
    }

    void SseHub::Broadcast(
        const std::string&                 key,
        std::shared_ptr<const std::string> payload)
    {
        if (invalidated_.load())
        {
            return;
        }
        const std::shared_lock lock(mutex_);
        auto                   it = sessions_by_key_.find(key);
        if (it == sessions_by_key_.end())
        {
            return;
        }
        for (const auto& weak : it->second)
        {
            if (auto session = weak.lock())
            {
                // Dispatch 写操作到会话的 executor（strand），防并发写
                boost::asio::dispatch(
                    session->GetExecutor(),
                    [session, payload]() { session->WriteData(payload); });
            }
        }
    }

    size_t SseHub::SubscriberCount(const std::string& key) const
    {
        const std::shared_lock lock(mutex_);
        auto                   it = sessions_by_key_.find(key);
        if (it == sessions_by_key_.end())
        {
            return 0;
        }
        return it->second.size();
    }

    void SseHub::SetOnKeyEmpty(
        std::function<void(const std::string&)> on_key_empty)
    {
        const std::unique_lock lock(mutex_);
        on_key_empty_ = std::move(on_key_empty);
    }

    void SseHub::SetKeyEmptyGrace(std::chrono::milliseconds grace)
    {
        const std::unique_lock lock(mutex_);
        key_empty_grace_ = grace;
    }

    void SseHub::ScheduleKeyEmptyNotification(const std::string& key)
    {
        if (invalidated_.load())
        {
            return;
        }
        std::shared_ptr<boost::asio::steady_timer> timer;
        {
            const std::unique_lock lock(mutex_);
            // 同 key 已有待触发定时器：到点复查自会覆盖（归零→回归→再归零
            // 的二次宽限可能短于全值，可接受——首排程已含一次完整窗口）。
            if (pending_key_empty_.contains(key))
            {
                return;
            }
            timer = std::make_shared<boost::asio::steady_timer>(
                ioc_,
                key_empty_grace_);
            pending_key_empty_[key] = timer;
        }
        DAS_LOG_INFO(
            DAS_FMT_NS::format(
                "SseHub: key-empty grace scheduled key={} grace_ms={}",
                key,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    key_empty_grace_)
                    .count())
                .c_str());
        // handler 持 self + timer：hub 与定时器存活到触发为止，销毁序无关。
        auto self = shared_from_this();
        timer->async_wait(
            [self = std::move(self), key, timer](
                const boost::system::error_code& ec)
            {
                {
                    const std::unique_lock lock(self->mutex_);
                    // 身份比对防误删后续排程的新定时器。
                    const auto it = self->pending_key_empty_.find(key);
                    if (it != self->pending_key_empty_.end()
                        && it->second == timer)
                    {
                        self->pending_key_empty_.erase(it);
                    }
                }
                // 取消（关停/销毁）/失效/宽限期内订阅者回归：什么都不发生。
                if (ec || self->invalidated_.load()
                    || self->SubscriberCount(key) > 0)
                {
                    DAS_LOG_INFO(
                        DAS_FMT_NS::format(
                            "SseHub: key-empty grace skipped key={} ec={} "
                            "subscribers={}",
                            key,
                            ec.message(),
                            self->SubscriberCount(key))
                            .c_str());
                    return;
                }
                DAS_LOG_INFO(
                    DAS_FMT_NS::format(
                        "SseHub: key-empty grace elapsed key={}",
                        key)
                        .c_str());
                if (self->on_key_empty_)
                {
                    // key-empty 反向通知含跨进程同步 IPC（DasHttp →
                    // DasMaaPi）， 直连会占住当前 worker 直到对端返回——post 到
                    // hub 的 io_context 全局队列，由任意空闲 worker 执行。
                    auto cb = self->on_key_empty_;
                    boost::asio::post(self->ioc_, [cb, key]() { cb(key); });
                }
            });
    }

    void SseHub::CleanupStaleConnections()
    {
        const std::unique_lock lock(mutex_);
        for (auto it = sessions_by_key_.begin(); it != sessions_by_key_.end();)
        {
            auto& sessions = it->second;
            sessions.erase(
                std::remove_if(
                    sessions.begin(),
                    sessions.end(),
                    [](const std::weak_ptr<SseSessionBase>& weak)
                    { return weak.expired(); }),
                sessions.end());
            if (sessions.empty())
            {
                it = sessions_by_key_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

} // namespace Das::Http
