#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/DasPtr.hpp>
#include <das/IDasBase.h>
#include <gtest/gtest.h>
#include <vector>

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

// ====== Dedup Tests ======

TEST_F(IpcObjectManagerTest, RegisterLocalObject_SamePointerReturnsSameObjectId)
{
    auto     mock = new MockDasObject();
    ObjectId id1{}, id2{};

    ASSERT_EQ(manager_->RegisterLocalObject(mock, id1), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterLocalObject(mock, id2), DAS_S_OK);

    // 同一指针应返回相同 ObjectId
    EXPECT_EQ(id1, id2);
    // 对象仍然有效
    EXPECT_TRUE(manager_->IsValidObject(id1));

    // 通过 Lookup 验证指针正确
    IDasBase* ptr = nullptr;
    ASSERT_EQ(manager_->LookupObject(id1, &ptr), DAS_S_OK);
    EXPECT_EQ(ptr, mock);
    ptr->Release();
}

TEST_F(IpcObjectManagerTest, RegisterLocalObject_RegisterTwiceUnregisterOnce)
{
    auto     mock = new MockDasObject();
    ObjectId id1{}, id2{};

    ASSERT_EQ(manager_->RegisterLocalObject(mock, id1), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterLocalObject(mock, id2), DAS_S_OK);
    EXPECT_EQ(id1, id2);

    // 注销一次
    ASSERT_EQ(manager_->UnregisterObject(id1), DAS_S_OK);

    // 对象仍可通过公共 API 访问
    IDasBase* ptr = nullptr;
    EXPECT_EQ(manager_->LookupObject(id1, &ptr), DAS_S_OK);
    ptr->Release(); // 释放 Lookup 的引用

    EXPECT_TRUE(manager_->IsValidObject(id1));
}

TEST_F(IpcObjectManagerTest, RegisterLocalObject_RegisterTwiceUnregisterTwice)
{
    auto     mock = new MockDasObject();
    ObjectId id1{}, id2{};

    ASSERT_EQ(manager_->RegisterLocalObject(mock, id1), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterLocalObject(mock, id2), DAS_S_OK);
    EXPECT_EQ(id1, id2);

    // 注销第一次
    ASSERT_EQ(manager_->UnregisterObject(id1), DAS_S_OK);

    // 注销第二次，对象完全释放
    ASSERT_EQ(manager_->UnregisterObject(id1), DAS_S_OK);

    // 对象已不存在
    EXPECT_FALSE(manager_->IsValidObject(id1));
    IDasBase* ptr = nullptr;
    EXPECT_NE(manager_->LookupObject(id1, &ptr), DAS_S_OK);
}

TEST_F(
    IpcObjectManagerTest,
    RegisterLocalObject_DifferentPointersReturnDifferentIds)
{
    auto     mock1 = new MockDasObject();
    auto     mock2 = new MockDasObject();
    auto     mock3 = new MockDasObject();
    ObjectId id1{}, id2{}, id3{};

    ASSERT_EQ(manager_->RegisterLocalObject(mock1, id1), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterLocalObject(mock2, id2), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterLocalObject(mock3, id3), DAS_S_OK);

    // 不同指针应返回不同 ObjectId
    EXPECT_NE(id1, id2);
    EXPECT_NE(id2, id3);
    EXPECT_NE(id1, id3);

    // 所有对象都有效
    EXPECT_TRUE(manager_->IsValidObject(id1));
    EXPECT_TRUE(manager_->IsValidObject(id2));
    EXPECT_TRUE(manager_->IsValidObject(id3));
}

TEST_F(IpcObjectManagerTest, RegisterRemoteObject_NotAffectedByDedup)
{
    ObjectId remote_id{.session_id = 2, .generation = 1, .local_id = 100};

    ASSERT_EQ(manager_->RegisterRemoteObject(remote_id), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterRemoteObject(remote_id), DAS_S_OK);

    // 远程对象有效且不是本地对象
    EXPECT_TRUE(manager_->IsValidObject(remote_id));
    EXPECT_FALSE(manager_->IsLocalObject(remote_id));

    // 远程对象无法通过 Lookup 获取指针
    IDasBase* ptr = nullptr;
    EXPECT_NE(manager_->LookupObject(remote_id, &ptr), DAS_S_OK);
}

TEST_F(IpcObjectManagerTest, RegisterRemoteObject_DuplicateUsesRefcount)
{
    ObjectId remote_id{.session_id = 2, .generation = 1, .local_id = 101};

    ASSERT_EQ(manager_->RegisterRemoteObject(remote_id), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterRemoteObject(remote_id), DAS_S_OK);

    ASSERT_EQ(manager_->UnregisterObject(remote_id), DAS_S_OK);
    EXPECT_TRUE(manager_->IsValidObject(remote_id));

    ASSERT_EQ(manager_->UnregisterObject(remote_id), DAS_S_OK);
    EXPECT_FALSE(manager_->IsValidObject(remote_id));
}

TEST_F(
    IpcObjectManagerTest,
    RegisterLocalObject_GenerationIncrementsOnlyOnFinalUnregister)
{
    auto     mock = new MockDasObject();
    ObjectId id{};

    ASSERT_EQ(manager_->RegisterLocalObject(mock, id), DAS_S_OK);
    ASSERT_EQ(manager_->RegisterLocalObject(mock, id), DAS_S_OK);

    ASSERT_EQ(manager_->UnregisterObject(id), DAS_S_OK);
    EXPECT_TRUE(manager_->IsValidObject(id));

    ObjectId lookup_id{};
    EXPECT_EQ(manager_->LookupObjectIdFromPtr(mock, lookup_id), DAS_S_OK);
    EXPECT_EQ(lookup_id, id);

    ASSERT_EQ(manager_->UnregisterObject(id), DAS_S_OK);
    EXPECT_FALSE(manager_->IsValidObject(id));

    ObjectId stale_id = id;
    EXPECT_FALSE(manager_->IsLocalObject(stale_id));
}

// ====== 双账本：local_pins_ + holder_sessions_ + remote_views_ ======

TEST_F(IpcObjectManagerTest, ExplicitPinPairsWithUnregisterLocalPin)
{
    auto     mock = new MockDasObject();
    ObjectId object_id{};
    ASSERT_EQ(manager_->RegisterLocalObject(mock, object_id, true), DAS_S_OK);
    EXPECT_EQ(manager_->GetLocalEntryCount(), 1u);

    DAS::DasPtr<IDasBase> released;
    ASSERT_EQ(manager_->UnregisterLocalPin(object_id, released), DAS_S_OK);

    // pins 归零且 holders 空 → 条目移除，对象本体唯一一次移交出参
    EXPECT_EQ(manager_->GetLocalEntryCount(), 0u);
    EXPECT_NE(released.Get(), nullptr);
    EXPECT_FALSE(manager_->IsValidObject(object_id));
}

TEST_F(IpcObjectManagerTest, PinlessRegistrationIsNoOpOnExistingEntry)
{
    auto     mock = new MockDasObject();
    ObjectId first_id{};
    ASSERT_EQ(manager_->RegisterLocalObject(mock, first_id, false), DAS_S_OK);
    EXPECT_EQ(manager_->GetLocalEntryCount(), 1u);

    ObjectId second_id{};
    ASSERT_EQ(manager_->RegisterLocalObject(mock, second_id, false), DAS_S_OK);
    EXPECT_EQ(second_id, first_id);

    // 重复 pinless 注册：不建新条目、不涨任何计数
    EXPECT_EQ(manager_->GetLocalEntryCount(), 1u);
    EXPECT_EQ(manager_->GetRemoteHolderCount(first_id), 0u);

    // pins==0 但存在远端 holder → 条目存活（配额由对端 ADD_REF 填充）
    ASSERT_EQ(manager_->AddRemoteHolder(first_id, 7), DAS_S_OK);
    EXPECT_EQ(manager_->GetLocalEntryCount(), 1u);
    EXPECT_TRUE(manager_->IsValidObject(first_id));
}

TEST_F(IpcObjectManagerTest, HolderAddReleasePairingAndFinalHandoff)
{
    auto     mock = new MockDasObject();
    ObjectId object_id{};
    ASSERT_EQ(manager_->RegisterLocalObject(mock, object_id, false), DAS_S_OK);

    ASSERT_EQ(manager_->AddRemoteHolder(object_id, 7), DAS_S_OK);
    ASSERT_EQ(manager_->AddRemoteHolder(object_id, 9), DAS_S_OK);
    EXPECT_EQ(manager_->GetRemoteHolderCount(object_id), 2u);

    DAS::DasPtr<IDasBase> partial_release;
    ASSERT_EQ(
        manager_->ReleaseRemoteHolder(object_id, 7, partial_release),
        DAS_S_OK);
    EXPECT_EQ(partial_release.Get(), nullptr); // 条目仍在，无移交
    EXPECT_EQ(manager_->GetRemoteHolderCount(object_id), 1u);
    EXPECT_EQ(manager_->GetLocalEntryCount(), 1u);

    DAS::DasPtr<IDasBase> final_release;
    ASSERT_EQ(
        manager_->ReleaseRemoteHolder(object_id, 9, final_release),
        DAS_S_OK);
    EXPECT_NE(final_release.Get(), nullptr); // 最后一个 holder：唯一一次移交
    EXPECT_EQ(manager_->GetLocalEntryCount(), 0u);

    // gen bump 生效：迟到 Lookup 收 STALE 而非 NOT_FOUND
    IDasBase* looked_up = nullptr;
    EXPECT_EQ(
        manager_->LookupObject(object_id, &looked_up),
        DAS_E_IPC_STALE_OBJECT_HANDLE);
}

TEST_F(IpcObjectManagerTest, RemoveSessionHoldersNeverTouchesLocalPins)
{
    auto     mock = new MockDasObject();
    ObjectId object_id{};
    ASSERT_EQ(manager_->RegisterLocalObject(mock, object_id, true), DAS_S_OK);
    ASSERT_EQ(manager_->AddRemoteHolder(object_id, 7), DAS_S_OK);
    ASSERT_EQ(manager_->AddRemoteHolder(object_id, 9), DAS_S_OK);

    // 只清 session 7 的 holder：pins 不动，条目存活
    const auto cleared_seven = manager_->RemoveSessionHolders(7);
    EXPECT_TRUE(cleared_seven.empty());
    EXPECT_EQ(manager_->GetRemoteHolderCount(object_id), 1u);
    EXPECT_EQ(manager_->GetLocalEntryCount(), 1u);
    EXPECT_TRUE(manager_->IsValidObject(object_id));

    // 清掉剩余 holder：pins>0 条目仍存活（服务 pin 不被断连清扫吃掉）
    const auto cleared_nine = manager_->RemoveSessionHolders(9);
    EXPECT_TRUE(cleared_nine.empty());
    EXPECT_EQ(manager_->GetLocalEntryCount(), 1u);
    EXPECT_TRUE(manager_->IsValidObject(object_id));

    // 服务 pin 仍可正常配对注销
    DAS::DasPtr<IDasBase> pin_release;
    ASSERT_EQ(manager_->UnregisterLocalPin(object_id, pin_release), DAS_S_OK);
    EXPECT_NE(pin_release.Get(), nullptr);
    EXPECT_EQ(manager_->GetLocalEntryCount(), 0u);
}

TEST_F(IpcObjectManagerTest, RemoveSessionHoldersRecyclesPinlessUnheldEntries)
{
    auto     pinless_mock = new MockDasObject();
    ObjectId pinless_id{};
    ASSERT_EQ(
        manager_->RegisterLocalObject(pinless_mock, pinless_id, false),
        DAS_S_OK);
    auto     pinned_mock = new MockDasObject();
    ObjectId pinned_id{};
    ASSERT_EQ(
        manager_->RegisterLocalObject(pinned_mock, pinned_id, true),
        DAS_S_OK);

    ASSERT_EQ(manager_->AddRemoteHolder(pinless_id, 7), DAS_S_OK);
    ASSERT_EQ(manager_->AddRemoteHolder(pinned_id, 7), DAS_S_OK);

    const auto released = manager_->RemoveSessionHolders(7);
    ASSERT_EQ(released.size(), 1u); // 只有 pinless 且 holders 清空的条目被移除
    EXPECT_EQ(released[0].Get(), pinless_mock);
    EXPECT_EQ(manager_->GetLocalEntryCount(), 1u);
    EXPECT_TRUE(manager_->IsValidObject(pinned_id));
}

TEST_F(IpcObjectManagerTest, RemoteViewTransitionsAreObservable)
{
    const ObjectId remote_id{.session_id = 2, .generation = 1, .local_id = 100};

    bool newly_created = false;
    ASSERT_EQ(
        manager_->RegisterRemoteObject(remote_id, newly_created),
        DAS_S_OK);
    EXPECT_TRUE(newly_created); // 0→1 首次物化

    newly_created = true;
    ASSERT_EQ(
        manager_->RegisterRemoteObject(remote_id, newly_created),
        DAS_S_OK);
    EXPECT_FALSE(newly_created); // dedup = view 递增，非首建

    bool entry_removed = true;
    ASSERT_EQ(manager_->UnregisterObject(remote_id, entry_removed), DAS_S_OK);
    EXPECT_FALSE(entry_removed); // view 归 1，条目仍在
    EXPECT_TRUE(manager_->IsValidObject(remote_id));

    entry_removed = false;
    ASSERT_EQ(manager_->UnregisterObject(remote_id, entry_removed), DAS_S_OK);
    EXPECT_TRUE(entry_removed); // view 归零 → 条目移除
    EXPECT_FALSE(manager_->IsValidObject(remote_id));
}

TEST_F(IpcObjectManagerTest, RemoveIfUnheldAutoEntryOnlyRemovesUnheld)
{
    auto     held_mock = new MockDasObject();
    ObjectId held_id{};
    ASSERT_EQ(
        manager_->RegisterLocalObject(held_mock, held_id, false),
        DAS_S_OK);
    ASSERT_EQ(manager_->AddRemoteHolder(held_id, 7), DAS_S_OK);

    DAS::DasPtr<IDasBase> not_removed;
    ASSERT_EQ(
        manager_->RemoveIfUnheldAutoEntry(held_id, not_removed),
        DAS_S_OK);
    EXPECT_EQ(not_removed.Get(), nullptr); // holders 非空：不动条目
    EXPECT_EQ(manager_->GetLocalEntryCount(), 1u);

    auto     unheld_mock = new MockDasObject();
    ObjectId unheld_id{};
    ASSERT_EQ(
        manager_->RegisterLocalObject(unheld_mock, unheld_id, false),
        DAS_S_OK);

    DAS::DasPtr<IDasBase> removed;
    ASSERT_EQ(manager_->RemoveIfUnheldAutoEntry(unheld_id, removed), DAS_S_OK);
    EXPECT_NE(removed.Get(), nullptr); // holders 空 + pins 0 → 移除移交
    EXPECT_EQ(manager_->GetLocalEntryCount(), 1u);
    EXPECT_FALSE(manager_->IsValidObject(unheld_id));
}
