#include "SseHub.hpp"
#include "beast/SseSession.hpp" // for SseSessionBase

#include <boost/asio/any_io_executor.hpp>
#include <gtest/gtest.h>

#include <chrono>
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
        auto                    hub = std::make_shared<Das::Http::SseHub>(ioc);
        // 测试注入毫秒级宽限（生产缺省 60s），run_for 驱动定时器路径。
        hub->SetKeyEmptyGrace(std::chrono::milliseconds(1));

        std::vector<std::string> fired_keys;
        hub->SetOnKeyEmpty([&fired_keys](const std::string& key)
                           { fired_keys.push_back(key); });

        auto session = std::make_shared<FakeSession>(ioc, "k1");
        hub->Register(session, "k1");
        ASSERT_EQ(hub->SubscriberCount("k1"), 1u);

        hub->Unregister(session.get(), "k1");

        EXPECT_EQ(hub->SubscriberCount("k1"), 0u);
        // key-empty 经宽限定时器 + post 两级异步，run_for 驱动到触发。
        ioc.run_for(std::chrono::milliseconds(100));
        ASSERT_EQ(fired_keys.size(), 1u);
        EXPECT_EQ(fired_keys[0], "k1");
    }

    // 宽限期内订阅者回归（断线重连形态）→ 不触发 key-empty：业务侧快照与
    // 生产不被连接抖动误清；回归后再归零仍按新一轮排程触发。
    TEST(SseHubTest, ReconnectWithinGrace_DoesNotFireKeyEmpty)
    {
        boost::asio::io_context ioc;
        auto                    hub = std::make_shared<Das::Http::SseHub>(ioc);
        hub->SetKeyEmptyGrace(std::chrono::milliseconds(50));

        int empty_fires = 0;
        hub->SetOnKeyEmpty([&empty_fires](const std::string&)
                           { ++empty_fires; });

        auto session = std::make_shared<FakeSession>(ioc, "k1");
        hub->Register(session, "k1");
        hub->Unregister(session.get(), "k1");
        // 宽限窗口内订阅者回归。
        hub->Register(session, "k1");
        ioc.run_for(std::chrono::milliseconds(200));
        EXPECT_EQ(empty_fires, 0);

        // 回归后再归零：fire 点复查生效，正常触发。run_for 因工作耗尽返回
        // 后 io_context 进入 stopped 态，二次驱动前须 restart。
        hub->Unregister(session.get(), "k1");
        ioc.restart();
        ioc.run_for(std::chrono::milliseconds(200));
        EXPECT_EQ(empty_fires, 1);
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
