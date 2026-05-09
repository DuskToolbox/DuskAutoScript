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
