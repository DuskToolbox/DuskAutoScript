#include <das/Core/IPC/SessionCoordinator.h>
#include <gtest/gtest.h>

using DAS::Core::IPC::SessionCoordinator;

// 测试单例模式的获取
TEST(SessionCoordinatorTest, GetInstance)
{
    SessionCoordinator& instance1 = SessionCoordinator::GetInstance();
    SessionCoordinator& instance2 = SessionCoordinator::GetInstance();

    // 同一个实例
    EXPECT_EQ(&instance1, &instance2);
}

// 测试 AllocateSessionId 和 ReleaseSessionId
TEST(SessionCoordinatorTest, AllocateAndReleaseSessionId)
{
    SessionCoordinator& coordinator = SessionCoordinator::GetInstance();

    // 分配 session_id
    uint16_t session_id1 = coordinator.AllocateSessionId();
    EXPECT_NE(session_id1, 0); // 应该分配到有效的 session_id
    EXPECT_TRUE(coordinator.IsSessionIdAllocated(session_id1));

    // 再次分配 session_id
    uint16_t session_id2 = coordinator.AllocateSessionId();
    EXPECT_NE(session_id2, 0);
    EXPECT_NE(session_id1, session_id2); // 应该分配不同的 session_id
    EXPECT_TRUE(coordinator.IsSessionIdAllocated(session_id2));

    // 释放 session_id
    coordinator.ReleaseSessionId(session_id1);
    EXPECT_FALSE(coordinator.IsSessionIdAllocated(session_id1));

    // 释放 session_id
    coordinator.ReleaseSessionId(session_id2);
    EXPECT_FALSE(coordinator.IsSessionIdAllocated(session_id2));
}

// 测试 SetLocalSessionId 和 GetLocalSessionId
TEST(SessionCoordinatorTest, LocalSessionId)
{
    SessionCoordinator& coordinator = SessionCoordinator::GetInstance();

    // 设置本地 session_id
    coordinator.SetLocalSessionId(100);
    EXPECT_EQ(coordinator.GetLocalSessionId(), 100);

    // 修改本地 session_id
    coordinator.SetLocalSessionId(200);
    EXPECT_EQ(coordinator.GetLocalSessionId(), 200);

    // 设置无效的 session_id（保留值）
    coordinator.SetLocalSessionId(0);              // 保留值
    EXPECT_NE(coordinator.GetLocalSessionId(), 0); // 不应该设置成功

    coordinator.SetLocalSessionId(1);              // 主进程，保留值
    EXPECT_NE(coordinator.GetLocalSessionId(), 1); // 不应该设置成功

    coordinator.SetLocalSessionId(0xFFFF);              // 保留值
    EXPECT_NE(coordinator.GetLocalSessionId(), 0xFFFF); // 不应该设置成功
}

// 测试 IsValidSessionId
TEST(SessionCoordinatorTest, IsValidSessionId)
{
    SessionCoordinator& coordinator = SessionCoordinator::GetInstance();

    // 测试无效的 session_id（保留值）
    EXPECT_FALSE(coordinator.IsValidSessionId(0));      // 保留值
    EXPECT_FALSE(coordinator.IsValidSessionId(1));      // 主进程，保留值
    EXPECT_FALSE(coordinator.IsValidSessionId(0xFFFF)); // 保留值

    // 测试有效的 session_id
    EXPECT_TRUE(coordinator.IsValidSessionId(2));      // 最小的有效值
    EXPECT_TRUE(coordinator.IsValidSessionId(0xFFFE)); // 最大的有效值
    EXPECT_TRUE(coordinator.IsValidSessionId(1000));   // 中间的值
    EXPECT_TRUE(coordinator.IsValidSessionId(32767));  // 大值
}

// 测试重复分配和释放
TEST(SessionCoordinatorTest, MultipleAllocateAndRelease)
{
    SessionCoordinator& coordinator = SessionCoordinator::GetInstance();

    std::vector<uint16_t> allocated_ids;

    // 分配多个 session_id
    for (int i = 0; i < 10; i++)
    {
        uint16_t session_id = coordinator.AllocateSessionId();
        EXPECT_NE(session_id, 0);
        EXPECT_TRUE(coordinator.IsValidSessionId(session_id));
        EXPECT_TRUE(coordinator.IsSessionIdAllocated(session_id));

        // 检查是否重复分配
        for (uint16_t existing_id : allocated_ids)
        {
            EXPECT_NE(session_id, existing_id);
        }

        allocated_ids.push_back(session_id);
    }

    // 释放所有 session_id
    for (uint16_t session_id : allocated_ids)
    {
        coordinator.ReleaseSessionId(session_id);
        EXPECT_FALSE(coordinator.IsSessionIdAllocated(session_id));
    }
}

// 测试分配超过最大数量的 session_id
TEST(SessionCoordinatorTest, AllocateMaxSessionIds)
{
    SessionCoordinator& coordinator = SessionCoordinator::GetInstance();

    std::vector<uint16_t> allocated_ids;
    const size_t          max_attempts = 65536; // 尝试分配所有可能的 session_id

    // 尝试分配尽可能多的 session_id
    for (size_t i = 0; i < max_attempts; i++)
    {
        uint16_t session_id = coordinator.AllocateSessionId();
        if (session_id == 0)
        {
            break; // 没有更多可用的 session_id
        }

        allocated_ids.push_back(session_id);
    }

    // 验证分配的数量是合理的
    EXPECT_GT(allocated_ids.size(), 0);
    EXPECT_LT(allocated_ids.size(), 65536); // 应该少于总数（因为有保留值）

    // 释放所有分配的 session_id
    for (uint16_t session_id : allocated_ids)
    {
        coordinator.ReleaseSessionId(session_id);
    }
}

// 测试线程安全性
TEST(SessionCoordinatorTest, ThreadSafety)
{
    SessionCoordinator& coordinator = SessionCoordinator::GetInstance();

    const int                          num_threads = 10;
    const int                          allocations_per_thread = 5;
    std::vector<std::vector<uint16_t>> thread_allocated_ids(num_threads);
    std::vector<std::thread>           threads;

    // 每个线程分配 session_id
    for (int i = 0; i < num_threads; i++)
    {
        threads.emplace_back(
            [&coordinator, &thread_allocated_ids, i, allocations_per_thread]()
            {
                for (int j = 0; j < allocations_per_thread; j++)
                {
                    uint16_t session_id = coordinator.AllocateSessionId();
                    if (session_id != 0)
                    {
                        thread_allocated_ids[i].push_back(session_id);
                    }
                }
            });
    }

    // 等待所有线程完成
    for (auto& thread : threads)
    {
        thread.join();
    }

    // 验证分配的 session_id 都是唯一的
    std::set<uint16_t> all_allocated_ids;
    for (const auto& thread_ids : thread_allocated_ids)
    {
        for (uint16_t session_id : thread_ids)
        {
            // 检查是否重复分配
            EXPECT_TRUE(
                all_allocated_ids.find(session_id) == all_allocated_ids.end());
            all_allocated_ids.insert(session_id);
        }
    }

    // 释放所有分配的 session_id
    for (const auto& thread_ids : thread_allocated_ids)
    {
        for (uint16_t session_id : thread_ids)
        {
            coordinator.ReleaseSessionId(session_id);
        }
    }
}

// 测试边界的 session_id 值
TEST(SessionCoordinatorTest, BoundaryValues)
{
    SessionCoordinator& coordinator = SessionCoordinator::GetInstance();

    // 测试最小的有效值
    uint16_t session_id = coordinator.AllocateSessionId();
    EXPECT_GE(session_id, 2); // 应该大于等于 2

    if (session_id != 0)
    {
        coordinator.ReleaseSessionId(session_id);
    }
}

// 测试在分配后验证 session_id 状态
TEST(SessionCoordinatorTest, SessionIdStateAfterAllocation)
{
    SessionCoordinator& coordinator = SessionCoordinator::GetInstance();

    uint16_t session_id = coordinator.AllocateSessionId();

    if (session_id != 0)
    {
        // 验证 session_id 状态
        EXPECT_TRUE(coordinator.IsValidSessionId(session_id));
        EXPECT_TRUE(coordinator.IsSessionIdAllocated(session_id));

        // 释放后验证状态
        coordinator.ReleaseSessionId(session_id);
        EXPECT_FALSE(coordinator.IsSessionIdAllocated(session_id));
    }
}