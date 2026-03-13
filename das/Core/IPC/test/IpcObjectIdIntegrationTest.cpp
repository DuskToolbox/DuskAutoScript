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
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/IPC/SessionCoordinator.h>
#include <das/DasTypes.hpp>
#include <das/IDasBase.h>
#include <gtest/gtest.h>

using DAS::Core::IPC::DecodeObjectId;
using DAS::Core::IPC::DistributedObjectManager;
using DAS::Core::IPC::EncodeObjectId;
using DAS::Core::IPC::IsNullObjectId;
using DAS::Core::IPC::ObjectId;
using DAS::Core::IPC::ProxyFactory;
using DAS::Core::IPC::RemoteObjectInfo;
using DAS::Core::IPC::RemoteObjectRegistry;
using DAS::Core::IPC::SessionCoordinator;

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
        // 初始化 SessionCoordinator
        session_coordinator_ = &SessionCoordinator::GetInstance();

        // 设置本地 session_id (主进程)
        session_coordinator_->SetLocalSessionId(1);

        // 获取 RemoteObjectRegistry 实例
        registry_ = &RemoteObjectRegistry::GetInstance();
        registry_->Clear();

        // 创建 DistributedObjectManager 实例
        object_manager_ = std::make_unique<DistributedObjectManager>();
    }

    void TearDown() override
    {
        registry_->Clear();
        object_manager_.reset();

        // 清理 ProxyFactory
        ProxyFactory::GetInstance().ClearAllProxies();
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

    SessionCoordinator*          session_coordinator_;
    RemoteObjectRegistry*        registry_;
    std::unique_ptr<DistributedObjectManager> object_manager_;
};

// ====== ObjectId 基本功能测试 ======

TEST_F(IpcObjectIdIntegrationTest, ObjectId_EncodeDecode)
{
    // 测试 ObjectId 编码/解码
    ObjectId original{
        .session_id = 1,
        .generation = 5,
        .local_id = 100
    };

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
    void*              test_object = reinterpret_cast<void*>(0x12345678);
    ObjectId           out_object_id{};
    DasResult result = object_manager_->RegisterLocalObject(test_object, out_object_id);

    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(out_object_id.session_id, 1u);  // 主进程 session_id
    EXPECT_EQ(out_object_id.generation, 1u);
    EXPECT_EQ(out_object_id.local_id, 1u);

    // 验证对象是本地对象
    EXPECT_TRUE(object_manager_->IsLocalObject(out_object_id));
}

TEST_F(IpcObjectIdIntegrationTest, DistributedObjectManager_RegisterRemoteObject)
{
    // 测试注册远程对象
    ObjectId remote_id{2, 1, 100};  // session_id=2 表示远程 Host
    DasResult result = object_manager_->RegisterRemoteObject(remote_id);

    ASSERT_EQ(result, DAS_S_OK);

    // 验证对象不是本地对象
    EXPECT_FALSE(object_manager_->IsLocalObject(remote_id));
}

TEST_F(IpcObjectIdIntegrationTest, DistributedObjectManager_LookupObject)
{
    // 注册本地对象
    void*    test_object = reinterpret_cast<void*>(0x12345678);
    ObjectId object_id{};
    ASSERT_EQ(object_manager_->RegisterLocalObject(test_object, object_id),
              DAS_S_OK);

    // 查找对象
    void* found_object = nullptr;
    DasResult result = object_manager_->LookupObject(object_id, &found_object);

    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(found_object, test_object);
}

TEST_F(IpcObjectIdIntegrationTest, DistributedObjectManager_IsValidObject)
{
    // 注册本地对象
    void*    test_object = reinterpret_cast<void*>(0x12345678);
    ObjectId object_id{};
    ASSERT_EQ(object_manager_->RegisterLocalObject(test_object, object_id),
              DAS_S_OK);

    // 验证有效性
    EXPECT_TRUE(object_manager_->IsValidObject(object_id));

    // 测试无效 ObjectId
    ObjectId invalid_id{1, 1, 9999};
    EXPECT_FALSE(object_manager_->IsValidObject(invalid_id));
}

TEST_F(IpcObjectIdIntegrationTest, DistributedObjectManager_RefCount)
{
    // 注册本地对象
    void*    test_object = reinterpret_cast<void*>(0x12345678);
    ObjectId object_id{};
    ASSERT_EQ(object_manager_->RegisterLocalObject(test_object, object_id),
              DAS_S_OK);

    // AddRef
    ASSERT_EQ(object_manager_->AddRef(object_id), DAS_S_OK);
    ASSERT_EQ(object_manager_->AddRef(object_id), DAS_S_OK);

    // Release 两次
    ASSERT_EQ(object_manager_->Release(object_id), DAS_S_OK);
    ASSERT_EQ(object_manager_->Release(object_id), DAS_S_OK);

    // 对象应该仍然有效（因为 RegisterLocalObject 增加了初始引用）
    // 再次 Release 应该能注销
    ASSERT_EQ(object_manager_->Release(object_id), DAS_S_OK);
}

// ====== RemoteObjectRegistry 测试 ======

TEST_F(IpcObjectIdIntegrationTest, RemoteObjectRegistry_RegisterObject)
{
    // 在 RemoteObjectRegistry 中注册远程对象
    ObjectId    obj_id{2, 1, 100};  // Host session_id = 2
    DasGuid     iid = CreateTestGuid(1);
    std::string name = "TestRemoteObject";

    DasResult result =
        registry_->RegisterObject(obj_id, iid, 2, name, 1);
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
    ObjectId    obj_id{2, 1, 100};
    DasGuid     iid = CreateTestGuid(1);

    ASSERT_EQ(registry_->RegisterObject(obj_id, iid, 2, "TestObject", 1),
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
    ObjectId host_obj_id{2, 1, 1};  // session_id=2 (Host), generation=1, local_id=1
    DasGuid  iid = CreateTestGuid(100);

    // 2. 主进程收到 REGISTER_OBJECT 消息后，在 RemoteObjectRegistry 中注册
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

    ASSERT_EQ(registry_->RegisterObject(obj_id, iid, 2, "TestObject", 1),
              DAS_S_OK);

    // 2. Host 进程发送 UNREGISTER_OBJECT 消息
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
    void*    local_object = reinterpret_cast<void*>(0x1000);
    ObjectId local_id{};
    ASSERT_EQ(object_manager_->RegisterLocalObject(local_object, local_id),
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
    void*    test_object = reinterpret_cast<void*>(0x2000);
    ObjectId object_id{};
    ASSERT_EQ(object_manager_->RegisterLocalObject(test_object, object_id),
              DAS_S_OK);

    // 2. 查找本地对象
    void* found = nullptr;
    DasResult result = object_manager_->LookupObject(object_id, &found);

    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(found, test_object);

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
    void* found = nullptr;
    DasResult result = object_manager_->LookupObject(remote_id, &found);

    // 远程对象在本地没有指针，返回错误是预期行为
    EXPECT_NE(result, DAS_S_OK);
}

// ====== 引用计数分布式测试 ======

TEST_F(IpcObjectIdIntegrationTest, DistributedRefCount_LocalAndRemote)
{
    // 测试本地和远程引用计数分离
    // 1. Host 进程注册对象
    ObjectId obj_id{2, 1, 1};
    ASSERT_EQ(object_manager_->RegisterRemoteObject(obj_id), DAS_S_OK);

    // 2. 本地 AddRef
    ASSERT_EQ(object_manager_->AddRef(obj_id), DAS_S_OK);

    // 3. 模拟远程 AddRef
    ASSERT_EQ(object_manager_->HandleRemoteAddRef(obj_id), DAS_S_OK);

    // 4. 本地 Release（不应销毁，因为远程还有引用）
    ASSERT_EQ(object_manager_->Release(obj_id), DAS_S_OK);
    EXPECT_TRUE(object_manager_->IsValidObject(obj_id));

    // 5. 远程 Release（应销毁）
    ASSERT_EQ(object_manager_->HandleRemoteRelease(obj_id), DAS_S_OK);
    EXPECT_FALSE(object_manager_->IsValidObject(obj_id));
}

// ====== 接口指针参数测试 (单元测试级别) ======

TEST_F(IpcObjectIdIntegrationTest, InterfacePointer_EncodeDecode)
{
    // 测试接口指针的 ObjectId 编码/解码
    // 这模拟了接口指针参数序列化场景

    // 1. 创建本地对象并获取 ObjectId
    void*    interface_ptr = reinterpret_cast<void*>(0x3000);
    ObjectId object_id{};
    ASSERT_EQ(object_manager_->RegisterLocalObject(interface_ptr, object_id),
              DAS_S_OK);

    // 2. 编码 ObjectId（模拟序列化）
    uint64_t encoded = EncodeObjectId(object_id);
    EXPECT_NE(encoded, 0u);

    // 3. 解码 ObjectId（模拟反序列化）
    ObjectId decoded = DecodeObjectId(encoded);

    // 4. 查找对象
    void* found_ptr = nullptr;
    DasResult result = object_manager_->LookupObject(decoded, &found_ptr);

    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(found_ptr, interface_ptr);
}

// ====== Session 管理测试 ======

TEST_F(IpcObjectIdIntegrationTest, SessionCoordinator_SetLocalSessionId)
{
    // 测试设置本地 session_id
    uint16_t main_session = 1;
    uint16_t host_session = 2;

    session_coordinator_->SetLocalSessionId(main_session);
    EXPECT_EQ(*session_coordinator_->GetLocalSessionId(), main_session);

    session_coordinator_->SetLocalSessionId(host_session);
    EXPECT_EQ(*session_coordinator_->GetLocalSessionId(), host_session);
}

TEST_F(IpcObjectIdIntegrationTest, SessionCoordinator_AllocateSessionId)
{
    // 测试动态分配 session_id
    uint16_t session1 = session_coordinator_->AllocateSessionId();
    EXPECT_TRUE(SessionCoordinator::IsValidSessionId(session1));

    uint16_t session2 = session_coordinator_->AllocateSessionId();
    EXPECT_TRUE(SessionCoordinator::IsValidSessionId(session2));
    EXPECT_NE(session1, session2);
}
