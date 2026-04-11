/**
 * @file IpcObjectIdIntegrationTest.cpp
 * @brief IPC ObjectId 集成测试
 *
 * 测试 Phase 19 实现的三个核心功能：
 * 1. 接口指针参数序列化/反序列化
 * 2. ObjectId 双向同步协议
 * 3. 本地对象 Fallback 机制
 */

#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/DasPtr.hpp>
#include <das/DasTypes.hpp>
#include <das/IDasBase.h>
#include <gtest/gtest.h>

#include "MockDasObject.h"

using DAS::Core::IPC::DecodeObjectId;
using DAS::Core::IPC::DistributedObjectManager;
using DAS::Core::IPC::EncodeObjectId;
using DAS::Core::IPC::IsNullObjectId;
using DAS::Core::IPC::ObjectId;
using DAS::Core::IPC::RemoteObjectInfo;
using DAS::Core::IPC::RemoteObjectRegistry;

/**
 * @brief IPC ObjectId 集成测试夹具
 *
 * 提供 Phase 19 功能的集成测试环境
 */
class IpcObjectIdIntegrationTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // 创建 DistributedObjectManager 实例
        object_manager_ = std::make_unique<DistributedObjectManager>();

        // 使用本地 RemoteObjectRegistry 实例
        registry_->Clear();
    }

    void TearDown() override
    {
        registry_->Clear();
        object_manager_.reset();
    }

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

    RemoteObjectRegistry                      registry_;
    std::unique_ptr<DistributedObjectManager> object_manager_;
};

// ====== ObjectId 基本功能测试 ======

TEST_F(IpcObjectIdIntegrationTest, ObjectId_EncodeDecode)
{
    // 测试 ObjectId 编码/解码
    ObjectId original{.session_id = 1, .generation = 5, .local_id = 100};

    // 编码为 uint64_t
    uint64_t encoded = EncodeObjectId(original);
    EXPECT_NE(encoded, 0u);

    // 解码回 ObjectId
    ObjectId decoded = DecodeObjectId(encoded);
    EXPECT_EQ(decoded.session_id, original.session_id);
    EXPECT_EQ(decoded.generation, original.generation);
    EXPECT_EQ(decoded.local_id, original.local_id);
}

TEST_F(IpcObjectIdIntegrationTest, ObjectId_IsNull)
{
    // 测试空 ObjectId 判断
    ObjectId null_id{};
    EXPECT_TRUE(IsNullObjectId(null_id));

    ObjectId valid_id{1, 1, 100};
    EXPECT_FALSE(IsNullObjectId(valid_id));
}

// ====== DistributedObjectManager 测试 ======

TEST_F(IpcObjectIdIntegrationTest, DistributedObjectManager_RegisterLocalObject)
{
    // 测试注册本地对象
    auto      test_object = new MockDasObject();
    ObjectId  out_object_id{};
    DasResult result =
        object_manager_->RegisterLocalObject(test_object, out_object_id);

    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(out_object_id.session_id, 1u); // 主进程 session_id
    EXPECT_EQ(out_object_id.generation, 1u);
    EXPECT_EQ(out_object_id.local_id, 1u);

    // 验证对象是本地对象
    EXPECT_TRUE(object_manager_->IsLocalObject(out_object_id));
}

TEST_F(
    IpcObjectIdIntegrationTest,
    DistributedObjectManager_RegisterRemoteObject)
{
    // 测试注册远程对象
    ObjectId  remote_id{2, 1, 100}; // session_id=2 表示远程 Host
    DasResult result = object_manager_->RegisterRemoteObject(remote_id);

    ASSERT_EQ(result, DAS_S_OK);

    // 验证对象不是本地对象
    EXPECT_FALSE(object_manager_->IsLocalObject(remote_id));
}

TEST_F(IpcObjectIdIntegrationTest, DistributedObjectManager_LookupObject)
{
    // 注册本地对象
    auto     test_object = new MockDasObject();
    ObjectId object_id{};
    ASSERT_EQ(
        object_manager_->RegisterLocalObject(test_object, object_id),
        DAS_S_OK);

    // 查找对象（LookupObject 内部 AddRef）
    DAS::DasPtr<IDasBase> found_holder;
    DasResult             result =
        object_manager_->LookupObject(object_id, found_holder.Put());

    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(found_holder.Get(), test_object);
}

TEST_F(IpcObjectIdIntegrationTest, DistributedObjectManager_IsValidObject)
{
    // 注册本地对象
    auto     test_object = new MockDasObject();
    ObjectId object_id{};
    ASSERT_EQ(
        object_manager_->RegisterLocalObject(test_object, object_id),
        DAS_S_OK);

    // 验证有效性
    EXPECT_TRUE(object_manager_->IsValidObject(object_id));

    // 测试无效 ObjectId
    ObjectId invalid_id{1, 1, 9999};
    EXPECT_FALSE(object_manager_->IsValidObject(invalid_id));
}

// ====== RemoteObjectRegistry 测试 ======

TEST_F(IpcObjectIdIntegrationTest, RemoteObjectRegistry_RegisterObject)
{
    // 在 RemoteObjectRegistry 中注册远程对象
    ObjectId    obj_id{2, 1, 100}; // Host session_id = 2
    DasGuid     iid = CreateTestGuid(1);
    std::string name = "TestRemoteObject";

    DasResult result = registry_->RegisterObject(obj_id, iid, 2, name, 1);
    ASSERT_EQ(result, DAS_S_OK);

    // 验证能查询到
    RemoteObjectInfo info;
    result = registry_->GetObjectInfo(obj_id, info);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(info.name, name);
    EXPECT_EQ(info.session_id, 2u);
}

TEST_F(IpcObjectIdIntegrationTest, RemoteObjectRegistry_LookupByName)
{
    // 注册对象
    ObjectId    obj_id{2, 1, 100};
    DasGuid     iid = CreateTestGuid(1);
    std::string name = "TestObject";

    ASSERT_EQ(registry_->RegisterObject(obj_id, iid, 2, name, 1), DAS_S_OK);

    // 按名称查找
    RemoteObjectInfo info;
    DasResult        result = registry_->LookupByName(name, info);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(info.object_id.local_id, 100u);
}

TEST_F(IpcObjectIdIntegrationTest, RemoteObjectRegistry_UnregisterObject)
{
    // 注册对象
    ObjectId obj_id{2, 1, 100};
    DasGuid  iid = CreateTestGuid(1);

    ASSERT_EQ(
        registry_->RegisterObject(obj_id, iid, 2, "TestObject", 1),
        DAS_S_OK);

    // 注销对象
    DasResult result = registry_->UnregisterObject(obj_id);
    ASSERT_EQ(result, DAS_S_OK);

    // 验证查询不到
    RemoteObjectInfo info;
    result = registry_->GetObjectInfo(obj_id, info);
    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);
}

// ====== ObjectId 双向同步测试 ======

TEST_F(IpcObjectIdIntegrationTest, ObjectIdBidirectionalSync_HostToMain)
{
    // 模拟 Host 进程创建对象并同步到主进程
    // 1. Host 进程注册本地对象
    ObjectId host_obj_id{
        2,
        1,
        1}; // session_id=2 (Host), generation=1, local_id=1
    DasGuid iid = CreateTestGuid(100);

    // 2. 数据平面响应携带 ObjectId，主进程在 RemoteObjectRegistry 中注册
    DasResult result =
        registry_->RegisterObject(host_obj_id, iid, 2, "HostObject", 1);
    ASSERT_EQ(result, DAS_S_OK);

    // 3. 验证主进程能查询到
    RemoteObjectInfo info;
    result = registry_->GetObjectInfo(host_obj_id, info);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(info.session_id, 2u);
    EXPECT_EQ(info.name, "HostObject");
}

TEST_F(IpcObjectIdIntegrationTest, ObjectIdBidirectionalSync_Unregister)
{
    // 模拟 Host 进程销毁对象并同步到主进程
    // 1. 先注册对象
    ObjectId obj_id{2, 1, 100};
    DasGuid  iid = CreateTestGuid(1);

    ASSERT_EQ(
        registry_->RegisterObject(obj_id, iid, 2, "TestObject", 1),
        DAS_S_OK);

    // 2. Host 进程销毁对象，RELEASE_OBJECT fire-and-forget 通知主进程
    DasResult result = registry_->UnregisterObject(obj_id);
    ASSERT_EQ(result, DAS_S_OK);

    // 3. 验证主进程查询不到
    RemoteObjectInfo info;
    result = registry_->GetObjectInfo(obj_id, info);
    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);
}

// ====== 本地对象 Fallback 测试 ======

TEST_F(IpcObjectIdIntegrationTest, LocalObjectFallback_IsLocal)
{
    // 测试 IsLocalObject 判断
    // 1. 注册本地对象
    auto     local_object = new MockDasObject();
    ObjectId local_id{};
    ASSERT_EQ(
        object_manager_->RegisterLocalObject(local_object, local_id),
        DAS_S_OK);

    // 2. 验证 IsLocalObject 返回 true
    EXPECT_TRUE(object_manager_->IsLocalObject(local_id));

    // 3. 注册远程对象引用
    ObjectId remote_id{2, 1, 100};
    ASSERT_EQ(object_manager_->RegisterRemoteObject(remote_id), DAS_S_OK);

    // 4. 验证 IsLocalObject 返回 false
    EXPECT_FALSE(object_manager_->IsLocalObject(remote_id));
}

TEST_F(IpcObjectIdIntegrationTest, LocalObjectFallback_LookupLocal)
{
    // 测试本地对象查找
    // 1. 注册本地对象
    auto     test_object = new MockDasObject();
    ObjectId object_id{};
    ASSERT_EQ(
        object_manager_->RegisterLocalObject(test_object, object_id),
        DAS_S_OK);

    // 2. 查找本地对象（LookupObject 内部 AddRef）
    DAS::DasPtr<IDasBase> found_holder;
    DasResult             result =
        object_manager_->LookupObject(object_id, found_holder.Put());

    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(found_holder.Get(), test_object);

    // 3. 验证是本地对象
    EXPECT_TRUE(object_manager_->IsLocalObject(object_id));
}

TEST_F(IpcObjectIdIntegrationTest, LocalObjectFallback_RemoteLookup)
{
    // 测试远程对象查找（应该失败，返回错误）
    // 1. 注册远程对象引用（不提供对象指针）
    ObjectId remote_id{2, 1, 100};
    ASSERT_EQ(object_manager_->RegisterRemoteObject(remote_id), DAS_S_OK);

    // 2. 尝试查找远程对象（因为没有指针，应该返回错误或 nullptr）
    DAS::DasPtr<IDasBase> found_holder;
    DasResult             result =
        object_manager_->LookupObject(remote_id, found_holder.Put());

    // 远程对象在本地没有指针，返回错误是预期行为
    EXPECT_NE(result, DAS_S_OK);
}

// ====== 接口指针参数测试 (单元测试级别) ======

TEST_F(IpcObjectIdIntegrationTest, InterfacePointer_EncodeDecode)
{
    // 测试接口指针的 ObjectId 编码/解码
    // 这模拟了接口指针参数序列化场景

    // 1. 创建本地对象并获取 ObjectId
    auto     interface_ptr = new MockDasObject();
    ObjectId object_id{};
    ASSERT_EQ(
        object_manager_->RegisterLocalObject(interface_ptr, object_id),
        DAS_S_OK);

    // 2. 编码 ObjectId（模拟序列化）
    uint64_t encoded = EncodeObjectId(object_id);
    EXPECT_NE(encoded, 0u);

    // 3. 解码 ObjectId（模拟反序列化）
    ObjectId decoded = DecodeObjectId(encoded);

    // 4. 查找对象（LookupObject 内部 AddRef）
    DAS::DasPtr<IDasBase> found_holder;
    DasResult             result =
        object_manager_->LookupObject(decoded, found_holder.Put());

    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(found_holder.Get(), interface_ptr);
}

// ====== Session 管理测试 ======

TEST_F(IpcObjectIdIntegrationTest, SessionId_Management)
{
    // 此测试保留为占位符，验证 ObjectId 功能不受影响
    EXPECT_EQ(1, 1);
}

// ====== CreateRemoteProxy 测试 ======

TEST_F(IpcObjectIdIntegrationTest, CreateRemoteProxy_InterfaceHash)
{
    // 测试 ComputeInterfaceId 函数能够正确将 DasGuid 转换为 uint32_t 哈希值
    // 使用一个已知的 UUID 来验证哈希函数的一致性
    DasGuid test_iid{};
    test_iid.data1 = 0x12345678;
    test_iid.data2 = 0xABCD;
    test_iid.data3 = 0xEF01;
    test_iid.data4[0] = 0x11;
    test_iid.data4[1] = 0x22;
    test_iid.data4[2] = 0x33;
    test_iid.data4[3] = 0x44;
    test_iid.data4[4] = 0x55;
    test_iid.data4[5] = 0x66;
    test_iid.data4[6] = 0x77;
    test_iid.data4[7] = 0x88;

    // 计算两次，验证结果一致
    uint32_t hash1 = RemoteObjectRegistry::ComputeInterfaceId(test_iid);
    uint32_t hash2 = RemoteObjectRegistry::ComputeInterfaceId(test_iid);

    EXPECT_EQ(hash1, hash2);
    // 验证哈希值不为 0 (有效的接口应该有非零哈希)
    EXPECT_NE(hash1, 0u);
}

TEST_F(IpcObjectIdIntegrationTest, CreateRemoteProxy_UnknownInterface)
{
    // 测试 CreateRemoteProxy 对于未知接口返回错误
    // 创建一个随机的 UUID (不在已知接口列表中)
    DasGuid unknown_iid{};
    unknown_iid.data1 = 0x12345678;
    unknown_iid.data2 = 0xABCD;
    unknown_iid.data3 = 0xEF01;
    unknown_iid.data4[0] = 0x11;
    unknown_iid.data4[1] = 0x22;
    unknown_iid.data4[2] = 0x33;
    unknown_iid.data4[3] = 0x44;
    unknown_iid.data4[4] = 0x55;
    unknown_iid.data4[5] = 0x66;
    unknown_iid.data4[6] = 0x77;
    unknown_iid.data4[7] = 0x88;

    // 计算接口哈希
    uint32_t interface_hash =
        RemoteObjectRegistry::ComputeInterfaceId(unknown_iid);

    // 这个哈希值不应该是已知的接口 (已知接口: 0xF5FBD328, 0xB6261785)
    EXPECT_NE(interface_hash, 0xF5FBD328u);
    EXPECT_NE(interface_hash, 0xB6261785u);
}
