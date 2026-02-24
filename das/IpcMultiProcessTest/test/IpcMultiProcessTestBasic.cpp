/**
 * @file IpcMultiProcessTestBasic.cpp
 * @brief IPC 多进程单元测试 - Mock/单元测试（不启动真实进程）
 *
 * 这些测试不需要启动 DasHost.exe，直接测试 IPC 组件的功能。
 *
 * 测试覆盖：
 * - SessionCoordinator: Session ID 分配与释放
 * - ObjectId: 编解码与空值检查
 * - RemoteObjectRegistry: 远程对象注册与查找
 * - Handshake: 握手协议结构初始化
 * - 消息队列/共享内存名称生成
 */

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/Host/HostConfig.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/IPC/SessionCoordinator.h>
#include <das/Utils/fmt.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>
#include <unordered_set>

// ====== 基础测试 ======

TEST(IpcMultiProcessTestBasic, BasicTest)
{
    // 基本测试，验证测试框架正常工作
    EXPECT_TRUE(true);
}

TEST(IpcMultiProcessTestBasic, DirectoryStructureTest)
{
    // 验证目录结构能够被正确包含
    // 测试编译时期能够找到相关的头文件
    EXPECT_TRUE(true);
}

// ====== SessionCoordinator 测试 ======

TEST(IpcMultiProcessTestBasic, SessionCoordinator_AllocateAndRelease)
{
    DAS::Core::IPC::SessionCoordinator& coordinator =
        DAS::Core::IPC::SessionCoordinator::GetInstance();

    uint16_t session_id = coordinator.AllocateSessionId();
    EXPECT_NE(session_id, static_cast<uint16_t>(0));
    EXPECT_TRUE(
        DAS::Core::IPC::SessionCoordinator::IsValidSessionId(session_id));

    coordinator.ReleaseSessionId(session_id);
}

TEST(IpcMultiProcessTestBasic, SessionCoordinator_MultipleAllocation)
{
    DAS::Core::IPC::SessionCoordinator& coordinator =
        DAS::Core::IPC::SessionCoordinator::GetInstance();

    std::vector<uint16_t> session_ids;
    for (int i = 0; i < 10; ++i)
    {
        uint16_t id = coordinator.AllocateSessionId();
        EXPECT_NE(id, static_cast<uint16_t>(0));
        session_ids.push_back(id);
    }

    // 验证所有 ID 唯一
    std::unordered_set<uint16_t> unique_ids(
        session_ids.begin(),
        session_ids.end());
    EXPECT_EQ(unique_ids.size(), session_ids.size());

    // 释放所有 ID
    for (uint16_t id : session_ids)
    {
        coordinator.ReleaseSessionId(id);
    }
}

TEST(IpcMultiProcessTestBasic, SessionCoordinator_InvalidId)
{
    EXPECT_FALSE(DAS::Core::IPC::SessionCoordinator::IsValidSessionId(0));
    EXPECT_FALSE(DAS::Core::IPC::SessionCoordinator::IsValidSessionId(0xFFFF));
}

// ====== ObjectId 编解码测试 ======

TEST(IpcMultiProcessTestBasic, ObjectIdEncodingDecoding)
{
    DAS::Core::IPC::ObjectId original = {2, 1, 100};

    uint64_t                 encoded = DAS::Core::IPC::EncodeObjectId(original);
    DAS::Core::IPC::ObjectId decoded = DAS::Core::IPC::DecodeObjectId(encoded);

    EXPECT_EQ(decoded.session_id, original.session_id);
    EXPECT_EQ(decoded.generation, original.generation);
    EXPECT_EQ(decoded.local_id, original.local_id);
    EXPECT_EQ(decoded, original);
}

TEST(IpcMultiProcessTestBasic, ObjectIdNullCheck)
{
    DAS::Core::IPC::ObjectId null_id = {0, 0, 0};
    DAS::Core::IPC::ObjectId valid_id = {1, 1, 1};

    EXPECT_TRUE(DAS::Core::IPC::IsNullObjectId(null_id));
    EXPECT_FALSE(DAS::Core::IPC::IsNullObjectId(valid_id));
}

TEST(IpcMultiProcessTestBasic, ObjectIdEncodeDecodeRoundTrip)
{
    for (uint32_t i = 0; i < 100; ++i)
    {
        DAS::Core::IPC::ObjectId original = {
            static_cast<uint16_t>(i % 65534 + 1),
            static_cast<uint16_t>(i % 65535),
            i * 1000};

        uint64_t encoded = DAS::Core::IPC::EncodeObjectId(original);
        DAS::Core::IPC::ObjectId decoded =
            DAS::Core::IPC::DecodeObjectId(encoded);

        EXPECT_EQ(decoded, original);
    }
}

// ====== RemoteObjectRegistry 测试 ======

TEST(IpcMultiProcessTestBasic, RemoteObjectRegistry_RegisterAndLookup)
{
    DAS::Core::IPC::RemoteObjectRegistry& registry =
        DAS::Core::IPC::RemoteObjectRegistry::GetInstance();

    DAS::Core::IPC::ObjectId obj_id = {2, 1, 100};
    DasGuid                  iid = {
        0x12345678,
        0x1234,
        0x5678,
        {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}};
    std::string obj_name = "TestRemoteObject";

    DasResult result = registry.RegisterObject(obj_id, iid, 2, obj_name, 1);
    ASSERT_EQ(result, DAS_S_OK);

    DAS::Core::IPC::RemoteObjectInfo info;
    result = registry.LookupByName(obj_name, info);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_EQ(info.name, obj_name);
    EXPECT_EQ(info.session_id, 2u);

    // 清理
    registry.UnregisterObject(obj_id);
}

TEST(IpcMultiProcessTestBasic, RemoteObjectRegistry_MultipleObjects)
{
    DAS::Core::IPC::RemoteObjectRegistry& registry =
        DAS::Core::IPC::RemoteObjectRegistry::GetInstance();

    std::vector<std::string> obj_names = {"Object1", "Object2", "Object3"};
    std::vector<DAS::Core::IPC::ObjectId> obj_ids;

    for (size_t i = 0; i < obj_names.size(); ++i)
    {
        DAS::Core::IPC::ObjectId obj_id = {
            2,
            1,
            static_cast<uint32_t>(100 + i)};
        DasGuid iid = {
            static_cast<uint32_t>(0x1000 + i),
            0x1234,
            0x5678,
            {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}};

        ASSERT_EQ(
            registry.RegisterObject(obj_id, iid, 2, obj_names[i], 1),
            DAS_S_OK);
        obj_ids.push_back(obj_id);
    }

    // 验证所有对象
    for (size_t i = 0; i < obj_names.size(); ++i)
    {
        DAS::Core::IPC::RemoteObjectInfo info;
        ASSERT_EQ(registry.LookupByName(obj_names[i], info), DAS_S_OK);
        EXPECT_EQ(info.name, obj_names[i]);
    }

    // 清理
    for (const auto& id : obj_ids)
    {
        registry.UnregisterObject(id);
    }
}

TEST(IpcMultiProcessTestBasic, RemoteObjectRegistry_SessionCleanup)
{
    DAS::Core::IPC::RemoteObjectRegistry& registry =
        DAS::Core::IPC::RemoteObjectRegistry::GetInstance();

    uint16_t                              session_id = 100;
    std::vector<DAS::Core::IPC::ObjectId> obj_ids;

    // 注册多个对象
    for (int i = 0; i < 5; ++i)
    {
        DAS::Core::IPC::ObjectId obj_id = {
            session_id,
            1,
            static_cast<uint32_t>(100 + i)};
        DasGuid iid = {
            static_cast<uint32_t>(0x2000 + i),
            0x1234,
            0x5678,
            {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}};
        std::string name = "SessionObject" + std::to_string(i);

        registry.RegisterObject(obj_id, iid, session_id, name, 1);
        obj_ids.push_back(obj_id);
    }

    // 验证对象存在
    std::vector<DAS::Core::IPC::RemoteObjectInfo> objects;
    registry.ListAllObjects(objects);
    size_t count_before = objects.size();

    // 清理会话所有对象
    registry.UnregisterAllFromSession(session_id);

    // 验证对象已清理
    objects.clear();
    registry.ListAllObjects(objects);
    EXPECT_LT(objects.size(), count_before);
}

TEST(IpcMultiProcessTestBasic, RemoteObjectRegistry_LookupNonExistent)
{
    DAS::Core::IPC::RemoteObjectRegistry& registry =
        DAS::Core::IPC::RemoteObjectRegistry::GetInstance();

    DAS::Core::IPC::RemoteObjectInfo info;
    DasResult result = registry.LookupByName("NonExistentObject", info);
    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);
}

// ====== 并发测试 ======

TEST(IpcMultiProcessTestBasic, ConcurrentSessionAllocation)
{
    const int                num_threads = 10;
    std::vector<std::thread> threads;
    std::vector<uint16_t>    session_ids(num_threads);
    std::atomic<int>         success_count{0};
    std::mutex               mutex;

    for (int i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(
            [&mutex, &session_ids, &success_count, i]()
            {
                DAS::Core::IPC::SessionCoordinator& coordinator =
                    DAS::Core::IPC::SessionCoordinator::GetInstance();
                uint16_t session_id = coordinator.AllocateSessionId();
                if (session_id != 0
                    && DAS::Core::IPC::SessionCoordinator::IsValidSessionId(
                        session_id))
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    session_ids[i] = session_id;
                    success_count++;
                }
            });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    // 验证所有线程都成功分配了 session ID
    EXPECT_EQ(success_count.load(), num_threads);

    // 验证所有 session ID 都不同
    std::unordered_set<uint16_t> unique_ids;
    for (uint16_t id : session_ids)
    {
        unique_ids.insert(id);
    }
    EXPECT_EQ(unique_ids.size(), static_cast<size_t>(num_threads));

    // 释放所有 session ID
    DAS::Core::IPC::SessionCoordinator& coordinator =
        DAS::Core::IPC::SessionCoordinator::GetInstance();
    for (uint16_t id : session_ids)
    {
        if (id != 0)
        {
            coordinator.ReleaseSessionId(id);
        }
    }
}

// ====== 握手协议结构测试 ======

TEST(IpcMultiProcessTestBasic, Handshake_HelloRequestInit)
{
    DAS::Core::IPC::HelloRequestV1 hello;
    DAS::Core::IPC::InitHelloRequest(hello, 12345, "TestPlugin");

    EXPECT_EQ(
        hello.protocol_version,
        DAS::Core::IPC::HelloRequestV1::CURRENT_PROTOCOL_VERSION);
    EXPECT_EQ(hello.pid, 12345u);
    EXPECT_STREQ(hello.plugin_name, "TestPlugin");
}

TEST(IpcMultiProcessTestBasic, Handshake_WelcomeResponseInit)
{
    DAS::Core::IPC::WelcomeResponseV1 welcome;
    DAS::Core::IPC::InitWelcomeResponse(
        welcome,
        42,
        DAS::Core::IPC::WelcomeResponseV1::STATUS_SUCCESS);

    EXPECT_EQ(welcome.session_id, 42u);
    EXPECT_EQ(
        welcome.status,
        DAS::Core::IPC::WelcomeResponseV1::STATUS_SUCCESS);
}

TEST(IpcMultiProcessTestBasic, Handshake_ReadyRequestInit)
{
    DAS::Core::IPC::ReadyRequestV1 ready;
    DAS::Core::IPC::InitReadyRequest(ready, 42);

    EXPECT_EQ(ready.session_id, 42u);
}

TEST(IpcMultiProcessTestBasic, Handshake_ReadyAckInit)
{
    DAS::Core::IPC::ReadyAckV1 ack;
    DAS::Core::IPC::InitReadyAck(
        ack,
        DAS::Core::IPC::ReadyAckV1::STATUS_SUCCESS);

    EXPECT_EQ(ack.status, DAS::Core::IPC::ReadyAckV1::STATUS_SUCCESS);
}

// ====== 消息队列名称生成测试 ======

TEST(IpcMultiProcessTestBasic, MessageQueueNameGeneration)
{
    std::string m2p_name =
        DAS::Core::IPC::Host::MakeMessageQueueName(12345, true);
    std::string h2m_name =
        DAS::Core::IPC::Host::MakeMessageQueueName(12345, false);

    EXPECT_TRUE(m2p_name.find("DAS_Host_12345_MQ_M2P") != std::string::npos);
    EXPECT_TRUE(h2m_name.find("DAS_Host_12345_MQ_H2M") != std::string::npos);
    EXPECT_NE(m2p_name, h2m_name);
}

TEST(IpcMultiProcessTestBasic, SharedMemoryNameGeneration)
{
    std::string shm_name = DAS::Core::IPC::Host::MakeSharedMemoryName(12345);

    EXPECT_TRUE(shm_name.find("DAS_Host_12345_SHM") != std::string::npos);
}
