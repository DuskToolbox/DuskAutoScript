#include "SseHub.hpp"
#include "beast/SseSession.hpp" // for SseSessionBase

#include <boost/asio/any_io_executor.hpp>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace
{
    // 轻量 fake SSE 会话：只记录广播消息，供 SseHub 并发/生命周期测试。
    class FakeSession : public Das::Http::Beast::SseSessionBase
    {
    public:
        FakeSession(boost::asio::io_context& ioc, std::string key)
            : key_(std::move(key)), ex_(ioc.get_executor())
        {
        }

        boost::asio::any_io_executor GetExecutor() override { return ex_; }

        void WriteData(std::shared_ptr<const std::string> payload) override
        {
            messages_.push_back(std::move(payload));
        }

        const std::string& Key() const noexcept override { return key_; }

        std::string                                     key_;
        boost::asio::any_io_executor                    ex_;
        std::vector<std::shared_ptr<const std::string>> messages_;
    };

    TEST(SseHubTest, BroadcastOnlyReachesKeySubscribers)
    {
        boost::asio::io_context ioc;
        Das::Http::SseHub       hub(ioc);

        auto key_a = std::make_shared<FakeSession>(ioc, "keyA");
        auto key_b = std::make_shared<FakeSession>(ioc, "keyB");
        hub.Register(key_a, "keyA");
        hub.Register(key_b, "keyB");

        hub.Broadcast("keyA", std::make_shared<const std::string>("hello"));
        ioc.poll();

        ASSERT_EQ(key_a->messages_.size(), 1u);
        EXPECT_EQ(*key_a->messages_[0], "hello");
        EXPECT_EQ(key_b->messages_.size(), 0u);
    }

    TEST(SseHubTest, SameKeyMultipleSubscribers_AllReceive)
    {
        boost::asio::io_context ioc;
        Das::Http::SseHub       hub(ioc);

        auto sub1 = std::make_shared<FakeSession>(ioc, "k1");
        auto sub2 = std::make_shared<FakeSession>(ioc, "k1");
        hub.Register(sub1, "k1");
        hub.Register(sub2, "k1");

        hub.Broadcast("k1", std::make_shared<const std::string>("data"));
        ioc.poll();

        EXPECT_EQ(sub1->messages_.size(), 1u);
        EXPECT_EQ(sub2->messages_.size(), 1u);
    }

    TEST(SseHubTest, UnregisterToZero_FiresKeyEmptyWithKey)
    {
        boost::asio::io_context ioc;
        Das::Http::SseHub       hub(ioc);

        std::vector<std::string> fired_keys;
        hub.SetOnKeyEmpty([&fired_keys](const std::string& key)
                          { fired_keys.push_back(key); });

        auto session = std::make_shared<FakeSession>(ioc, "k1");
        hub.Register(session, "k1");
        ASSERT_EQ(hub.SubscriberCount("k1"), 1u);

        hub.Unregister(session.get(), "k1");

        EXPECT_EQ(hub.SubscriberCount("k1"), 0u);
        ASSERT_EQ(fired_keys.size(), 1u);
        EXPECT_EQ(fired_keys[0], "k1");
    }

    TEST(SseHubTest, CleanupStaleConnections_RemovesExpired_NoKeyEmptyFiring)
    {
        boost::asio::io_context ioc;
        Das::Http::SseHub       hub(ioc);

        int empty_fires = 0;
        hub.SetOnKeyEmpty([&empty_fires](const std::string&)
                          { ++empty_fires; });

        {
            auto session = std::make_shared<FakeSession>(ioc, "k1");
            hub.Register(session, "k1");
            ASSERT_EQ(hub.SubscriberCount("k1"), 1u);
        } // session 析构 → weak_ptr expired

        hub.CleanupStaleConnections();

        EXPECT_EQ(hub.SubscriberCount("k1"), 0u);
        // CleanupStaleConnections 不触发 key-empty（避免误报）
        EXPECT_EQ(empty_fires, 0);
    }
} // namespace
