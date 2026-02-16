/**
 * @file RemoteObjectRegistryIntegrationTest.cpp
 * @brief RemoteObjectRegistry 集成测试
 *
 * ⚠️ [未来移除] 此测试将被 IPC端到端多进程测试计划 替代
 *
 * 计划位置: .sisyphus/plans/IPC端到端多进程测试计划.md
 * 替代测试: IpcE2EMultiProcessTest.cpp (Task 8)
 *
 * 原因: 端到端测试会真实模拟多进程场景，比集成测试更全面
 *
 * TODO: 当 IpcE2EMultiProcessTest 完成时，删除此文件
 */

#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/IPC/SessionCoordinator.h>
#include <das/IDasBase.h>
#include <gtest/gtest.h>

using DAS::Core::IPC::DecodeObjectId;
using DAS::Core::IPC::EncodeObjectId;
using DAS::Core::IPC::IsNullObjectId;
using DAS::Core::IPC::ObjectId;
using DAS::Core::IPC::RemoteObjectInfo;
using DAS::Core::IPC::RemoteObjectRegistry;
using DAS::Core::IPC::SessionCoordinator;

class RemoteObjectRegistryIntegrationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        session_coordinator_ = &SessionCoordinator::GetInstance();
        registry_ = &RemoteObjectRegistry::GetInstance();
        registry_->Clear();
    }

    void TearDown() override { registry_->Clear(); }

    DasGuid CreateTestGuid(uint32_t seed)
    {
        DasGuid guid{};
        guid.data1 = seed;
        guid.data2 = static_cast<uint16_t>(seed >> 16);
        guid.data3 = static_cast<uint16_t>(seed >> 8);
        for (int i = 0; i < 8; ++i)
        {
            guid.data4[i] = static_cast<uint8_t>(seed + i);
        }
        return guid;
    }

    SessionCoordinator*   session_coordinator_;
    RemoteObjectRegistry* registry_;
};

// ====== Session ID Allocation and Usage Tests ======

TEST_F(RemoteObjectRegistryIntegrationTest, SessionIdAllocationAndUsage)
{
    uint16_t host_session_id = session_coordinator_->AllocateSessionId();
    ASSERT_NE(host_session_id, 0);
    ASSERT_TRUE(SessionCoordinator::IsValidSessionId(host_session_id));

    ObjectId    obj_id = {host_session_id, 1, 100};
    DasGuid     iid = CreateTestGuid(1);
    std::string name = "TestObject";

    DasResult result =
        registry_->RegisterObject(obj_id, iid, host_session_id, name, 1);
    ASSERT_EQ(result, DAS_S_OK);

    std::vector<RemoteObjectInfo> objects;
    registry_->ListObjectsBySession(host_session_id, objects);
    ASSERT_EQ(objects.size(), 1);
    EXPECT_EQ(objects[0].name, "TestObject");
    EXPECT_EQ(objects[0].session_id, host_session_id);

    RemoteObjectInfo found_info;
    result = registry_->LookupByName("TestObject", found_info);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(found_info.session_id, host_session_id);
    EXPECT_EQ(found_info.object_id.local_id, 100);

    session_coordinator_->ReleaseSessionId(host_session_id);
}

// ====== Multiple Hosts Tests ======

TEST_F(RemoteObjectRegistryIntegrationTest, MultipleHosts)
{
    std::vector<uint16_t> host_session_ids;

    for (int i = 0; i < 3; ++i)
    {
        uint16_t session_id = session_coordinator_->AllocateSessionId();
        host_session_ids.push_back(session_id);
        ASSERT_TRUE(SessionCoordinator::IsValidSessionId(session_id));
    }

    for (size_t i = 0; i < host_session_ids.size(); ++i)
    {
        ObjectId obj_id = {
            host_session_ids[i],
            1,
            static_cast<uint32_t>(i + 1)};
        DasGuid     iid = CreateTestGuid(static_cast<uint32_t>(i));
        std::string name = "HostObject_" + std::to_string(i);

        DasResult result =
            registry_
                ->RegisterObject(obj_id, iid, host_session_ids[i], name, 1);
        ASSERT_EQ(result, DAS_S_OK);
    }

    for (size_t i = 0; i < host_session_ids.size(); ++i)
    {
        std::vector<RemoteObjectInfo> objects;
        registry_->ListObjectsBySession(host_session_ids[i], objects);
        ASSERT_EQ(objects.size(), 1);
        EXPECT_EQ(objects[0].name, "HostObject_" + std::to_string(i));

        RemoteObjectInfo found_info;
        DasResult        result = registry_->LookupByName(
            "HostObject_" + std::to_string(i),
            found_info);
        ASSERT_EQ(result, DAS_S_OK);
        EXPECT_EQ(found_info.session_id, host_session_ids[i]);
    }

    for (uint16_t id : host_session_ids)
    {
        session_coordinator_->ReleaseSessionId(id);
    }
}

// ====== Session ID Reuse Tests ======

TEST_F(RemoteObjectRegistryIntegrationTest, SessionIdReuse)
{
    uint16_t session_id1 = session_coordinator_->AllocateSessionId();

    ObjectId    obj_id1 = {session_id1, 1, 100};
    DasGuid     iid1 = CreateTestGuid(1);
    std::string name1 = "Object1";

    DasResult result =
        registry_->RegisterObject(obj_id1, iid1, session_id1, name1, 1);
    ASSERT_EQ(result, DAS_S_OK);

    std::vector<RemoteObjectInfo> objects;
    registry_->ListObjectsBySession(session_id1, objects);
    ASSERT_EQ(objects.size(), 1);

    session_coordinator_->ReleaseSessionId(session_id1);

    registry_->ListObjectsBySession(session_id1, objects);
    ASSERT_EQ(objects.size(), 1);

    result = registry_->UnregisterObject(obj_id1);
    ASSERT_EQ(result, DAS_S_OK);

    registry_->ListObjectsBySession(session_id1, objects);
    ASSERT_EQ(objects.size(), 0);

    uint16_t session_id2 = session_coordinator_->AllocateSessionId();
    EXPECT_EQ(session_id1, session_id2);

    ObjectId    obj_id2 = {session_id2, 1, 101};
    DasGuid     iid2 = CreateTestGuid(2);
    std::string name2 = "Object2";

    result = registry_->RegisterObject(obj_id2, iid2, session_id2, name2, 1);
    ASSERT_EQ(result, DAS_S_OK);

    registry_->ListObjectsBySession(session_id2, objects);
    ASSERT_EQ(objects.size(), 1);
    EXPECT_EQ(objects[0].name, "Object2");

    session_coordinator_->ReleaseSessionId(session_id2);
}

// ====== Edge Cases Tests ======

TEST_F(RemoteObjectRegistryIntegrationTest, EdgeCases)
{
    uint16_t    session_id = session_coordinator_->AllocateSessionId();
    ObjectId    obj_id = {session_id, 1, 100};
    DasGuid     iid = CreateTestGuid(1);
    std::string name = "TestObject";
    DasResult   result =
        registry_->RegisterObject(obj_id, iid, session_id, name, 1);
    ASSERT_EQ(result, DAS_S_OK);

    RemoteObjectInfo not_found;
    result = registry_->LookupByName("NonExistent", not_found);
    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);

    ObjectId non_existent_id = {1, 1, 999};
    result = registry_->UnregisterObject(non_existent_id);
    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);

    RemoteObjectInfo info;
    result = registry_->GetObjectInfo(non_existent_id, info);
    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);

    session_coordinator_->ReleaseSessionId(session_id);
}

// ====== Duplicate Names Tests ======

TEST_F(RemoteObjectRegistryIntegrationTest, DuplicateNames)
{
    uint16_t session_id1 = session_coordinator_->AllocateSessionId();
    uint16_t session_id2 = session_coordinator_->AllocateSessionId();

    ObjectId  obj_id1 = {session_id1, 1, 100};
    DasGuid   iid1 = CreateTestGuid(1);
    DasResult result =
        registry_
            ->RegisterObject(obj_id1, iid1, session_id1, "DuplicateName", 1);
    ASSERT_EQ(result, DAS_S_OK);

    ObjectId obj_id2 = {session_id2, 1, 100};
    DasGuid  iid2 = CreateTestGuid(2);
    result =
        registry_
            ->RegisterObject(obj_id2, iid2, session_id2, "DuplicateName", 1);
    EXPECT_EQ(result, DAS_E_DUPLICATE_ELEMENT);

    RemoteObjectInfo found_info;
    result = registry_->LookupByName("DuplicateName", found_info);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(found_info.session_id, session_id1);

    RemoteObjectInfo info1;
    result = registry_->LookupByInterface(
        RemoteObjectRegistry::ComputeInterfaceId(iid1),
        info1);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(info1.session_id, session_id1);

    session_coordinator_->ReleaseSessionId(session_id1);
    session_coordinator_->ReleaseSessionId(session_id2);
}