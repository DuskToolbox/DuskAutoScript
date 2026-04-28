#include "NotificationHub.hpp"
#include "beast/Server.hpp" // for WsSession full definition

#include <boost/asio/dispatch.hpp>

namespace Das::Http
{

    NotificationHub::NotificationHub(boost::asio::io_context& ioc) : ioc_(ioc)
    {
    }

    NotificationHub::~NotificationHub()
    {
        // 不负责关闭连接，WsSession 自行管理生命周期
    }

    void NotificationHub::Register(std::weak_ptr<WsSession> session)
    {
        std::unique_lock lock(mutex_);
        sessions_.push_back(std::move(session));
    }

    void NotificationHub::Unregister(WsSession* session)
    {
        std::unique_lock lock(mutex_);
        sessions_.erase(
            std::remove_if(
                sessions_.begin(),
                sessions_.end(),
                [session](const std::weak_ptr<WsSession>& weak)
                {
                    auto sp = weak.lock();
                    return !sp || sp.get() == session;
                }),
            sessions_.end());
    }

    void NotificationHub::Broadcast(std::string message)
    {
        auto shared_msg = std::make_shared<std::string>(std::move(message));

        std::shared_lock lock(mutex_);
        for (const auto& weak : sessions_)
        {
            if (auto session = weak.lock())
            {
                // Dispatch 写操作到 websocket stream 的 executor（strand）
                boost::asio::dispatch(
                    session->GetExecutor(),
                    [session, shared_msg]()
                    { session->WriteMessage(shared_msg); });
            }
        }
    }

    size_t NotificationHub::ActiveConnectionCount() const
    {
        std::shared_lock lock(mutex_);
        return sessions_.size();
    }

    void NotificationHub::CleanupStaleConnections()
    {
        std::unique_lock lock(mutex_);
        sessions_.erase(
            std::remove_if(
                sessions_.begin(),
                sessions_.end(),
                [](const std::weak_ptr<WsSession>& weak)
                { return weak.expired(); }),
            sessions_.end());
    }

} // namespace Das::Http
