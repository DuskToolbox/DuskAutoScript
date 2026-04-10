#include <das/Core/IPC/ProxyFactory.h>
#include <das/IDasBase.h>
#include <gtest/gtest.h>
#include <memory>
using DAS::Core::IPC::ProxyFactory;

// 测试 ProxyFactory 单例模式
TEST(ProxyFactoryTest, GetInstance_ReturnsSameInstance)
{
    auto& instance1 = ProxyFactory::GetInstance();
    auto& instance2 = ProxyFactory::GetInstance();

    // 验证返回的是同一个实例
    EXPECT_EQ(&instance1, &instance2);
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
