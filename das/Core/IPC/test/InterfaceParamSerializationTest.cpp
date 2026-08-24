#include <das/Core/IPC/DasReadOnlyStringProxy.h>
#include <das/Core/IPC/DasVariantVectorByValueProxy.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/InterfaceParamSerialization.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <gtest/gtest.h>

#include "MockDasObject.h"

using Das::Core::IPC::DasReadOnlyStringProxy;
using Das::Core::IPC::DasVariantVectorByValueProxy;
using Das::Core::IPC::DeserializeInInterfaceParam;
using Das::Core::IPC::DistributedObjectManager;
using Das::Core::IPC::EncodeObjectId;
using Das::Core::IPC::IpcRunLoop;
using Das::Core::IPC::IsNullObjectId;
using Das::Core::IPC::IsTransportLevelError;
using Das::Core::IPC::ObjectId;
using Das::Core::IPC::PendingInParamExportGuard;
using Das::Core::IPC::ProxyFactory;
using Das::Core::IPC::RemoteObjectRegistry;
using Das::Core::IPC::SerializeInInterfaceParam;

// Test fixture for InterfaceParamSerialization tests
class InterfaceParamSerializationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        manager_ = std::make_unique<DistributedObjectManager>();
    }

    void TearDown() override { manager_.reset(); }

    std::unique_ptr<DistributedObjectManager> manager_;
};

// ====== SerializeInInterfaceParam Tests ======

TEST_F(InterfaceParamSerializationTest, NullptrSerialization)
{
    ObjectId out_id{1, 1, 1};
    bool     newly = true;

    auto result = SerializeInInterfaceParam(nullptr, *manager_, out_id, &newly);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(IsNullObjectId(out_id));
    EXPECT_FALSE(newly);
}

TEST_F(InterfaceParamSerializationTest, RegisteredLocalObject)
{
    auto     mock = new MockDasObject();
    ObjectId registered_id{};
    ASSERT_EQ(manager_->RegisterLocalObject(mock, registered_id), DAS_S_OK);

    ObjectId out_id{};
    bool     newly = true;

    auto result = SerializeInInterfaceParam(mock, *manager_, out_id, &newly);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_EQ(out_id, registered_id);
    EXPECT_FALSE(newly);
}

TEST_F(InterfaceParamSerializationTest, NewLocalObjectAutoRegister)
{
    auto mock = new MockDasObject();

    ObjectId out_id{};
    bool     newly = false;

    auto result = SerializeInInterfaceParam(mock, *manager_, out_id, &newly);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_FALSE(IsNullObjectId(out_id));
    EXPECT_TRUE(newly);
    EXPECT_TRUE(manager_->IsValidObject(out_id));

    // Verify the object is findable via reverse lookup
    ObjectId lookup_id{};
    EXPECT_EQ(manager_->LookupObjectIdFromPtr(mock, lookup_id), DAS_S_OK);
    EXPECT_EQ(lookup_id, out_id);
}

// 同一已注册对象重复序列化：零记账（不新增 pin、不新增条目/holder）。
// 观测锚点 = 条目回收：重复序列化不得产生超出"服务 pin 一次释放 + 对端
// 唯一 proxy 一次消亡"之外的存活名额，否则条目悬挂无法归零。
TEST_F(
    InterfaceParamSerializationTest,
    RepeatedSerializationOfRegisteredObjectAddsNoAccounting)
{
    auto     mock = new MockDasObject();
    ObjectId registered_id{};
    ASSERT_EQ(manager_->RegisterLocalObject(mock, registered_id), DAS_S_OK);

    const size_t entry_count_before = manager_->GetLocalEntryCount();

    ObjectId serialized_id{};
    for (int serialization_round = 0; serialization_round < 5;
         ++serialization_round)
    {
        ASSERT_EQ(
            SerializeInInterfaceParam(mock, *manager_, serialized_id),
            DAS_S_OK);
        ASSERT_EQ(serialized_id, registered_id);
    }

    // 重复序列化零记账：条目数与远端 holder 数均不增长
    EXPECT_EQ(manager_->GetLocalEntryCount(), entry_count_before);
    EXPECT_EQ(manager_->GetRemoteHolderCount(registered_id), 0u);

    // 对端物化唯一 proxy（物化申报的记账面）
    constexpr uint16_t peer_session_id = 9;
    ASSERT_EQ(
        manager_->AddRemoteHolder(registered_id, peer_session_id),
        DAS_S_OK);

    // 服务下线：显式 pin 唯一一次释放；远端仍持有 → 条目保留
    DAS::DasPtr<IDasBase> pin_released_object;
    ASSERT_EQ(
        manager_->UnregisterLocalPin(registered_id, pin_released_object),
        DAS_S_OK);
    EXPECT_EQ(manager_->GetLocalEntryCount(), entry_count_before);

    // 对端唯一 proxy 消亡 → 条目按移除条件回收
    DAS::DasPtr<IDasBase> holder_released_object;
    ASSERT_EQ(
        manager_->ReleaseRemoteHolder(
            registered_id,
            peer_session_id,
            holder_released_object),
        DAS_S_OK);
    EXPECT_EQ(manager_->GetLocalEntryCount(), 0u);
}

// 未注册对象重复序列化 N 次后，对端唯一 proxy 消亡即回收条目——
// auto-register 条目不得携带会悬挂的本地名额。
TEST_F(
    InterfaceParamSerializationTest,
    RepeatedAutoRegistrationThenSingleProxyDeathRecyclesEntry)
{
    auto mock = new MockDasObject();

    ObjectId auto_registered_id{};
    bool     newly_registered = false;
    ASSERT_EQ(
        SerializeInInterfaceParam(
            mock,
            *manager_,
            auto_registered_id,
            &newly_registered),
        DAS_S_OK);
    ASSERT_TRUE(newly_registered);

    for (int serialization_round = 0; serialization_round < 4;
         ++serialization_round)
    {
        ObjectId repeated_id{};
        bool     repeated_newly = true;
        ASSERT_EQ(
            SerializeInInterfaceParam(
                mock,
                *manager_,
                repeated_id,
                &repeated_newly),
            DAS_S_OK);
        ASSERT_EQ(repeated_id, auto_registered_id);
        ASSERT_FALSE(repeated_newly);
    }

    // 重复序列化零记账：单一 auto 条目、零远端 holder
    EXPECT_EQ(manager_->GetLocalEntryCount(), 1u);
    EXPECT_EQ(manager_->GetRemoteHolderCount(auto_registered_id), 0u);

    constexpr uint16_t peer_session_id = 9;
    ASSERT_EQ(
        manager_->AddRemoteHolder(auto_registered_id, peer_session_id),
        DAS_S_OK);
    DAS::DasPtr<IDasBase> released_object;
    ASSERT_EQ(
        manager_->ReleaseRemoteHolder(
            auto_registered_id,
            peer_session_id,
            released_object),
        DAS_S_OK);

    // 单 proxy 消亡即回收：多余名额悬挂时条目必不归零
    EXPECT_EQ(manager_->GetLocalEntryCount(), 0u);
}

// ====== IsTransportLevelError Tests ======

TEST_F(InterfaceParamSerializationTest, IsTransportLevelError_AllCodes)
{
    // All transport-level error codes must return true
    EXPECT_TRUE(IsTransportLevelError(DAS_E_IPC_NOT_INITIALIZED));
    EXPECT_TRUE(IsTransportLevelError(DAS_E_IPC_DISCONNECTED));
    EXPECT_TRUE(IsTransportLevelError(DAS_E_IPC_NO_CONNECTIONS));
    EXPECT_TRUE(IsTransportLevelError(DAS_E_IPC_SEND_FAILED));
    EXPECT_TRUE(IsTransportLevelError(DAS_E_IPC_CANCELED));

    // REMOTE_ERROR is NOT a transport-level error (request may have reached
    // remote)
    EXPECT_FALSE(IsTransportLevelError(DAS_E_IPC_REMOTE_ERROR));

    // Non-transport codes must return false
    EXPECT_FALSE(IsTransportLevelError(DAS_S_OK));
    EXPECT_FALSE(IsTransportLevelError(DAS_E_INVALID_ARGUMENT));
}

// ====== PendingInParamExportGuard Tests ======

TEST_F(
    InterfaceParamSerializationTest,
    PendingInParamExportGuard_CommitPreventsRollback)
{
    auto     mock = new MockDasObject();
    ObjectId object_id{};
    ASSERT_EQ(manager_->RegisterLocalObject(mock, object_id), DAS_S_OK);
    ASSERT_TRUE(manager_->IsValidObject(object_id));

    {
        PendingInParamExportGuard guard(*manager_);
        guard.Track(object_id, true);
        guard.Commit();
    }

    // Object should still be registered after guard destruction
    EXPECT_TRUE(manager_->IsValidObject(object_id));
}

TEST_F(
    InterfaceParamSerializationTest,
    PendingInParamExportGuard_NoCommitTriggersRollback)
{
    auto     mock = new MockDasObject();
    ObjectId object_id{};
    ASSERT_EQ(manager_->RegisterLocalObject(mock, object_id), DAS_S_OK);
    ASSERT_TRUE(manager_->IsValidObject(object_id));

    {
        PendingInParamExportGuard guard(*manager_);
        guard.Track(object_id, true);
        // No Commit() — guard destructor should unregister
    }

    // Object should be unregistered after guard destruction
    EXPECT_FALSE(manager_->IsValidObject(object_id));
}

TEST_F(
    InterfaceParamSerializationTest,
    PendingInParamExportGuard_NotNewlyRegisteredIgnored)
{
    auto     mock = new MockDasObject();
    ObjectId object_id{};
    ASSERT_EQ(manager_->RegisterLocalObject(mock, object_id), DAS_S_OK);
    ASSERT_TRUE(manager_->IsValidObject(object_id));

    {
        PendingInParamExportGuard guard(*manager_);
        guard.Track(object_id, false);
        // No Commit() — but newly_registered=false, so nothing should
        // be unregistered
    }

    // Object should still be registered
    EXPECT_TRUE(manager_->IsValidObject(object_id));
}

TEST_F(
    InterfaceParamSerializationTest,
    PendingInParamExportGuard_MultipleTracks)
{
    auto     mock1 = new MockDasObject();
    auto     mock2 = new MockDasObject();
    auto     mock3 = new MockDasObject();
    ObjectId id1{}, id2{}, id3{};
    ASSERT_EQ(manager_->RegisterLocalObject(mock1, id1), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterLocalObject(mock2, id2), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterLocalObject(mock3, id3), DAS_S_OK);

    {
        PendingInParamExportGuard guard(*manager_);
        guard.Track(id1, true);
        guard.Track(id2, false);
        guard.Track(id3, true);
        guard.Commit();
    }

    // All objects should still be registered after commit
    EXPECT_TRUE(manager_->IsValidObject(id1));
    EXPECT_TRUE(manager_->IsValidObject(id2));
    EXPECT_TRUE(manager_->IsValidObject(id3));
}

// 回滚条件化：对端已物化（远端 holder 非空）的条目不被 transport 回滚
// 连根拔——条目保留，由对端 RELEASE / 断连清扫按配对收口。
TEST_F(
    InterfaceParamSerializationTest,
    PendingInParamExportGuard_RollbackKeepsMaterializedEntry)
{
    auto     mock = new MockDasObject();
    ObjectId object_id{};
    ASSERT_EQ(manager_->RegisterLocalObject(mock, object_id, false), DAS_S_OK);

    constexpr uint16_t peer_session_id = 9;
    ASSERT_EQ(manager_->AddRemoteHolder(object_id, peer_session_id), DAS_S_OK);

    {
        PendingInParamExportGuard guard(*manager_);
        guard.Track(object_id, true);
        // No Commit() — transport-level error rollback path
    }

    // holders 非空：条目保留，不连根拔已物化条目
    EXPECT_TRUE(manager_->IsValidObject(object_id));
    EXPECT_EQ(manager_->GetRemoteHolderCount(object_id), 1u);
    EXPECT_EQ(manager_->GetLocalEntryCount(), 1u);

    // 对端 RELEASE 后条目按移除条件回收
    DAS::DasPtr<IDasBase> released_object;
    ASSERT_EQ(
        manager_
            ->ReleaseRemoteHolder(object_id, peer_session_id, released_object),
        DAS_S_OK);
    EXPECT_EQ(manager_->GetLocalEntryCount(), 0u);
}

TEST(
    InterfaceParamSerializationMaterializationTest,
    RemoteManualProxyUsesProxyFactoryCache)
{
    ProxyFactory         proxy_factory;
    RemoteObjectRegistry registry;
    IpcRunLoop           run_loop(false, nullptr, proxy_factory, registry);
    const ObjectId remote_id{.session_id = 88, .generation = 1, .local_id = 5};

    IDasBase* out = nullptr;
    ASSERT_EQ(
        DeserializeInInterfaceParam(
            EncodeObjectId(remote_id),
            DasReadOnlyStringProxy::InterfaceId,
            proxy_factory.GetObjectManager(),
            run_loop,
            {},
            proxy_factory,
            &out),
        DAS_S_OK);
    ASSERT_NE(out, nullptr);
    EXPECT_TRUE(proxy_factory.HasProxy(remote_id));

    EXPECT_EQ(out->Release(), 0u);
    EXPECT_FALSE(proxy_factory.HasProxy(remote_id));
}

TEST(
    InterfaceParamSerializationMaterializationTest,
    VariantVectorByValueUsesProxyFactoryCache)
{
    ProxyFactory         proxy_factory;
    RemoteObjectRegistry registry;
    IpcRunLoop           run_loop(false, nullptr, proxy_factory, registry);
    const ObjectId remote_id{.session_id = 89, .generation = 1, .local_id = 6};

    IDasBase* out = nullptr;
    ASSERT_EQ(
        DeserializeInInterfaceParam(
            EncodeObjectId(remote_id),
            DasVariantVectorByValueProxy::InterfaceId,
            proxy_factory.GetObjectManager(),
            run_loop,
            {},
            proxy_factory,
            &out),
        DAS_S_OK);
    ASSERT_NE(out, nullptr);
    EXPECT_TRUE(proxy_factory.HasProxy(remote_id));

    IDasBase* second = nullptr;
    ASSERT_EQ(
        DeserializeInInterfaceParam(
            EncodeObjectId(remote_id),
            DasVariantVectorByValueProxy::InterfaceId,
            proxy_factory.GetObjectManager(),
            run_loop,
            {},
            proxy_factory,
            &second),
        DAS_S_OK);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(out, second);

    EXPECT_EQ(second->Release(), 1u);
    EXPECT_EQ(out->Release(), 0u);
    EXPECT_FALSE(proxy_factory.HasProxy(remote_id));
}
