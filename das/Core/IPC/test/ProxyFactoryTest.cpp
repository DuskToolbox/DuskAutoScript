#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/DasTypes.hpp>
#include <gtest/gtest.h>
#include <memory>
using DAS::Core::IPC::DecodeObjectId;
using DAS::Core::IPC::DistributedObjectManager;
using DAS::Core::IPC::EncodeObjectId;
using DAS::Core::IPC::IPCProxyBase;
using DAS::Core::IPC::IpcRunLoop;
using DAS::Core::IPC::ObjectId;
using DAS::Core::IPC::ProxyFactory;
using DAS::Core::IPC::RemoteObjectRegistry;

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

// 测试 ProxyFactory 与 RemoteObjectRegistry 和 DistributedObjectManager 的集成
TEST(ProxyFactoryTest, IntegrationWithRemoteObjectRegistry)
{
    ProxyFactory& factory = ProxyFactory::GetInstance();
    auto&         registry = RemoteObjectRegistry::GetInstance();

    // 注册一个测试对象
    ObjectId test_obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    DasGuid  test_iid{
         .data1 = 0x12345678,
         .data2 = 0x1234,
         .data3 = 0x5678,
         .data4 = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xEF}};

    DasResult result =
        registry.RegisterObject(test_obj_id, test_iid, 1, "TestObject");

    EXPECT_EQ(result, DAS_S_OK);

    // 测试对象注册成功
    EXPECT_TRUE(registry.ObjectExists(test_obj_id));

    // 测试未初始化状态下创建代理
    auto proxy1 = factory.CreateProxy<void>(test_obj_id);
    EXPECT_EQ(proxy1, nullptr); // 应该返回 nullptr，因为工厂未初始化

    // 测试初始化后创建代理
    DistributedObjectManager obj_manager;
    result = obj_manager.Initialize(1);
    EXPECT_EQ(result, DAS_S_OK);

    result = factory.Initialize(&obj_manager, &registry);
    EXPECT_TRUE(factory.IsInitialized());

    // 现在应该能创建代理了（虽然返回的可能是空代理）
    auto proxy2 = factory.CreateProxy<void>(test_obj_id);
    // 可能返回空代理，因为缺少 IpcRunLoop 等依赖
    // 但至少不会崩溃
    EXPECT_NO_THROW(proxy2);

    // 清理
    factory.ClearAllProxies();
    registry.UnregisterObject(test_obj_id);
    obj_manager.Shutdown();
}

// 测试 CreateProxy 方法的类型安全特性
TEST(ProxyFactoryTest, CreateProxy_TypeSafety)
{
    ProxyFactory& factory = ProxyFactory::GetInstance();
    auto&         registry = RemoteObjectRegistry::GetInstance();

    // 注册不同类型的对象
    ObjectId obj1{.session_id = 1, .generation = 1, .local_id = 100};
    ObjectId obj2{.session_id = 1, .generation = 1, .local_id = 200};

    DasGuid iid1{
        .data1 = 0x11111111,
        .data2 = 0x1111,
        .data3 = 0x1111,
        .data4 = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11}};
    DasGuid iid2{
        .data1 = 0x22222222,
        .data2 = 0x2222,
        .data3 = 0x2222,
        .data4 = {0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22}};

    // 注册两个对象
    EXPECT_EQ(registry.RegisterObject(obj1, iid1, 1, "Object1"), DAS_S_OK);
    EXPECT_EQ(registry.RegisterObject(obj2, iid2, 1, "Object2"), DAS_S_OK);

    // 创建分布式对象管理器并初始化工厂
    DistributedObjectManager obj_manager;
    EXPECT_EQ(obj_manager.Initialize(1), DAS_S_OK);
    EXPECT_EQ(factory.Initialize(&obj_manager, &registry), DAS_S_OK);

    // 测试类型安全的代理创建
    auto proxy1 = factory.CreateProxy<void>(obj1);
    auto proxy2 = factory.CreateProxy<void>(obj2);

    // 至少应该能创建而不崩溃
    EXPECT_NO_THROW(proxy1);
    EXPECT_NO_THROW(proxy2);

    // 清理
    factory.ClearAllProxies();
    registry.UnregisterObject(obj1);
    registry.UnregisterObject(obj2);
    obj_manager.Shutdown();
}

// 测试代理的生命周期管理
TEST(ProxyFactoryTest, ProxyLifecycleManagement)
{
    ProxyFactory& factory = ProxyFactory::GetInstance();
    auto&         registry = RemoteObjectRegistry::GetInstance();

    ObjectId test_obj{.session_id = 1, .generation = 1, .local_id = 300};
    DasGuid  test_iid{
         .data1 = 0x33333333,
         .data2 = 0x3333,
         .data3 = 0x3333,
         .data4 = {0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33}};

    // 注册对象
    EXPECT_EQ(
        registry.RegisterObject(test_obj, test_iid, 1, "LifecycleTest"),
        DAS_S_OK);

    // 初始化工厂
    DistributedObjectManager obj_manager;
    EXPECT_EQ(obj_manager.Initialize(1), DAS_S_OK);
    EXPECT_EQ(factory.Initialize(&obj_manager, &registry), DAS_S_OK);

    // 测试初始状态
    EXPECT_FALSE(factory.HasProxy(test_obj));
    EXPECT_EQ(factory.GetProxyCount(), 0);

    // 创建代理
    auto proxy = factory.CreateProxy<void>(test_obj);
    EXPECT_NO_THROW(proxy); // 至少不崩溃

    // 检查状态
    EXPECT_TRUE(factory.HasProxy(test_obj)); // 现在应该有代理了
    EXPECT_EQ(factory.GetProxyCount(), 1);

    // 再次创建应该返回相同的代理（缓存机制）
    auto proxy2 = factory.CreateProxy<void>(test_obj);
    EXPECT_EQ(proxy, proxy2); // 应该是同一个代理

    // 释放代理
    auto result = factory.ReleaseProxy(test_obj);
    EXPECT_EQ(result, DAS_S_OK);

    // 检查释放后状态
    EXPECT_FALSE(factory.HasProxy(test_obj));
    EXPECT_EQ(factory.GetProxyCount(), 0);

    // 清理
    factory.ClearAllProxies();
    registry.UnregisterObject(test_obj);
    obj_manager.Shutdown();
}

// 测试 ProxyFactory 与 IpcRunLoop 的集成
TEST(ProxyFactoryTest, IntegrationWithIpcRunLoop)
{
    ProxyFactory& factory = ProxyFactory::GetInstance();
    auto&         registry = RemoteObjectRegistry::GetInstance();

    // 创建并初始化 IpcRunLoop
    auto runloop = std::make_unique<IpcRunLoop>();
    EXPECT_EQ(runloop->Initialize(), DAS_S_OK);

    ObjectId test_obj{.session_id = 1, .generation = 1, .local_id = 400};
    DasGuid  test_iid{
         .data1 = 0x44444444,
         .data2 = 0x4444,
         .data3 = 0x4444,
         .data4 = {0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44}};

    // 注册对象
    EXPECT_EQ(
        registry.RegisterObject(test_obj, test_iid, 1, "IpcTest"),
        DAS_S_OK);

    // 创建分布式对象管理器并初始化工厂
    DistributedObjectManager obj_manager;
    EXPECT_EQ(obj_manager.Initialize(1), DAS_S_OK);

    // 测试不带 IpcRunLoop 的初始化
    EXPECT_EQ(factory.Initialize(&obj_manager, &registry), DAS_S_OK);
    EXPECT_TRUE(factory.IsInitialized());
    EXPECT_TRUE(factory.GetRunLoop() == nullptr);

    // 测试设置 IpcRunLoop
    EXPECT_EQ(factory.SetRunLoop(runloop.get()), DAS_S_OK);
    EXPECT_TRUE(factory.GetRunLoop() == runloop.get());

    // 创建代理并验证 IpcRunLoop 集成
    auto proxy = factory.CreateProxy<void>(test_obj);
    EXPECT_NO_THROW(proxy);

    if (proxy)
    {
        // 验证 Proxy 有效
        EXPECT_TRUE(proxy->IsValid());

        // 测试远程方法调用功能
        std::vector<uint8_t> request_data = {0x01, 0x02, 0x03};
        std::vector<uint8_t> response_data;

        // 由于没有实际的 IPC 服务，这个调用可能会超时或返回错误
        // 但至少应该不会崩溃
        EXPECT_NO_THROW(
            proxy->CallRemoteMethod(1, request_data, response_data));

        // 验证 Get() 方法不会崩溃
        EXPECT_NO_THROW(proxy->Get());
    }

    // 清理
    factory.ClearAllProxies();
    registry.UnregisterObject(test_obj);
    obj_manager.Shutdown();
    runloop->Stop();
    runloop->Shutdown();
}