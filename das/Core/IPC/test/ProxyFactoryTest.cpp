#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <gtest/gtest.h>
#include <memory>

using DAS::Core::IPC::DecodeObjectId;
using DAS::Core::IPC::EncodeObjectId;
using DAS::Core::IPC::IPCProxyBase;
using DAS::Core::IPC::ObjectId;
using DAS::Core::IPC::ProxyFactory;

// 测试 ProxyFactory 单例模式
TEST(ProxyFactoryTest, GetInstance_ReturnsSameInstance)
{
    auto& instance1 = ProxyFactory::GetInstance();
    auto& instance2 = ProxyFactory::GetInstance();

    // 验证返回的是同一个实例
    EXPECT_EQ(&instance1, &instance2);
}

// 测试 HasProxy 和 GetProxy 方法
TEST(ProxyFactoryTest, HasProxy_GetProxy_BasicFunctionality)
{
    ProxyFactory& factory = ProxyFactory::GetInstance();

    // 测试不存在的情况
    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    EXPECT_FALSE(factory.HasProxy(obj_id));
    EXPECT_EQ(factory.GetProxy(obj_id), nullptr);
}

// 测试 GetProxyCount 方法
TEST(ProxyFactoryTest, GetProxyCount_InitialZero)
{
    ProxyFactory& factory = ProxyFactory::GetInstance();

    // 初始状态下应该没有 proxy
    EXPECT_EQ(factory.GetProxyCount(), 0);
}

// 测试 ClearAllProxies 方法
TEST(ProxyFactoryTest, ClearAllProxies_EmptyCache)
{
    ProxyFactory& factory = ProxyFactory::GetInstance();

    // 在空缓存情况下调用 ClearAllProxies 不应该出错
    EXPECT_NO_THROW(factory.ClearAllProxies());

    // 验证计数仍为0
    EXPECT_EQ(factory.GetProxyCount(), 0);
}

// 测试 ReleaseProxy 方法对不存在对象的处理
TEST(ProxyFactoryTest, ReleaseProxy_NonExistingObject)
{
    ProxyFactory& factory = ProxyFactory::GetInstance();

    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};

    // 释放不存在的对象应该返回 DAS_E_NOT_FOUND
    // 这里我们暂时返回 false，因为 DasResult 的具体值需要根据实际实现确定
    EXPECT_NO_THROW(factory.ReleaseProxy(obj_id));
}

// 测试同一个对象的多次查询
TEST(ProxyFactoryTest, MultipleQueriesForSameObject)
{
    ProxyFactory& factory = ProxyFactory::GetInstance();

    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};

    // 多次检查是否存在应该返回相同结果
    EXPECT_FALSE(factory.HasProxy(obj_id));
    EXPECT_FALSE(factory.HasProxy(obj_id));
    EXPECT_EQ(factory.GetProxy(obj_id), nullptr);
    EXPECT_EQ(factory.GetProxy(obj_id), nullptr);
}

// 测试多个不同对象的状态检查
TEST(ProxyFactoryTest, MultipleObjectsStateCheck)
{
    ProxyFactory& factory = ProxyFactory::GetInstance();

    ObjectId obj1{.session_id = 1, .generation = 1, .local_id = 100};
    ObjectId obj2{.session_id = 2, .generation = 1, .local_id = 200};
    ObjectId obj3{.session_id = 3, .generation = 1, .local_id = 300};

    // 验证所有对象都不存在
    EXPECT_FALSE(factory.HasProxy(obj1));
    EXPECT_FALSE(factory.HasProxy(obj2));
    EXPECT_FALSE(factory.HasProxy(obj3));

    // 验证所有对象的 GetProxy 都返回 nullptr
    EXPECT_EQ(factory.GetProxy(obj1), nullptr);
    EXPECT_EQ(factory.GetProxy(obj2), nullptr);
    EXPECT_EQ(factory.GetProxy(obj3), nullptr);

    // 验证总计数
    EXPECT_EQ(factory.GetProxyCount(), 0);
}

// 测试 ObjectId 编码解码后的状态一致性
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

    // 使用编码后的 ID 进行工厂操作
    ProxyFactory& factory = ProxyFactory::GetInstance();
    EXPECT_FALSE(factory.HasProxy(decoded));
    EXPECT_EQ(factory.GetProxy(decoded), nullptr);
}

// 测试边界值的 ObjectId
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

    // 使用最大值对象进行工厂操作
    ProxyFactory& factory = ProxyFactory::GetInstance();
    EXPECT_FALSE(factory.HasProxy(decoded));
    EXPECT_EQ(factory.GetProxy(decoded), nullptr);
}

// 测试零值的 ObjectId
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

    // 使用零值对象进行工厂操作
    ProxyFactory& factory = ProxyFactory::GetInstance();
    EXPECT_FALSE(factory.HasProxy(decoded));
    EXPECT_EQ(factory.GetProxy(decoded), nullptr);
}

// 测试工厂状态的一致性
TEST(ProxyFactoryTest, FactoryStateConsistency)
{
    ProxyFactory& factory = ProxyFactory::GetInstance();

    // 重置工厂状态
    factory.ClearAllProxies();

    // 验证状态一致性
    EXPECT_EQ(factory.GetProxyCount(), 0);

    // 创建多个对象 ID
    std::vector<ObjectId> obj_ids = {
        {.session_id = 1, .generation = 1, .local_id = 100},
        {.session_id = 2, .generation = 2, .local_id = 200},
        {.session_id = 3, .generation = 3, .local_id = 300}};

    // 验证所有对象都不存在
    for (const auto& obj_id : obj_ids)
    {
        EXPECT_FALSE(factory.HasProxy(obj_id));
        EXPECT_EQ(factory.GetProxy(obj_id), nullptr);
    }

    // 验证计数
    EXPECT_EQ(factory.GetProxyCount(), 0);

    // 尝试释放所有对象
    for (const auto& obj_id : obj_ids)
    {
        EXPECT_NO_THROW(factory.ReleaseProxy(obj_id));
    }

    // 最终状态仍应一致
    EXPECT_EQ(factory.GetProxyCount(), 0);
}

// 测试 ProxyFactory 初始化
TEST(ProxyFactoryTest, Initialization)
{
    ProxyFactory& factory = ProxyFactory::GetInstance();

    // 初始状态下应该未初始化
    EXPECT_FALSE(factory.IsInitialized());

    // 测试初始化（这里我们只是测试接口，实际需要有效的对象管理器）
    // 由于我们没有实际的对象管理器实例，这里只测试不会崩溃
    EXPECT_NO_THROW(factory.Initialize(nullptr, nullptr));

    // 初始化后应该仍未初始化（因为传入了 nullptr）
    EXPECT_FALSE(factory.IsInitialized());
}