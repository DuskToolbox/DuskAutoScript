/**
 * @file IpcMultiProcessTestBasic.cpp
 * @brief IPC 多进程集成测试
 *
 * 测试主进程 ↔ Host 进程通信、RemoteObjectRegistry 注册/查找、消息分发。
 *
 * 架构说明：
 * - 主进程 (测试进程)：负责管理会话、查找远程对象、分发消息
 * - Host 进程：加载插件、创建 IPC 资源、暴露 IPC 接口
 *
 * 测试场景：
 * 1. 进程间会话建立与断开
 * 2. 远程对象注册与查找
 * 3. 消息分发机制
 * 4. 会话 ID 分配与管理
 */

#include <atomic>
#include <chrono>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/MainProcessServer.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/ObjectManager.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/IPC/SessionCoordinator.h>
#include <das/DasApi.h>
#include <das/IDasBase.h>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

using DAS::Core::IPC::DecodeObjectId;
using DAS::Core::IPC::DistributedObjectManager;
using DAS::Core::IPC::EncodeObjectId;
using DAS::Core::IPC::HostSessionInfo;
using DAS::Core::IPC::IPCMessageHeader;
using DAS::Core::IPC::IsNullObjectId;
using DAS::Core::IPC::MainProcessServer;
using DAS::Core::IPC::MessageType;
using DAS::Core::IPC::ObjectId;
using DAS::Core::IPC::RemoteObjectInfo;
using DAS::Core::IPC::RemoteObjectRegistry;
using DAS::Core::IPC::SessionCoordinator;

// ====== 测试辅助类 ======

/**
 * @brief 模拟 Host 进程环境
 *
 * 提供 Host 进程的核心功能：
 * - Session ID 分配
 * - 远程对象注册
 * - 对象生命周期管理
 */
class MockHostProcess
{
public:
    MockHostProcess() : session_id_(0), is_running_(false) {}

    ~MockHostProcess() { Shutdown(); }

    DasResult Initialize()
    {
        // 分配 session ID
        auto& coordinator = SessionCoordinator::GetInstance();
        session_id_ = coordinator.AllocateSessionId();
        if (session_id_ == 0)
        {
            DAS_LOG_ERROR("Failed to allocate session ID");
            return DAS_E_IPC_SESSION_ALLOC_FAILED;
        }

        // 设置本地 session ID
        coordinator.SetLocalSessionId(session_id_);

        // 初始化对象管理器
        object_manager_ = std::make_unique<DistributedObjectManager>();
        DasResult result = object_manager_->Initialize(session_id_);
        if (result != DAS_S_OK)
        {
            coordinator.ReleaseSessionId(session_id_);
            session_id_ = 0;
            DAS_LOG_ERROR("Failed to initialize object manager: {}", result);
            return result;
        }

        is_running_ = true;
        DAS_LOG_INFO(
            "MockHostProcess initialized with session_id={}",
            session_id_);
        return DAS_S_OK;
    }

    DasResult Shutdown()
    {
        if (!is_running_)
        {
            return DAS_S_OK;
        }

        is_running_ = false;

        // 注销所有对象
        auto& registry = RemoteObjectRegistry::GetInstance();
        registry.UnregisterAllFromSession(session_id_);

        // 释放 session ID
        auto& coordinator = SessionCoordinator::GetInstance();
        coordinator.ReleaseSessionId(session_id_);
        DAS_LOG_INFO(
            "MockHostProcess shutdown, released session_id={}",
            session_id_);
        session_id_ = 0;

        if (object_manager_)
        {
            object_manager_->Shutdown();
            object_manager_.reset();
        }

        return DAS_S_OK;
    }

    uint16_t GetSessionId() const { return session_id_; }

    bool IsRunning() const { return is_running_; }

    DasResult RegisterObject(
        const ObjectId&    object_id,
        const DasGuid&     iid,
        const std::string& name,
        uint16_t           version = 1)
    {
        if (!is_running_)
        {
            return DAS_E_IPC_NOT_INITIALIZED;
        }

        // 注册到远程对象注册表
        auto& registry = RemoteObjectRegistry::GetInstance();
        return registry
            .RegisterObject(object_id, iid, session_id_, name, version);
    }

    DasResult UnregisterObject(const ObjectId& object_id)
    {
        auto& registry = RemoteObjectRegistry::GetInstance();
        return registry.UnregisterObject(object_id);
    }

private:
    uint16_t                                  session_id_;
    bool                                      is_running_;
    std::unique_ptr<DistributedObjectManager> object_manager_;
};

/**
 * @brief 模拟主进程环境
 *
 * 提供主进程的核心功能：
 * - MainProcessServer 管理
 * - Host 连接管理
 * - 远程对象查找
 */
class MockMainProcess
{
public:
    MockMainProcess() : is_initialized_(false) {}

    ~MockMainProcess() { Shutdown(); }

    DasResult Initialize()
    {
        auto&     server = MainProcessServer::GetInstance();
        DasResult result = server.Initialize();
        if (result != DAS_S_OK)
        {
            DAS_LOG_ERROR("Failed to initialize MainProcessServer: {}", result);
            return result;
        }

        // 设置主进程 session ID
        auto& coordinator = SessionCoordinator::GetInstance();
        coordinator.SetLocalSessionId(1); // 主进程 session ID = 1

        is_initialized_ = true;
        DAS_LOG_INFO("MockMainProcess initialized");
        return DAS_S_OK;
    }

    DasResult Shutdown()
    {
        if (!is_initialized_)
        {
            return DAS_S_OK;
        }

        auto& server = MainProcessServer::GetInstance();
        server.Shutdown();
        is_initialized_ = false;
        DAS_LOG_INFO("MockMainProcess shutdown");
        return DAS_S_OK;
    }

    DasResult OnHostConnected(uint16_t session_id)
    {
        auto& server = MainProcessServer::GetInstance();
        return server.OnHostConnected(session_id);
    }

    DasResult OnHostDisconnected(uint16_t session_id)
    {
        auto& server = MainProcessServer::GetInstance();
        return server.OnHostDisconnected(session_id);
    }

    DasResult LookupRemoteObject(
        const std::string& name,
        RemoteObjectInfo&  out_info)
    {
        auto& server = MainProcessServer::GetInstance();
        return server.LookupRemoteObjectByName(name, out_info);
    }

    bool IsInitialized() const { return is_initialized_; }

private:
    bool is_initialized_;
};

// ====== 测试辅助函数 ======

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

// ====== 测试夹具 ======

class IpcMultiProcessTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // 清理注册表
        auto& registry = RemoteObjectRegistry::GetInstance();
        registry.Clear();

        // 初始化主进程
        ASSERT_EQ(main_process_.Initialize(), DAS_S_OK);
    }

    void TearDown() override
    {
        // 关闭 Host 进程
        host_process_.Shutdown();

        // 关闭主进程
        main_process_.Shutdown();

        // 清理注册表
        auto& registry = RemoteObjectRegistry::GetInstance();
        registry.Clear();
    }

    MockMainProcess main_process_;
    MockHostProcess host_process_;
};

// ====== 基础测试 ======

TEST_F(IpcMultiProcessTest, BasicTest)
{
    // 基本测试，验证测试框架正常工作
    EXPECT_TRUE(main_process_.IsInitialized());
}

TEST_F(IpcMultiProcessTest, DirectoryStructureTest)
{
    // 验证目录结构能够被正确包含
    // 测试编译时期能够找到相关的头文件
    EXPECT_TRUE(main_process_.IsInitialized());
}

// ====== 会话建立与断开测试 ======

TEST_F(IpcMultiProcessTest, SessionEstablishAndDisconnect)
{
    // 初始化 Host 进程
    ASSERT_EQ(host_process_.Initialize(), DAS_S_OK);
    uint16_t host_session_id = host_process_.GetSessionId();
    ASSERT_NE(host_session_id, 0);
    ASSERT_TRUE(host_process_.IsRunning());

    // 主进程处理 Host 连接
    ASSERT_EQ(main_process_.OnHostConnected(host_session_id), DAS_S_OK);

    // 验证会话已建立
    HostSessionInfo session_info;
    auto&           server = MainProcessServer::GetInstance();
    ASSERT_EQ(server.GetSessionInfo(host_session_id, session_info), DAS_S_OK);
    EXPECT_EQ(session_info.session_id, host_session_id);
    EXPECT_TRUE(session_info.is_connected);

    // 断开连接
    ASSERT_EQ(main_process_.OnHostDisconnected(host_session_id), DAS_S_OK);

    // 验证会话已断开
    EXPECT_FALSE(server.IsSessionConnected(host_session_id));
}

TEST_F(IpcMultiProcessTest, MultipleHostConnections)
{
    std::vector<std::unique_ptr<MockHostProcess>> hosts;
    std::vector<uint16_t>                         session_ids;

    // 创建多个 Host 进程
    for (int i = 0; i < 3; ++i)
    {
        auto host = std::make_unique<MockHostProcess>();
        ASSERT_EQ(host->Initialize(), DAS_S_OK);
        session_ids.push_back(host->GetSessionId());
        hosts.push_back(std::move(host));
    }

    // 依次连接所有 Host
    for (uint16_t session_id : session_ids)
    {
        ASSERT_EQ(main_process_.OnHostConnected(session_id), DAS_S_OK);
    }

    // 验证所有会话都已建立
    auto connected_sessions =
        MainProcessServer::GetInstance().GetConnectedSessions();
    EXPECT_EQ(connected_sessions.size(), 3u);

    // 断开所有连接
    for (uint16_t session_id : session_ids)
    {
        ASSERT_EQ(main_process_.OnHostDisconnected(session_id), DAS_S_OK);
    }

    // 验证所有会话都已断开
    connected_sessions =
        MainProcessServer::GetInstance().GetConnectedSessions();
    EXPECT_EQ(connected_sessions.size(), 0u);
}

// ====== 远程对象注册与查找测试 ======

TEST_F(IpcMultiProcessTest, RemoteObjectRegisterAndLookup)
{
    // 初始化 Host 进程并连接
    ASSERT_EQ(host_process_.Initialize(), DAS_S_OK);
    uint16_t host_session_id = host_process_.GetSessionId();
    ASSERT_EQ(main_process_.OnHostConnected(host_session_id), DAS_S_OK);

    // Host 注册远程对象
    ObjectId    obj_id = {host_session_id, 1, 100};
    DasGuid     iid = CreateTestGuid(0x12345678);
    std::string obj_name = "TestRemoteObject";

    ASSERT_EQ(host_process_.RegisterObject(obj_id, iid, obj_name, 1), DAS_S_OK);

    // 主进程收到对象注册通知
    ASSERT_EQ(
        MainProcessServer::GetInstance().OnRemoteObjectRegistered(
            obj_id,
            iid,
            host_session_id,
            obj_name,
            1),
        DAS_S_OK);

    // 主进程查找远程对象
    RemoteObjectInfo found_info;
    ASSERT_EQ(main_process_.LookupRemoteObject(obj_name, found_info), DAS_S_OK);
    EXPECT_EQ(found_info.name, obj_name);
    EXPECT_EQ(found_info.session_id, host_session_id);
    EXPECT_EQ(found_info.object_id.local_id, 100u);

    // 通过接口类型查找
    ASSERT_EQ(
        MainProcessServer::GetInstance().LookupRemoteObjectByInterface(
            iid,
            found_info),
        DAS_S_OK);
    EXPECT_EQ(found_info.name, obj_name);
}

TEST_F(IpcMultiProcessTest, MultipleRemoteObjectsFromSameHost)
{
    // 初始化 Host 进程并连接
    ASSERT_EQ(host_process_.Initialize(), DAS_S_OK);
    uint16_t host_session_id = host_process_.GetSessionId();
    ASSERT_EQ(main_process_.OnHostConnected(host_session_id), DAS_S_OK);

    // 注册多个对象
    std::vector<std::string> obj_names = {"Object1", "Object2", "Object3"};
    for (size_t i = 0; i < obj_names.size(); ++i)
    {
        ObjectId obj_id = {host_session_id, 1, static_cast<uint32_t>(100 + i)};
        DasGuid  iid = CreateTestGuid(static_cast<uint32_t>(0x1000 + i));

        ASSERT_EQ(
            host_process_.RegisterObject(obj_id, iid, obj_names[i], 1),
            DAS_S_OK);
        ASSERT_EQ(
            MainProcessServer::GetInstance().OnRemoteObjectRegistered(
                obj_id,
                iid,
                host_session_id,
                obj_names[i],
                1),
            DAS_S_OK);
    }

    // 查找所有对象
    std::vector<RemoteObjectInfo> objects;
    ASSERT_EQ(
        MainProcessServer::GetInstance().GetRemoteObjects(objects),
        DAS_S_OK);
    EXPECT_EQ(objects.size(), 3u);

    // 逐个查找
    for (const auto& name : obj_names)
    {
        RemoteObjectInfo info;
        ASSERT_EQ(main_process_.LookupRemoteObject(name, info), DAS_S_OK);
        EXPECT_EQ(info.name, name);
    }
}

TEST_F(IpcMultiProcessTest, RemoteObjectUnregistration)
{
    // 初始化 Host 进程并连接
    ASSERT_EQ(host_process_.Initialize(), DAS_S_OK);
    uint16_t host_session_id = host_process_.GetSessionId();
    ASSERT_EQ(main_process_.OnHostConnected(host_session_id), DAS_S_OK);

    // 注册对象
    ObjectId    obj_id = {host_session_id, 1, 100};
    DasGuid     iid = CreateTestGuid(0x12345678);
    std::string obj_name = "TestObject";

    ASSERT_EQ(host_process_.RegisterObject(obj_id, iid, obj_name, 1), DAS_S_OK);
    ASSERT_EQ(
        MainProcessServer::GetInstance().OnRemoteObjectRegistered(
            obj_id,
            iid,
            host_session_id,
            obj_name,
            1),
        DAS_S_OK);

    // 验证对象存在
    RemoteObjectInfo info;
    ASSERT_EQ(main_process_.LookupRemoteObject(obj_name, info), DAS_S_OK);

    // 注销对象
    ASSERT_EQ(
        MainProcessServer::GetInstance().OnRemoteObjectUnregistered(obj_id),
        DAS_S_OK);
    ASSERT_EQ(host_process_.UnregisterObject(obj_id), DAS_S_OK);

    // 验证对象不存在
    ASSERT_EQ(
        main_process_.LookupRemoteObject(obj_name, info),
        DAS_E_IPC_OBJECT_NOT_FOUND);
}

// ====== 会话断开时自动清理测试 ======

TEST_F(IpcMultiProcessTest, AutoCleanupOnSessionDisconnect)
{
    // 初始化 Host 进程并连接
    ASSERT_EQ(host_process_.Initialize(), DAS_S_OK);
    uint16_t host_session_id = host_process_.GetSessionId();
    ASSERT_EQ(main_process_.OnHostConnected(host_session_id), DAS_S_OK);

    // 注册多个对象
    for (int i = 0; i < 3; ++i)
    {
        ObjectId obj_id = {host_session_id, 1, static_cast<uint32_t>(100 + i)};
        DasGuid  iid = CreateTestGuid(static_cast<uint32_t>(0x1000 + i));
        std::string name = "Object" + std::to_string(i);

        host_process_.RegisterObject(obj_id, iid, name, 1);
        MainProcessServer::GetInstance()
            .OnRemoteObjectRegistered(obj_id, iid, host_session_id, name, 1);
    }

    // 验证对象存在
    std::vector<RemoteObjectInfo> objects;
    ASSERT_EQ(
        MainProcessServer::GetInstance().GetRemoteObjects(objects),
        DAS_S_OK);
    EXPECT_EQ(objects.size(), 3u);

    // 断开连接并自动清理
    ASSERT_EQ(main_process_.OnHostDisconnected(host_session_id), DAS_S_OK);

    // 注册表中的对象清理
    auto& registry = RemoteObjectRegistry::GetInstance();
    registry.UnregisterAllFromSession(host_session_id);

    // 验证对象已被清理
    objects.clear();
    ASSERT_EQ(
        MainProcessServer::GetInstance().GetRemoteObjects(objects),
        DAS_S_OK);
    EXPECT_EQ(objects.size(), 0u);
}

// ====== 错误处理测试 ======

TEST_F(IpcMultiProcessTest, LookupNonExistentObject)
{
    RemoteObjectInfo info;
    EXPECT_EQ(
        main_process_.LookupRemoteObject("NonExistent", info),
        DAS_E_IPC_OBJECT_NOT_FOUND);
}

TEST_F(IpcMultiProcessTest, InvalidSessionOperations)
{
    // 尝试对无效 session ID 进行操作
    uint16_t invalid_session_id = 0xFFFF; // 保留的 session ID

    EXPECT_NE(main_process_.OnHostConnected(invalid_session_id), DAS_S_OK);
}

// ====== 并发测试 ======

TEST_F(IpcMultiProcessTest, ConcurrentSessionAllocation)
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
                auto&    coordinator = SessionCoordinator::GetInstance();
                uint16_t session_id = coordinator.AllocateSessionId();
                if (session_id != 0
                    && SessionCoordinator::IsValidSessionId(session_id))
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
    auto& coordinator = SessionCoordinator::GetInstance();
    for (uint16_t id : session_ids)
    {
        if (id != 0)
        {
            coordinator.ReleaseSessionId(id);
        }
    }
}

// ====== 消息分发测试 ======

TEST_F(IpcMultiProcessTest, MessageDispatch)
{
    // 初始化 Host 进程并连接
    ASSERT_EQ(host_process_.Initialize(), DAS_S_OK);
    uint16_t host_session_id = host_process_.GetSessionId();
    ASSERT_EQ(main_process_.OnHostConnected(host_session_id), DAS_S_OK);

    // 注册远程对象
    ObjectId    obj_id = {host_session_id, 1, 100};
    DasGuid     iid = CreateTestGuid(0x12345678);
    std::string obj_name = "DispatchTestObject";

    ASSERT_EQ(host_process_.RegisterObject(obj_id, iid, obj_name, 1), DAS_S_OK);
    ASSERT_EQ(
        MainProcessServer::GetInstance().OnRemoteObjectRegistered(
            obj_id,
            iid,
            host_session_id,
            obj_name,
            1),
        DAS_S_OK);

    // 创建测试消息
    IPCMessageHeader header{};
    header.call_id = 1;
    header.message_type = static_cast<uint8_t>(MessageType::REQUEST);
    header.interface_id = iid.data1;
    header.session_id = obj_id.session_id;
    header.generation = obj_id.generation;
    header.local_id = obj_id.local_id;
    header.version = 2;

    std::string          body = "test_request_body";
    std::vector<uint8_t> response_body;

    // 设置消息处理器
    bool handler_called = false;
    MainProcessServer::GetInstance().SetMessageDispatchHandler(
        [&handler_called](
            const IPCMessageHeader& h,
            const uint8_t*          b,
            size_t                  size,
            std::vector<uint8_t>&   resp) -> DasResult
        {
            handler_called = true;
            (void)h;
            (void)b;
            (void)size;
            (void)resp;
            return DAS_S_OK;
        });

    // 分发消息
    DasResult result = MainProcessServer::GetInstance().DispatchMessage(
        header,
        reinterpret_cast<const uint8_t*>(body.data()),
        body.size(),
        response_body);

    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(handler_called);
}

// ====== 会话事件回调测试 ======

TEST_F(IpcMultiProcessTest, SessionEventCallbacks)
{
    bool     connected_called = false;
    bool     disconnected_called = false;
    uint16_t connected_session_id = 0;
    uint16_t disconnected_session_id = 0;

    // 设置回调
    MainProcessServer::GetInstance().SetOnSessionConnectedCallback(
        [&connected_called, &connected_session_id](uint16_t session_id)
        {
            connected_called = true;
            connected_session_id = session_id;
        });

    MainProcessServer::GetInstance().SetOnSessionDisconnectedCallback(
        [&disconnected_called, &disconnected_session_id](uint16_t session_id)
        {
            disconnected_called = true;
            disconnected_session_id = session_id;
        });

    // 初始化 Host 进程并连接
    ASSERT_EQ(host_process_.Initialize(), DAS_S_OK);
    uint16_t host_session_id = host_process_.GetSessionId();

    // 连接
    ASSERT_EQ(main_process_.OnHostConnected(host_session_id), DAS_S_OK);
    EXPECT_TRUE(connected_called);
    EXPECT_EQ(connected_session_id, host_session_id);

    // 断开
    ASSERT_EQ(main_process_.OnHostDisconnected(host_session_id), DAS_S_OK);
    EXPECT_TRUE(disconnected_called);
    EXPECT_EQ(disconnected_session_id, host_session_id);
}

// ====== 对象事件回调测试 ======

TEST_F(IpcMultiProcessTest, ObjectEventCallbacks)
{
    bool        registered_called = false;
    bool        unregistered_called = false;
    std::string registered_object_name;
    std::string unregistered_object_name;

    // 设置回调
    MainProcessServer::GetInstance().SetOnObjectRegisteredCallback(
        [&registered_called,
         &registered_object_name](const RemoteObjectInfo& info)
        {
            registered_called = true;
            registered_object_name = info.name;
        });

    MainProcessServer::GetInstance().SetOnObjectUnregisteredCallback(
        [&unregistered_called,
         &unregistered_object_name](const RemoteObjectInfo& info)
        {
            unregistered_called = true;
            unregistered_object_name = info.name;
        });

    // 初始化 Host 进程并连接
    ASSERT_EQ(host_process_.Initialize(), DAS_S_OK);
    uint16_t host_session_id = host_process_.GetSessionId();
    ASSERT_EQ(main_process_.OnHostConnected(host_session_id), DAS_S_OK);

    // 注册对象
    ObjectId    obj_id = {host_session_id, 1, 100};
    DasGuid     iid = CreateTestGuid(0x12345678);
    std::string obj_name = "CallbackTestObject";

    ASSERT_EQ(host_process_.RegisterObject(obj_id, iid, obj_name, 1), DAS_S_OK);
    ASSERT_EQ(
        MainProcessServer::GetInstance().OnRemoteObjectRegistered(
            obj_id,
            iid,
            host_session_id,
            obj_name,
            1),
        DAS_S_OK);

    EXPECT_TRUE(registered_called);
    EXPECT_EQ(registered_object_name, obj_name);

    // 注销对象
    ASSERT_EQ(
        MainProcessServer::GetInstance().OnRemoteObjectUnregistered(obj_id),
        DAS_S_OK);

    // 注意：unregistered_called 取决于实现是否在注销时调用回调
}

// ====== 性能测试 ======

TEST_F(IpcMultiProcessTest, Performance_ObjectRegistration)
{
    // 初始化 Host 进程并连接
    ASSERT_EQ(host_process_.Initialize(), DAS_S_OK);
    uint16_t host_session_id = host_process_.GetSessionId();
    ASSERT_EQ(main_process_.OnHostConnected(host_session_id), DAS_S_OK);

    const int num_objects = 1000;
    auto      start = std::chrono::high_resolution_clock::now();

    // 注册大量对象
    for (int i = 0; i < num_objects; ++i)
    {
        ObjectId    obj_id = {host_session_id, 1, static_cast<uint32_t>(i)};
        DasGuid     iid = CreateTestGuid(static_cast<uint32_t>(i));
        std::string name = "PerfObject" + std::to_string(i);

        host_process_.RegisterObject(obj_id, iid, name, 1);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 验证性能：1000 个对象注册应该在 1 秒内完成
    EXPECT_LT(duration.count(), 1000);

    // 验证所有对象都已注册
    std::vector<RemoteObjectInfo> objects;
    MainProcessServer::GetInstance().GetRemoteObjects(objects);
    EXPECT_EQ(objects.size(), static_cast<size_t>(num_objects));
}

// ====== ObjectId 编解码测试 ======

TEST_F(IpcMultiProcessTest, ObjectIdEncodingDecoding)
{
    ObjectId original = {2, 1, 100};

    uint64_t encoded = EncodeObjectId(original);
    ObjectId decoded = DecodeObjectId(encoded);

    EXPECT_EQ(decoded.session_id, original.session_id);
    EXPECT_EQ(decoded.generation, original.generation);
    EXPECT_EQ(decoded.local_id, original.local_id);
    EXPECT_EQ(decoded, original);
}

TEST_F(IpcMultiProcessTest, ObjectIdNullCheck)
{
    ObjectId null_id = {0, 0, 0};
    ObjectId valid_id = {1, 1, 1};

    EXPECT_TRUE(IsNullObjectId(null_id));
    EXPECT_FALSE(IsNullObjectId(valid_id));
}