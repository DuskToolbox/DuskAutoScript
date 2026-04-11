/**
 * @file ProxyFactoryTest.cpp
 * @brief ProxyFactory 单元测试
 *
 * ProxyFactory 已从单例改为构造函数注入模式。
 * 由于 ProxyFactory 依赖 IpcRunLoop（构造复杂），
 * 这里仅测试不依赖 IpcRunLoop 的基础行为。
 * 完整行为通过集成测试覆盖。
 */

#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/DasTypes.hpp>
#include <das/IDasBase.h>
#include <gtest/gtest.h>
#include <memory>

using DAS::Core::IPC::DecodeObjectId;
using DAS::Core::IPC::DistributedObjectManager;
using DAS::Core::IPC::EncodeObjectId;
using DAS::Core::IPC::IPCProxyBase;
using DAS::Core::IPC::ObjectId;
using DAS::Core::IPC::ProxyFactory;
using DAS::Core::IPC::RemoteObjectRegistry;

// Test ObjectId 编码解码后的状态一致性
TEST(ProxyFactoryTest, ObjectIdEncodingConsistency)
{
    ObjectId original{.session_id = 123, .generation = 456, .local_id = 789012};

    // 编码
    uint64_t encoded = EncodeObjectId(original);

    // 解码
    ObjectId decoded = DecodeObjectId(encoded);

    // 验证解码后的值与原始值一致
    EXPECT_EQ(decoded.session_id, original.session_id);
    EXPECT_EQ(decoded.generation, original.generation);
    EXPECT_EQ(decoded.local_id, original.local_id);
}

// Test 边界值的 ObjectId
TEST(ProxyFactoryTest, ObjectIdWithBoundaryValues)
{
    ObjectId max_values{
        .session_id = 0xFFFF,
        .generation = 0xFFFF,
        .local_id = 0xFFFFFFFF};

    // 编码解码
    uint64_t encoded = EncodeObjectId(max_values);
    ObjectId decoded = DecodeObjectId(encoded);

    EXPECT_EQ(decoded.session_id, max_values.session_id);
    EXPECT_EQ(decoded.generation, max_values.generation);
    EXPECT_EQ(decoded.local_id, max_values.local_id);
}

// Test 零值的 ObjectId
TEST(ProxyFactoryTest, ObjectIdWithZeroValues)
{
    ObjectId zero{.session_id = 0, .generation = 0, .local_id = 0};

    // 零值对象的编码应该为0
    uint64_t encoded = EncodeObjectId(zero);
    EXPECT_EQ(encoded, 0);

    // 解码后仍为零值
    ObjectId decoded = DecodeObjectId(encoded);
    EXPECT_EQ(decoded.session_id, 0);
    EXPECT_EQ(decoded.generation, 0);
    EXPECT_EQ(decoded.local_id, 0);
}
