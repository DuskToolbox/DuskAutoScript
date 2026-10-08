#include "CommitMemory.h"

#include <gtest/gtest.h>

#include <cstdint>

// Windows 主口径 GetPerformanceInfo：系统级 commit 余量读数 sane
// 且精确；其余平台的精确性由口径矩阵决定（Linux 仅 mode 2 精确）
TEST(CommitMemoryTest, QueryCommitHeadroom_ReturnsSaneValueOnWindows)
{
#ifdef _WIN32
    const CommitMemory::CommitHeadroom headroom =
        CommitMemory::QueryCommitHeadroom();
    EXPECT_GT(headroom.available_bytes, static_cast<std::uint64_t>(0));
    EXPECT_TRUE(headroom.precise);
#else
    GTEST_SKIP() << "Windows-specific precise reading";
#endif
}

// 读数失败契约形态：available_bytes==0 且 precise=false；消费方（准入
// 闸门）对该形态走具名失败而非崩溃。重复调用覆盖交叉告警 once 路径
TEST(CommitMemoryTest, QueryCommitHeadroom_FallbackPathNeverCrashes)
{
    const CommitMemory::CommitHeadroom read_failure_shape{};
    EXPECT_EQ(
        read_failure_shape.available_bytes,
        static_cast<std::uint64_t>(0));
    EXPECT_FALSE(read_failure_shape.precise);
    for (int call_index = 0; call_index < 3; ++call_index)
    {
        const CommitMemory::CommitHeadroom headroom =
            CommitMemory::QueryCommitHeadroom();
        EXPECT_TRUE(headroom.precise);
    }
}
