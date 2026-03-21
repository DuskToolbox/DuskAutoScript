#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/DasPtr.hpp>
#include <gtest/gtest.h>

#include "MockDasObject.h"

using DAS::Core::IPC::DecodeObjectId;
using DAS::Core::IPC::DistributedObjectManager;
using DAS::Core::IPC::EncodeObjectId;
using DAS::Core::IPC::ObjectId;

// Test fixture for ObjectManager tests
class IpcObjectManagerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        manager_ = std::make_unique<DistributedObjectManager>();
    }

    void TearDown() override { manager_.reset(); }

    std::unique_ptr<DistributedObjectManager> manager_;
};

// ====== Generation Tests ======

TEST_F(IpcObjectManagerTest, RegisterLocalObject_GenerationStartsAtOne)
{
    auto     mock = new MockDasObject();
    ObjectId object_id{1, 0, 0};

    auto result = manager_->RegisterLocalObject(mock, object_id);
    EXPECT_EQ(result, DAS_S_OK);

    EXPECT_EQ(object_id.generation, 1);
    // RegisterLocalObject 内部 AddRef，mock 应该有 2 个引用
    // （new 创建时 ref=0，RegisterLocalObject AddRef → ref=1）
    // 但析构由 DasPtr 管理，不需要手动 delete
}

TEST_F(
    IpcObjectManagerTest,
    RegisterLocalObject_MultipleObjectsHaveDifferentLocalIds)
{
    auto     obj1 = new MockDasObject();
    auto     obj2 = new MockDasObject();
    auto     obj3 = new MockDasObject();
    ObjectId id1{}, id2{}, id3{};

    ASSERT_EQ(manager_->RegisterLocalObject(obj1, id1), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterLocalObject(obj2, id2), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterLocalObject(obj3, id3), DAS_S_OK);

    EXPECT_NE(id1.local_id, id2.local_id);
    EXPECT_NE(id2.local_id, id3.local_id);
    EXPECT_NE(id1.local_id, id3.local_id);
}

// ====== ValidateHandle Tests ======

TEST_F(IpcObjectManagerTest, IsValidObject_ValidHandle)
{
    auto     mock = new MockDasObject();
    ObjectId object_id{.session_id = 0, .generation = 0, .local_id = 0};

    ASSERT_EQ(manager_->RegisterLocalObject(mock, object_id), DAS_S_OK);
    EXPECT_TRUE(manager_->IsValidObject(object_id));
}

TEST_F(IpcObjectManagerTest, IsValidObject_NullHandle)
{
    ObjectId null_id{.session_id = 0, .generation = 0, .local_id = 0};
    EXPECT_FALSE(manager_->IsValidObject(null_id));
}

TEST_F(IpcObjectManagerTest, IsValidObject_UnregisteredHandle)
{
    ObjectId fake_id{.session_id = 1, .generation = 1, .local_id = 99999};

    EXPECT_FALSE(manager_->IsValidObject(fake_id));
}

// ====== IsLocal Tests ======

TEST_F(IpcObjectManagerTest, IsLocalObject_LocalObject)
{
    auto     mock = new MockDasObject();
    ObjectId object_id{0, 0, 0};

    ASSERT_EQ(manager_->RegisterLocalObject(mock, object_id), DAS_S_OK);
    EXPECT_TRUE(manager_->IsLocalObject(object_id));
}

TEST_F(IpcObjectManagerTest, IsLocalObject_RemoteObject)
{
    // Create a remote object ID with different session_id
    ObjectId remote_id{
        .session_id = 2, // different process
        .generation = 1,
        .local_id = 100};

    ASSERT_EQ(manager_->RegisterRemoteObject(remote_id), DAS_S_OK);
    EXPECT_FALSE(manager_->IsLocalObject(remote_id));
}

TEST_F(IpcObjectManagerTest, IsLocalObject_NonExistentObject)
{
    ObjectId fake_id{.session_id = 1, .generation = 1, .local_id = 99999};

    EXPECT_FALSE(manager_->IsLocalObject(fake_id));
}

// ====== Stale Handle Tests ======

TEST_F(IpcObjectManagerTest, StaleHandle_AfterUnregister)
{
    auto     mock = new MockDasObject();
    ObjectId object_id{0, 0, 0};

    ASSERT_EQ(manager_->RegisterLocalObject(mock, object_id), DAS_S_OK);
    EXPECT_TRUE(manager_->IsValidObject(object_id));

    // Unregister the object
    ASSERT_EQ(manager_->UnregisterObject(object_id), DAS_S_OK);

    // Now the handle should be stale (invalid)
    EXPECT_FALSE(manager_->IsValidObject(object_id));
}

// ====== Lookup Tests ======

TEST_F(IpcObjectManagerTest, LookupObject_LocalObject)
{
    auto     mock = new MockDasObject();
    ObjectId object_id{0, 0, 0};

    ASSERT_EQ(manager_->RegisterLocalObject(mock, object_id), DAS_S_OK);

    IDasBase* ptr = nullptr;
    auto      result = manager_->LookupObject(object_id, &ptr);
    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_EQ(ptr, mock);
    // LookupObject 内部 AddRef，需要 Release
    ptr->Release();
}

TEST_F(IpcObjectManagerTest, LookupObject_RemoteObjectFails)
{
    ObjectId remote_id{.session_id = 2, .generation = 1, .local_id = 100};

    ASSERT_EQ(manager_->RegisterRemoteObject(remote_id), DAS_S_OK);

    IDasBase* ptr = nullptr;
    auto      result = manager_->LookupObject(remote_id, &ptr);
    EXPECT_NE(result, DAS_S_OK); // Should fail for remote objects
}

TEST_F(IpcObjectManagerTest, LookupObject_NullPointer)
{
    auto     mock = new MockDasObject();
    ObjectId object_id{0, 0, 0};

    ASSERT_EQ(manager_->RegisterLocalObject(mock, object_id), DAS_S_OK);

    auto result = manager_->LookupObject(object_id, nullptr);
    EXPECT_NE(result, DAS_S_OK); // Should fail with null output pointer
}

// ====== Error Cases ======

TEST_F(IpcObjectManagerTest, RegisterLocalObject_NullPointer)
{
    ObjectId object_id{0, 0, 0};
    auto     result = manager_->RegisterLocalObject(nullptr, object_id);
    EXPECT_NE(result, DAS_S_OK);
}

TEST_F(IpcObjectManagerTest, RegisterRemoteObject_NullObjectId)
{
    auto result = manager_->RegisterRemoteObject(ObjectId{0, 0, 0});
    EXPECT_NE(result, DAS_S_OK);
}

TEST_F(IpcObjectManagerTest, UnregisterObject_InvalidId)
{
    ObjectId invalid_id{0, 0, 0};
    auto     result = manager_->UnregisterObject(invalid_id);
    EXPECT_NE(result, DAS_S_OK);
}

// ====== Shutdown Cleanup Tests ======

TEST_F(IpcObjectManagerTest, Shutdown_ClearsAllObjects)
{
    auto     obj1 = new MockDasObject();
    auto     obj2 = new MockDasObject();
    uint32_t id1_val = 0, id2_val = 0;
    ObjectId oid1{1, 1, id1_val}, oid2{1, 1, id2_val};

    ASSERT_EQ(manager_->RegisterLocalObject(obj1, oid1), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterLocalObject(obj2, oid2), DAS_S_OK);

    // Re-create manager to clear all objects (DasPtr 自动 Release)
    manager_.reset();
    manager_ = std::make_unique<DistributedObjectManager>();

    // Old handles should be invalid
    EXPECT_FALSE(manager_->IsValidObject(oid1));
    EXPECT_FALSE(manager_->IsValidObject(oid2));
}
