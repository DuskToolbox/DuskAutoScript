#include <cstdint>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <gtest/gtest.h>

using DAS::Core::IPC::DecodeObjectId;
using DAS::Core::IPC::EncodeObjectId;
using DAS::Core::IPC::IsNullObjectId;
using DAS::Core::IPC::ObjectId;
using DAS::Core::IPC::RemoteObjectInfo;
using DAS::Core::IPC::RemoteObjectRegistry;

// 创建测试用的 DasGuid
DasGuid CreateTestGuid(
    uint32_t data1,
    uint16_t data2,
    uint16_t data3,
    uint8_t  b1,
    uint8_t  b2,
    uint8_t  b3,
    uint8_t  b4,
    uint8_t  b5,
    uint8_t  b6,
    uint8_t  b7,
    uint8_t  b8)
{
    DasGuid guid;
    guid.data1 = data1;
    guid.data2 = data2;
    guid.data3 = data3;
    guid.data4[0] = b1;
    guid.data4[1] = b2;
    guid.data4[2] = b3;
    guid.data4[3] = b4;
    guid.data4[4] = b5;
    guid.data4[5] = b6;
    guid.data4[6] = b7;
    guid.data4[7] = b8;
    return guid;
}

// Test 注册基本功能
TEST(RemoteObjectRegistryTest, RegisterObject_Basic)
{
    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    registry.Clear();

    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    DasResult result =
        registry.RegisterObject(obj_id, iid, 1, "test_object", 1);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_EQ(registry.GetObjectCount(), 1);
    EXPECT_TRUE(registry.ObjectExists(obj_id));
}

// Test 重复注册同一个对象
TEST(RemoteObjectRegistryTest, RegisterObject_Duplicate)
{
    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    registry.Clear();

    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    // 第一次注册成功
    DasResult result1 =
        registry.RegisterObject(obj_id, iid, 1, "test_object", 1);
    EXPECT_EQ(result1, DAS_S_OK);

    // 第二次注册应该失败
    DasResult result2 =
        registry.RegisterObject(obj_id, iid, 1, "test_object", 1);
    EXPECT_EQ(result2, DAS_E_DUPLICATE_ELEMENT);
}

// Test 注册空名称
TEST(RemoteObjectRegistryTest, RegisterObject_EmptyName)
{
    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    registry.Clear();

    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    DasResult result = registry.RegisterObject(obj_id, iid, 1, "", 1);

    EXPECT_EQ(result, DAS_E_INVALID_ARGUMENT);
    EXPECT_EQ(registry.GetObjectCount(), 0);
}

// Test 注册无效ObjectId
TEST(RemoteObjectRegistryTest, RegisterObject_InvalidObjectId)
{
    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    registry.Clear();

    ObjectId obj_id{
        .session_id = 0,
        .generation = 0,
        .local_id = 0}; // 无效ObjectId
    DasGuid iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    DasResult result =
        registry.RegisterObject(obj_id, iid, 1, "test_object", 1);

    EXPECT_EQ(result, DAS_E_IPC_INVALID_OBJECT_ID);
    EXPECT_EQ(registry.GetObjectCount(), 0);
}

// Test 注销对象
TEST(RemoteObjectRegistryTest, UnregisterObject)
{
    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    registry.Clear();

    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    // 先注册对象
    registry.RegisterObject(obj_id, iid, 1, "test_object", 1);
    EXPECT_EQ(registry.GetObjectCount(), 1);

    // 注销对象
    DasResult result = registry.UnregisterObject(obj_id);
    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_EQ(registry.GetObjectCount(), 0);
    EXPECT_FALSE(registry.ObjectExists(obj_id));
}

// Test 注销不存在的对象
TEST(RemoteObjectRegistryTest, UnregisterObject_NotFound)
{
    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    registry.Clear();

    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    // 注册一个不同的对象
    ObjectId other_obj_id{.session_id = 1, .generation = 1, .local_id = 200};
    registry.RegisterObject(other_obj_id, iid, 1, "other_object", 1);

    // 注销不存在的对象
    DasResult result = registry.UnregisterObject(obj_id);
    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);
    EXPECT_EQ(registry.GetObjectCount(), 1); // 其他对象仍然存在
}

// Test 注销指定会话的所有对象
TEST(RemoteObjectRegistryTest, UnregisterAllFromSession)
{
    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    registry.Clear();

    // 注册来自会话1的对象
    ObjectId obj1{.session_id = 1, .generation = 1, .local_id = 100};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);
    registry.RegisterObject(obj1, iid, 1, "object1", 1);

    // 注册来自会话2的对象
    ObjectId obj2{.session_id = 2, .generation = 1, .local_id = 200};
    registry.RegisterObject(obj2, iid, 2, "object2", 1);

    // 注册来自会话1的另一个对象
    ObjectId obj3{.session_id = 1, .generation = 1, .local_id = 300};
    registry.RegisterObject(obj3, iid, 1, "object3", 1);

    EXPECT_EQ(registry.GetObjectCount(), 3);

    // 注销会话1的所有对象
    registry.UnregisterAllFromSession(1);
    DasResult result = DAS_S_OK;
    EXPECT_EQ(registry.GetObjectCount(), 1);
    EXPECT_FALSE(registry.ObjectExists(obj1));
    EXPECT_FALSE(registry.ObjectExists(obj3));
    EXPECT_TRUE(registry.ObjectExists(obj2));
}

// Test 通过名称查找对象
TEST(RemoteObjectRegistryTest, LookupByName)
{
    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    registry.Clear();

    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    registry.RegisterObject(obj_id, iid, 1, "test_object", 1);

    RemoteObjectInfo info;
    DasResult        result = registry.LookupByName("test_object", info);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_EQ(info.object_id.session_id, 1);
    EXPECT_EQ(info.object_id.local_id, 100);
    EXPECT_EQ(info.name, "test_object");
    EXPECT_EQ(info.version, 1);
}

// Test 通过名称查找不存在的对象
TEST(RemoteObjectRegistryTest, LookupByName_NotFound)
{
    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    registry.Clear();

    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    registry.RegisterObject(obj_id, iid, 1, "test_object", 1);

    RemoteObjectInfo info;
    DasResult        result = registry.LookupByName("nonexistent_object", info);

    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);
}

// Test 通过接口类型查找对象
TEST(RemoteObjectRegistryTest, LookupByInterface)
{
    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    registry.Clear();

    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    registry.RegisterObject(obj_id, iid, 1, "test_object", 1);

    RemoteObjectInfo info;
    DasResult        result = registry.LookupByInterface(iid, info);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_EQ(info.object_id.session_id, 1);
    EXPECT_EQ(info.object_id.local_id, 100);
    EXPECT_EQ(info.name, "test_object");
    EXPECT_EQ(info.version, 1);
}

// Test 通过接口类型查找不存在的对象
TEST(RemoteObjectRegistryTest, LookupByInterface_NotFound)
{
    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    registry.Clear();

    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    registry.RegisterObject(obj_id, iid, 1, "test_object", 1);

    DasGuid other_iid = CreateTestGuid(
        0x87654321,
        0x4321,
        0x8765,
        0x21,
        0x43,
        0x65,
        0x87,
        0x65,
        0x43,
        0x21,
        0xF0);

    RemoteObjectInfo info;
    DasResult        result = registry.LookupByInterface(other_iid, info);

    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);
}

// Test 获取对象信息
TEST(RemoteObjectRegistryTest, GetObjectInfo)
{
    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    registry.Clear();

    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    DasGuid  iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    registry.RegisterObject(obj_id, iid, 1, "test_object", 2);

    RemoteObjectInfo info;
    DasResult        result = registry.GetObjectInfo(obj_id, info);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_EQ(info.object_id.session_id, 1);
    EXPECT_EQ(info.object_id.generation, 1);
    EXPECT_EQ(info.object_id.local_id, 100);
    EXPECT_EQ(info.session_id, 1);
    EXPECT_EQ(info.name, "test_object");
    EXPECT_EQ(info.version, 2);
}

// Test 列出所有对象
TEST(RemoteObjectRegistryTest, ListAllObjects)
{
    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    registry.Clear();

    DasGuid iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    // 注册多个对象
    ObjectId obj1{.session_id = 1, .generation = 1, .local_id = 100};
    registry.RegisterObject(obj1, iid, 1, "object1", 1);

    ObjectId obj2{.session_id = 2, .generation = 1, .local_id = 200};
    registry.RegisterObject(obj2, iid, 2, "object2", 1);

    ObjectId obj3{.session_id = 1, .generation = 1, .local_id = 300};
    registry.RegisterObject(obj3, iid, 1, "object3", 1);

    std::vector<RemoteObjectInfo> objects;
    registry.ListAllObjects(objects);
    EXPECT_EQ(objects.size(), 3);

    // 检查所有对象都存在
    bool found1 = false, found2 = false, found3 = false;
    for (const auto& obj : objects)
    {
        if (obj.object_id.local_id == 100)
            found1 = true;
        if (obj.object_id.local_id == 200)
            found2 = true;
        if (obj.object_id.local_id == 300)
            found3 = true;
    }

    EXPECT_TRUE(found1);
    EXPECT_TRUE(found2);
    EXPECT_TRUE(found3);
}

// Test 按会话列出对象
TEST(RemoteObjectRegistryTest, ListObjectsBySession)
{
    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    registry.Clear();

    DasGuid iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    // 注册来自不同会话的对象
    ObjectId obj1{.session_id = 1, .generation = 1, .local_id = 100};
    registry.RegisterObject(obj1, iid, 1, "object1", 1);

    ObjectId obj2{.session_id = 2, .generation = 1, .local_id = 200};
    registry.RegisterObject(obj2, iid, 2, "object2", 1);

    ObjectId obj3{.session_id = 1, .generation = 1, .local_id = 300};
    registry.RegisterObject(obj3, iid, 1, "object3", 1);

    // 列出来自会话1的对象
    std::vector<RemoteObjectInfo> objects;
    registry.ListObjectsBySession(1, objects);
    EXPECT_EQ(objects.size(), 2);

    // 检查只有来自会话1的对象
    bool found1 = false, found3 = false;
    for (const auto& obj : objects)
    {
        EXPECT_EQ(obj.session_id, 1);
        if (obj.object_id.local_id == 100)
            found1 = true;
        if (obj.object_id.local_id == 300)
            found3 = true;
    }

    EXPECT_TRUE(found1);
    EXPECT_TRUE(found3);
}

// Test 清空注册表
TEST(RemoteObjectRegistryTest, Clear)
{
    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    registry.Clear();

    DasGuid iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);

    // 注册多个对象
    ObjectId obj1{.session_id = 1, .generation = 1, .local_id = 100};
    registry.RegisterObject(obj1, iid, 1, "object1", 1);

    ObjectId obj2{.session_id = 2, .generation = 1, .local_id = 200};
    registry.RegisterObject(obj2, iid, 2, "object2", 1);

    EXPECT_EQ(registry.GetObjectCount(), 2);

    // 清空注册表
    registry.Clear();
    EXPECT_EQ(registry.GetObjectCount(), 0);
    EXPECT_FALSE(registry.ObjectExists(obj1));
    EXPECT_FALSE(registry.ObjectExists(obj2));
}

// Test 单例模式
TEST(RemoteObjectRegistryTest, Singleton)
{
    RemoteObjectRegistry& registry1 = RemoteObjectRegistry::GetInstance();
    RemoteObjectRegistry& registry2 = RemoteObjectRegistry::GetInstance();

    // 应该是同一个实例
    EXPECT_EQ(&registry1, &registry2);

    // 测试在一个实例上的修改在另一个实例上可见
    registry1.Clear();
    DasGuid iid = CreateTestGuid(
        0x12345678,
        0x1234,
        0x5678,
        0x12,
        0x34,
        0x56,
        0x78,
        0x9A,
        0xBC,
        0xDE,
        0xF0);
    ObjectId obj_id{.session_id = 1, .generation = 1, .local_id = 100};
    registry1.RegisterObject(obj_id, iid, 1, "test_object", 1);

    EXPECT_EQ(registry2.GetObjectCount(), 1);
    EXPECT_TRUE(registry2.ObjectExists(obj_id));
}