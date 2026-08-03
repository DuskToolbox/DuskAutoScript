#include "SseHub.hpp"
#include "beast/SseSession.hpp" // for SseSessionBase full definition

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
        const std::unique_lock lock(mutex_);
        sessions_by_key_[std::move(key)].push_back(std::move(session));
    }

    void SseHub::Unregister(SseSessionBase* session, const std::string& key)
    {
        if (invalidated_.load())
        {
            return;
        }
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
            if (on_key_empty_)
            {
                // key-empty 反向通知含跨进程同步 IPC（DasHttp → DasMaaPi），
                // 在 SseSession 的 strand 线程上执行会阻塞该 strand 上全部
                // SSE 会话的写（所有连接共享同一 strand）——post 到 hub 的
                // io_context 全局队列，由任意空闲 worker 执行，脱离 strand
                // 串行化约束。
                auto cb = on_key_empty_;
                boost::asio::post(ioc_, [cb, key]() { cb(key); });
            }
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
