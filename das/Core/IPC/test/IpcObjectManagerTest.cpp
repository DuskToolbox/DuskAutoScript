#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/ObjectManager.h>
#include <gtest/gtest.h>

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
        auto result = manager_->Initialize(1); // local_session_id = 1
        ASSERT_EQ(result, DAS_S_OK);
    }

    void TearDown() override
    {
        if (manager_)
        {
            manager_->Shutdown();
        }
    }

    std::unique_ptr<DistributedObjectManager> manager_;
};

// ====== Generation Tests ======

TEST_F(IpcObjectManagerTest, RegisterLocalObject_GenerationStartsAtOne)
{
    int      dummy_object = 42;
    uint64_t object_id = 0;

    auto result = manager_->RegisterLocalObject(&dummy_object, object_id);
    EXPECT_EQ(result, DAS_S_OK);

    ObjectId decoded = DecodeObjectId(object_id);
    EXPECT_EQ(decoded.generation, 1);
}

TEST_F(
    IpcObjectManagerTest,
    RegisterLocalObject_MultipleObjectsHaveDifferentLocalIds)
{
    int      obj1 = 1, obj2 = 2, obj3 = 3;
    uint64_t id1 = 0, id2 = 0, id3 = 0;

    ASSERT_EQ(manager_->RegisterLocalObject(&obj1, id1), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterLocalObject(&obj2, id2), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterLocalObject(&obj3, id3), DAS_S_OK);

    ObjectId decoded1 = DecodeObjectId(id1);
    ObjectId decoded2 = DecodeObjectId(id2);
    ObjectId decoded3 = DecodeObjectId(id3);

    EXPECT_NE(decoded1.local_id, decoded2.local_id);
    EXPECT_NE(decoded2.local_id, decoded3.local_id);
    EXPECT_NE(decoded1.local_id, decoded3.local_id);
}

// ====== ValidateHandle Tests ======

TEST_F(IpcObjectManagerTest, IsValidObject_ValidHandle)
{
    int      dummy = 42;
    uint64_t object_id = 0;

    ASSERT_EQ(manager_->RegisterLocalObject(&dummy, object_id), DAS_S_OK);
    EXPECT_TRUE(manager_->IsValidObject(object_id));
}

TEST_F(IpcObjectManagerTest, IsValidObject_NullHandle)
{
    EXPECT_FALSE(manager_->IsValidObject(0));
}

TEST_F(IpcObjectManagerTest, IsValidObject_UnregisteredHandle)
{
    ObjectId fake_id{.session_id = 1, .generation = 1, .local_id = 99999};
    uint64_t encoded = EncodeObjectId(fake_id);

    EXPECT_FALSE(manager_->IsValidObject(encoded));
}

// ====== IsLocal Tests ======

TEST_F(IpcObjectManagerTest, IsLocalObject_LocalObject)
{
    int      dummy = 42;
    uint64_t object_id = 0;

    ASSERT_EQ(manager_->RegisterLocalObject(&dummy, object_id), DAS_S_OK);
    EXPECT_TRUE(manager_->IsLocalObject(object_id));
}

TEST_F(IpcObjectManagerTest, IsLocalObject_RemoteObject)
{
    // Create a remote object ID with different session_id
    ObjectId remote_id{
        .session_id = 2, // different process
        .generation = 1,
        .local_id = 100};
    uint64_t remote_encoded = EncodeObjectId(remote_id);

    ASSERT_EQ(manager_->RegisterRemoteObject(remote_encoded), DAS_S_OK);
    EXPECT_FALSE(manager_->IsLocalObject(remote_encoded));
}

TEST_F(IpcObjectManagerTest, IsLocalObject_NonExistentObject)
{
    ObjectId fake_id{.session_id = 1, .generation = 1, .local_id = 99999};
    uint64_t encoded = EncodeObjectId(fake_id);

    EXPECT_FALSE(manager_->IsLocalObject(encoded));
}

// ====== Stale Handle Tests ======

TEST_F(IpcObjectManagerTest, StaleHandle_AfterUnregister)
{
    int      dummy = 42;
    uint64_t object_id = 0;

    ASSERT_EQ(manager_->RegisterLocalObject(&dummy, object_id), DAS_S_OK);
    EXPECT_TRUE(manager_->IsValidObject(object_id));

    // Unregister the object
    ASSERT_EQ(manager_->UnregisterObject(object_id), DAS_S_OK);

    // Now the handle should be stale (invalid)
    EXPECT_FALSE(manager_->IsValidObject(object_id));
}

TEST_F(IpcObjectManagerTest, StaleHandle_AfterRelease)
{
    int      dummy = 42;
    uint64_t object_id = 0;

    ASSERT_EQ(manager_->RegisterLocalObject(&dummy, object_id), DAS_S_OK);

    // Release the object (refcount goes to 0)
    ASSERT_EQ(manager_->Release(object_id), DAS_S_OK);

    // Now the handle should be stale (invalid)
    EXPECT_FALSE(manager_->IsValidObject(object_id));
}

// ====== Reference Counting Tests ======

TEST_F(IpcObjectManagerTest, AddRef_IncrementsRefcount)
{
    int      dummy = 42;
    uint64_t object_id = 0;

    ASSERT_EQ(manager_->RegisterLocalObject(&dummy, object_id), DAS_S_OK);

    // AddRef twice
    ASSERT_EQ(manager_->AddRef(object_id), DAS_S_OK);
    ASSERT_EQ(manager_->AddRef(object_id), DAS_S_OK);

    // Release once - should still be valid
    ASSERT_EQ(manager_->Release(object_id), DAS_S_OK);
    EXPECT_TRUE(manager_->IsValidObject(object_id));

    // Release again - should still be valid
    ASSERT_EQ(manager_->Release(object_id), DAS_S_OK);
    EXPECT_TRUE(manager_->IsValidObject(object_id));

    // Final release - should be removed
    ASSERT_EQ(manager_->Release(object_id), DAS_S_OK);
    EXPECT_FALSE(manager_->IsValidObject(object_id));
}

// ====== Lookup Tests ======

TEST_F(IpcObjectManagerTest, LookupObject_LocalObject)
{
    int      dummy = 42;
    uint64_t object_id = 0;

    ASSERT_EQ(manager_->RegisterLocalObject(&dummy, object_id), DAS_S_OK);

    void* ptr = nullptr;
    auto  result = manager_->LookupObject(object_id, &ptr);
    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_EQ(ptr, &dummy);
}

TEST_F(IpcObjectManagerTest, LookupObject_RemoteObjectFails)
{
    ObjectId remote_id{.session_id = 2, .generation = 1, .local_id = 100};
    uint64_t remote_encoded = EncodeObjectId(remote_id);

    ASSERT_EQ(manager_->RegisterRemoteObject(remote_encoded), DAS_S_OK);

    void* ptr = nullptr;
    auto  result = manager_->LookupObject(remote_encoded, &ptr);
    EXPECT_NE(result, DAS_S_OK); // Should fail for remote objects
}

TEST_F(IpcObjectManagerTest, LookupObject_NullPointer)
{
    int      dummy = 42;
    uint64_t object_id = 0;

    ASSERT_EQ(manager_->RegisterLocalObject(&dummy, object_id), DAS_S_OK);

    auto result = manager_->LookupObject(object_id, nullptr);
    EXPECT_NE(result, DAS_S_OK); // Should fail with null output pointer
}

// ====== Error Cases ======

TEST_F(IpcObjectManagerTest, RegisterLocalObject_NullPointer)
{
    uint64_t object_id = 0;
    auto     result = manager_->RegisterLocalObject(nullptr, object_id);
    EXPECT_NE(result, DAS_S_OK);
}

TEST_F(IpcObjectManagerTest, RegisterRemoteObject_NullObjectId)
{
    auto result = manager_->RegisterRemoteObject(0);
    EXPECT_NE(result, DAS_S_OK);
}

TEST_F(IpcObjectManagerTest, UnregisterObject_InvalidId)
{
    auto result = manager_->UnregisterObject(0);
    EXPECT_NE(result, DAS_S_OK);
}

TEST_F(IpcObjectManagerTest, AddRef_InvalidId)
{
    auto result = manager_->AddRef(0);
    EXPECT_NE(result, DAS_S_OK);
}

TEST_F(IpcObjectManagerTest, Release_InvalidId)
{
    auto result = manager_->Release(0);
    EXPECT_NE(result, DAS_S_OK);
}

// ====== Shutdown Cleanup Tests ======

TEST_F(IpcObjectManagerTest, Shutdown_ClearsAllObjects)
{
    int      obj1 = 1, obj2 = 2;
    uint64_t id1 = 0, id2 = 0;

    ASSERT_EQ(manager_->RegisterLocalObject(&obj1, id1), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterLocalObject(&obj2, id2), DAS_S_OK);

    // Shutdown and reinitialize
    manager_->Shutdown();
    ASSERT_EQ(manager_->Initialize(1), DAS_S_OK);

    // Old handles should be invalid
    EXPECT_FALSE(manager_->IsValidObject(id1));
    EXPECT_FALSE(manager_->IsValidObject(id2));
}
