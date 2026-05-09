/**
 * @file ProxyFactoryTest.cpp
 * @brief ProxyFactory 单元测试
 *
 * ProxyFactory 已从单例改为构造函数注入模式。
 * 由于 ProxyFactory 依赖 IpcRunLoop（构造复杂），
 * 这里仅测试不依赖 IpcRunLoop 的基础行为。
 * 完整行为通过集成测试覆盖。
 */

#include <das/Core/IPC/DasReadOnlyStringProxy.h>
#include <das/Core/IPC/DasVariantVectorByValueProxy.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/DasTypes.hpp>
#include <das/IDasBase.h>
#include <gtest/gtest.h>
#include <memory>

using DAS::DasPtr;
using DAS::Core::IPC::BusinessThread;
using DAS::Core::IPC::DasReadOnlyStringProxy;
using DAS::Core::IPC::DasVariantVectorByValueProxy;
using DAS::Core::IPC::DecodeObjectId;
using DAS::Core::IPC::DistributedObjectManager;
using DAS::Core::IPC::EncodeObjectId;
using DAS::Core::IPC::IPCProxyBase;
using DAS::Core::IPC::IpcRunLoop;
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

class ProxyRefcountTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        run_loop_ = std::make_unique<IpcRunLoop>(
            false,
            nullptr,
            proxy_factory_,
            registry_);
    }

    ProxyFactory                  proxy_factory_;
    RemoteObjectRegistry          registry_;
    std::unique_ptr<IpcRunLoop>   run_loop_;
    std::weak_ptr<BusinessThread> business_thread_;
    const ObjectId object_id_{.session_id = 7, .generation = 1, .local_id = 42};
};

TEST_F(ProxyRefcountTest, ReadOnlyStringProxyUsesIPCProxyBaseRefcount)
{
    auto* proxy = new DasReadOnlyStringProxy(
        DasReadOnlyStringProxy::InterfaceId,
        object_id_,
        *run_loop_,
        business_thread_,
        proxy_factory_);

    EXPECT_EQ(proxy->AddRef(), 2u);

    IPCProxyBase* runtime_tag = nullptr;
    ASSERT_EQ(
        proxy->QueryInterface(
            DasIidOf<IPCProxyBase>(),
            reinterpret_cast<void**>(&runtime_tag)),
        DAS_S_OK);
    ASSERT_NE(runtime_tag, nullptr);
    EXPECT_EQ(runtime_tag, static_cast<IPCProxyBase*>(proxy));

    EXPECT_EQ(runtime_tag->ReleaseRuntimeTagRef(), 2u);
    EXPECT_TRUE(runtime_tag->TryAddRefForCache());

    EXPECT_EQ(proxy->Release(), 2u);
    EXPECT_EQ(proxy->Release(), 1u);
    EXPECT_EQ(proxy->Release(), 0u);
}

TEST_F(ProxyRefcountTest, VariantVectorProxyUsesIPCProxyBaseRefcount)
{
    auto* proxy = new DasVariantVectorByValueProxy(
        DasVariantVectorByValueProxy::InterfaceId,
        object_id_,
        *run_loop_,
        business_thread_,
        proxy_factory_);

    EXPECT_EQ(proxy->AddRef(), 2u);

    IPCProxyBase* runtime_tag = nullptr;
    ASSERT_EQ(
        proxy->QueryInterface(
            DasIidOf<IPCProxyBase>(),
            reinterpret_cast<void**>(&runtime_tag)),
        DAS_S_OK);
    ASSERT_NE(runtime_tag, nullptr);
    EXPECT_EQ(runtime_tag, static_cast<IPCProxyBase*>(proxy));

    EXPECT_EQ(runtime_tag->ReleaseRuntimeTagRef(), 2u);
    EXPECT_TRUE(runtime_tag->TryAddRefForCache());

    EXPECT_EQ(proxy->Release(), 2u);
    EXPECT_EQ(proxy->Release(), 1u);
    EXPECT_EQ(proxy->Release(), 0u);
}

TEST_F(ProxyRefcountTest, GetOrCreateProxyCacheHitUsesLiveRuntimeRef)
{
    DasPtr<IDasBase> first = proxy_factory_.GetOrCreateProxy(
        *run_loop_,
        business_thread_,
        object_id_,
        DasReadOnlyStringProxy::InterfaceId);
    ASSERT_TRUE(first);
    EXPECT_TRUE(proxy_factory_.HasProxy(object_id_));

    DasPtr<IDasBase> second = proxy_factory_.GetOrCreateProxy(
        *run_loop_,
        business_thread_,
        object_id_,
        DasReadOnlyStringProxy::InterfaceId);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.Get(), second.Get());
    EXPECT_EQ(proxy_factory_.GetProxyCount(), 1u);

    second.Reset();
    EXPECT_TRUE(proxy_factory_.HasProxy(object_id_));

    first.Reset();
    EXPECT_FALSE(proxy_factory_.HasProxy(object_id_));
    EXPECT_EQ(proxy_factory_.GetProxyCount(), 0u);
}

TEST_F(ProxyRefcountTest, GetOrCreateProxyCachesInterfaceViewsPerObjectId)
{
    DasPtr<IDasBase> string_proxy = proxy_factory_.GetOrCreateProxy(
        *run_loop_,
        business_thread_,
        object_id_,
        DasReadOnlyStringProxy::InterfaceId);
    ASSERT_TRUE(string_proxy);

    DasPtr<IDasBase> vector_proxy = proxy_factory_.GetOrCreateProxy(
        *run_loop_,
        business_thread_,
        object_id_,
        DasVariantVectorByValueProxy::InterfaceId);
    ASSERT_TRUE(vector_proxy);

    EXPECT_NE(string_proxy.Get(), vector_proxy.Get());
    EXPECT_TRUE(proxy_factory_.HasProxy(object_id_));
    EXPECT_EQ(proxy_factory_.GetProxyCount(), 2u);
}

TEST_F(ProxyRefcountTest, FinalReleaseInvalidationRequiresRuntimePointerMatch)
{
    DasPtr<IDasBase> proxy = proxy_factory_.GetOrCreateProxy(
        *run_loop_,
        business_thread_,
        object_id_,
        DasReadOnlyStringProxy::InterfaceId);
    ASSERT_TRUE(proxy);
    EXPECT_TRUE(proxy_factory_.HasProxy(object_id_));

    proxy_factory_.OnProxyFinalRelease(
        object_id_,
        DasReadOnlyStringProxy::InterfaceId,
        nullptr);
    EXPECT_TRUE(proxy_factory_.HasProxy(object_id_));

    proxy.Reset();
    EXPECT_FALSE(proxy_factory_.HasProxy(object_id_));
}
