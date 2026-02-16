#include <das/Core/IPC/ForwardingRouter.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <gtest/gtest.h>

using DAS::Core::IPC::ForwardingRouter;
using DAS::Core::IPC::IPCMessageHeader;
using DAS::Core::IPC::MessageType;
using DAS::Core::IPC::RouteKey;
using DAS::Core::IPC::RouteTarget;

class ForwardingRouterTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        target1_ = RouteTarget(1, 100, 200, DasGuid{});
        target2_ = RouteTarget(2, 200, 300, DasGuid{});
        target3_ = RouteTarget(3, 300, 400, DasGuid{});

        key1_ = RouteKey(100, 200, 300, 400);
        key2_ = RouteKey(200, 300, 400, 500);
        key3_ = RouteKey(300, 400, 500, 600);
        key4_ = RouteKey(400, 500, 600, 700);

        header1_ = IPCMessageHeader{};
        header1_.call_id = 1;
        header1_.message_type = static_cast<uint8_t>(MessageType::REQUEST);
        header1_.error_code = 0;
        header1_.interface_id = 200;
        header1_.session_id = 1;
        header1_.generation = 1;
        header1_.local_id = 100;
        header1_.version = 2;
        header1_.flags = 0;
        header1_.body_size = 0;

        header2_ = IPCMessageHeader{};
        header2_.call_id = 2;
        header2_.message_type = static_cast<uint8_t>(MessageType::RESPONSE);
        header2_.error_code = 0;
        header2_.interface_id = 300;
        header2_.session_id = 1;
        header2_.generation = 1;
        header2_.local_id = 200;
        header2_.version = 2;
        header2_.flags = 0;
        header2_.body_size = 0;

        payload_ = std::vector<uint8_t>{1, 2, 3, 4, 5};
    }

    RouteTarget          target1_, target2_, target3_;
    RouteKey             key1_, key2_, key3_, key4_;
    IPCMessageHeader     header1_, header2_;
    std::vector<uint8_t> payload_;
};

// ====== Constructor and Basic Functions Tests ======

TEST_F(ForwardingRouterTest, ConstructorAndBasicFunctions)
{
    ForwardingRouter router;

    EXPECT_EQ(router.GetRouteCount(), 0);
    EXPECT_FALSE(router.HasRoute(key1_));
    EXPECT_FALSE(router.HasRoute(key2_));

    ForwardingRouter::RouteStats stats = router.GetStats();
    EXPECT_EQ(stats.total_routes, 0);
    EXPECT_EQ(stats.successful_routes, 0);
    EXPECT_EQ(stats.failed_routes, 0);
}

// 测试添加路由
TEST_F(ForwardingRouterTest, AddRoute)
{
    ForwardingRouter router;

    // 添加第一个路由
    EXPECT_TRUE(router.AddRoute(key1_, target1_));
    EXPECT_EQ(router.GetRouteCount(), 1);
    EXPECT_TRUE(router.HasRoute(key1_));
    EXPECT_FALSE(router.HasRoute(key2_));

    // 添加第二个路由
    EXPECT_TRUE(router.AddRoute(key2_, target2_));
    EXPECT_EQ(router.GetRouteCount(), 2);
    EXPECT_TRUE(router.HasRoute(key1_));
    EXPECT_TRUE(router.HasRoute(key2_));

    // 添加无效的路由
    RouteTarget invalid_target;
    EXPECT_FALSE(router.AddRoute(key3_, invalid_target));
    EXPECT_EQ(router.GetRouteCount(), 2);
}

// 测试查找目标
TEST_F(ForwardingRouterTest, FindTarget)
{
    ForwardingRouter router;

    // 在空路由表中查找
    RouteTarget found_target;
    EXPECT_FALSE(router.FindTarget(key1_, found_target));

    // 添加路由后查找
    EXPECT_TRUE(router.AddRoute(key1_, target1_));
    EXPECT_TRUE(router.FindTarget(key1_, found_target));
    EXPECT_EQ(found_target.session_id, target1_.session_id);
    EXPECT_EQ(found_target.object_id, target1_.object_id);
    EXPECT_EQ(found_target.interface_id, target1_.interface_id);
    EXPECT_TRUE(found_target.is_valid);

    // 查找不存在的键
    EXPECT_FALSE(router.FindTarget(key4_, found_target));
}

// 测试查找所有目标
TEST_F(ForwardingRouterTest, FindAllTargets)
{
    ForwardingRouter router;

    // 空路由表
    auto targets = router.FindAllTargets();
    EXPECT_EQ(targets.size(), 0);

    // 添加路由
    EXPECT_TRUE(router.AddRoute(key1_, target1_));
    EXPECT_TRUE(router.AddRoute(key2_, target2_));

    targets = router.FindAllTargets();
    EXPECT_EQ(targets.size(), 2);

    // 验证目标
    bool found1 = false, found2 = false;
    for (const auto& target : targets)
    {
        if (target.session_id == target1_.session_id)
        {
            found1 = true;
        }
        if (target.session_id == target2_.session_id)
        {
            found2 = true;
        }
    }
    EXPECT_TRUE(found1);
    EXPECT_TRUE(found2);
}

// 测试删除路由
TEST_F(ForwardingRouterTest, RemoveRoute)
{
    ForwardingRouter router;

    // 添加路由
    EXPECT_TRUE(router.AddRoute(key1_, target1_));
    EXPECT_TRUE(router.AddRoute(key2_, target2_));
    EXPECT_EQ(router.GetRouteCount(), 2);

    // 删除存在的路由
    EXPECT_TRUE(router.RemoveRoute(key1_));
    EXPECT_EQ(router.GetRouteCount(), 1);
    EXPECT_FALSE(router.HasRoute(key1_));
    EXPECT_TRUE(router.HasRoute(key2_));

    // 删除不存在的路由
    EXPECT_FALSE(router.RemoveRoute(key4_));
    EXPECT_EQ(router.GetRouteCount(), 1);

    // 删除最后一个路由
    EXPECT_TRUE(router.RemoveRoute(key2_));
    EXPECT_EQ(router.GetRouteCount(), 0);
}

// 测试清空路由表
TEST_F(ForwardingRouterTest, ClearRoutes)
{
    ForwardingRouter router;

    // 添加路由
    EXPECT_TRUE(router.AddRoute(key1_, target1_));
    EXPECT_TRUE(router.AddRoute(key2_, target2_));
    EXPECT_EQ(router.GetRouteCount(), 2);

    // 清空路由表
    router.ClearRoutes();
    EXPECT_EQ(router.GetRouteCount(), 0);
    EXPECT_FALSE(router.HasRoute(key1_));
    EXPECT_FALSE(router.HasRoute(key2_));

    // 清空后不能再找到目标
    RouteTarget found_target;
    EXPECT_FALSE(router.FindTarget(key1_, found_target));
}

// 测试更新路由
TEST_F(ForwardingRouterTest, UpdateRoute)
{
    ForwardingRouter router;

    // 添加初始路由
    EXPECT_TRUE(router.AddRoute(key1_, target1_));

    // 获取统计信息
    ForwardingRouter::RouteStats stats1 = router.GetStats();
    EXPECT_EQ(stats1.total_routes, 1);

    // 添加新的路由，应该更新现有的
    RouteTarget updated_target(5, 100, 200, DasGuid{});
    EXPECT_TRUE(router.AddRoute(key1_, updated_target));
    EXPECT_EQ(router.GetRouteCount(), 1);

    // 验证更新后的目标
    RouteTarget found_target;
    EXPECT_TRUE(router.FindTarget(key1_, found_target));
    EXPECT_EQ(found_target.session_id, 5);
    EXPECT_EQ(found_target.object_id, 100);
    EXPECT_EQ(found_target.interface_id, 200);
}

// 测试消息路由
TEST_F(ForwardingRouterTest, RouteMessage)
{
    ForwardingRouter router;

    // 路由到不存在的目标
    auto result = router.RouteMessage(header1_, payload_);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.target.is_valid);
    EXPECT_FALSE(result.error_message.empty());

    // 添加路由
    EXPECT_TRUE(router.AddRoute(key1_, target1_));

    // 路由到存在的目标
    result = router.RouteMessage(header1_, payload_);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.target.is_valid);
    EXPECT_EQ(result.target.session_id, target1_.session_id);
    EXPECT_EQ(result.target.object_id, target1_.object_id);
    EXPECT_EQ(result.target.interface_id, target1_.interface_id);
    EXPECT_TRUE(result.error_message.empty());

    // 路由到另一个目标
    EXPECT_TRUE(router.AddRoute(key2_, target2_));
    result = router.RouteMessage(header2_, payload_);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.target.is_valid);
    EXPECT_EQ(result.target.session_id, target2_.session_id);
    EXPECT_EQ(result.target.object_id, target2_.object_id);
    EXPECT_EQ(result.target.interface_id, target2_.interface_id);
}

// 测试路由统计
TEST_F(ForwardingRouterTest, RouteStats)
{
    ForwardingRouter router;

    // 初始统计
    ForwardingRouter::RouteStats stats = router.GetStats();
    EXPECT_EQ(stats.total_routes, 0);
    EXPECT_EQ(stats.successful_routes, 0);
    EXPECT_EQ(stats.failed_routes, 0);

    // 添加路由
    EXPECT_TRUE(router.AddRoute(key1_, target1_));

    // 成功的路由
    auto result = router.RouteMessage(header1_, payload_);
    EXPECT_TRUE(result.success);

    // 检查统计
    stats = router.GetStats();
    EXPECT_EQ(stats.total_routes, 1);
    EXPECT_EQ(stats.successful_routes, 1);
    EXPECT_EQ(stats.failed_routes, 0);

    // 失败的路由
    auto result2 = router.RouteMessage(header2_, payload_);
    EXPECT_FALSE(result2.success);

    // 检查统计更新
    stats = router.GetStats();
    EXPECT_EQ(stats.total_routes, 1);
    EXPECT_EQ(stats.successful_routes, 1);
    EXPECT_EQ(stats.failed_routes, 1);
}

// 测试路由键比较和哈希
TEST_F(ForwardingRouterTest, RouteKeyComparisonAndHash)
{
    RouteKey key1a(100, 200, 300, 400);
    RouteKey key1b(100, 200, 300, 400);
    RouteKey key2(200, 300, 400, 500);

    // 相同的键应该相等
    EXPECT_TRUE(key1a == key1b);
    EXPECT_FALSE(key1a == key2);

    // 哈希值应该一致
    EXPECT_EQ(key1a.hash(), key1b.hash());
    EXPECT_NE(key1a.hash(), key2.hash());
}

// 测试路由目标构造函数
TEST_F(ForwardingRouterTest, RouteTargetConstructors)
{
    // 默认构造函数
    RouteTarget default_target;
    EXPECT_EQ(default_target.session_id, 0);
    EXPECT_EQ(default_target.object_id, 0);
    EXPECT_EQ(default_target.interface_id, 0);
    EXPECT_FALSE(default_target.is_valid);

    RouteTarget custom_target(1, 100, 200, DasGuid{});
    EXPECT_EQ(custom_target.session_id, 1);
    EXPECT_EQ(custom_target.object_id, 100);
    EXPECT_EQ(custom_target.interface_id, 200);
    EXPECT_TRUE(custom_target.is_valid);
}