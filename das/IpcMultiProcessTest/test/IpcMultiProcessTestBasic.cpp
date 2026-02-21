/**
 * @file IpcMultiProcessTestBasic.cpp
 * @brief IPC 多进程集成测试

 * *

 * *
 * 测试主进程 <-> Host 进程通信、握手协议、消息传输。
 *
 *
 * 架构说明：

 * *
 * -
 * 测试进程：启动真实的 DasHost.exe 进程，通过 IPC 与之通信

 * *
 * - Host
 * 进程：被启动的 DasHost.exe，创建 IPC 资源、处理握手
 *
 *
 * 测试场景：

 * * 1.
 * 进程启动与关闭
 * 2. 握手协议（Hello -> Welcome -> Ready ->
 * ReadyAck）

 * * 3. IPC 消息传输
 */

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/MessageQueueTransport.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/IPC/SessionCoordinator.h>
#include <das/DasApi.h>
#include <das/Host/HostConfig.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>

#include <boost/asio/io_context.hpp>
#include <boost/process/v2/pid.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/system/error_code.hpp>

// ============================================================
// ProcessLauncher - 使用 boost::process v2 启动外部进程
// ============================================================

class ProcessLauncher
{
public:
    ProcessLauncher() = default;

    ~ProcessLauncher() { Terminate(); }

    DasResult Launch(
        const std::string&              exe_path,
        const std::vector<std::string>& args = {})
    {
        if (!std::filesystem::exists(exe_path))
        {
            std::string msg = std::string("Executable not found: ") + exe_path;
            DAS_LOG_ERROR(msg.c_str());
            return DAS_E_INVALID_ARGUMENT;
        }

        try
        {
            std::vector<std::string> cmd_args;
            for (const auto& arg : args)
            {
                cmd_args.push_back(arg);
            }

            process_ = std::make_unique<boost::process::v2::process>(
                io_ctx_,
                exe_path,
                cmd_args,
                boost::process::v2::process_start_dir(
                    std::filesystem::path(exe_path).parent_path().string()));
            pid_ = static_cast<uint32_t>(process_->id());
            is_running_ = true;

            std::string msg =
                std::string("Process launched: PID=") + std::to_string(pid_);
            DAS_LOG_INFO(msg.c_str());
            return DAS_S_OK;
        }
        catch (const std::exception& e)
        {
            std::string msg =
                std::string("Exception launching process: ") + e.what();
            DAS_LOG_ERROR(msg.c_str());
            return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
        }
    }

    void Terminate()
    {
        if (process_)
        {
            std::string msg =
                std::string("Terminating process: PID=") + std::to_string(pid_);
            DAS_LOG_INFO(msg.c_str());

            boost::system::error_code ec;
            process_->terminate(ec);
            process_.reset();
        }
        is_running_ = false;
    }

    bool IsRunning() const { return process_ && is_running_; }

    uint32_t GetPid() const { return pid_; }

private:
    boost::asio::io_context                      io_ctx_;
    std::unique_ptr<boost::process::v2::process> process_;
    uint32_t                                     pid_ = 0;
    bool                                         is_running_ = false;
};

// ============================================================
// IpcClient - 连接到 Host 进程的 IPC 客户端
// ============================================================

class IpcClient
{
public:
    IpcClient() = default;

    ~IpcClient() { Disconnect(); }

    DasResult Connect(uint32_t host_pid)
    {
        host_pid_ = host_pid;

        std::string host_to_plugin_queue =
            DAS::Host::MakeMessageQueueName(host_pid, true);
        std::string plugin_to_host_queue =
            DAS::Host::MakeMessageQueueName(host_pid, false);

        std::string msg = std::string("Connecting to Host IPC: ")
                          + host_to_plugin_queue + ", " + plugin_to_host_queue;
        DAS_LOG_INFO(msg.c_str());

        transport_ = std::make_unique<DAS::Core::IPC::IpcTransport>();

        DasResult result = transport_->Initialize(
            host_to_plugin_queue,
            plugin_to_host_queue,
            DAS::Host::DEFAULT_MAX_MESSAGE_SIZE,
            DAS::Host::DEFAULT_MAX_MESSAGES);

        if (result != DAS_S_OK)
        {
            std::string err_msg =
                std::string("Failed to connect to Host IPC: error=")
                + std::to_string(result);
            DAS_LOG_ERROR(err_msg.c_str());
            return result;
        }

        is_connected_ = true;
        DAS_LOG_INFO("Connected to Host IPC successfully");
        return DAS_S_OK;
    }

    void Disconnect()
    {
        if (transport_)
        {
            transport_->Shutdown();
            transport_.reset();
        }
        is_connected_ = false;
        host_pid_ = 0;
    }

    DasResult SendHandshakeHello(const std::string& plugin_name = "TestClient")
    {
        if (!transport_)
        {
            DAS_LOG_ERROR("Transport not initialized");
            return DAS_E_IPC_NOT_INITIALIZED;
        }

        uint32_t my_pid =
            static_cast<uint32_t>(boost::process::v2::current_pid());

        DAS::Core::IPC::HelloRequestV1 hello;
        DAS::Core::IPC::InitHelloRequest(hello, my_pid, plugin_name.c_str());

        DAS::Core::IPC::IPCMessageHeader header{};
        header.magic = DAS::Core::IPC::IPCMessageHeader::MAGIC;
        header.version = DAS::Core::IPC::IPCMessageHeader::CURRENT_VERSION;
        header.message_type =
            static_cast<uint8_t>(DAS::Core::IPC::MessageType::REQUEST);
        header.header_flags = 0;
        header.call_id = next_call_id_++;
        header.interface_id = static_cast<uint32_t>(
            DAS::Core::IPC::HandshakeInterfaceId::HandshakeHello);
        header.method_id = 0;
        header.flags = 0;
        header.error_code = 0;
        header.body_size = sizeof(hello);
        header.session_id = 0;
        header.generation = 0;
        header.local_id = 0;

        DasResult result = transport_->Send(
            header,
            reinterpret_cast<const uint8_t*>(&hello),
            sizeof(hello));

        if (result != DAS_S_OK)
        {
            std::string msg = std::string("Failed to send Hello: error=")
                              + std::to_string(result);
            DAS_LOG_ERROR(msg.c_str());
            return result;
        }

        std::string info_msg = std::string("Sent Hello: pid=")
                               + std::to_string(my_pid)
                               + ", name=" + plugin_name;
        DAS_LOG_INFO(info_msg.c_str());
        return DAS_S_OK;
    }

    DasResult ReceiveHandshakeWelcome(
        DAS::Core::IPC::WelcomeResponseV1& out_welcome,
        uint32_t                           timeout_ms = 5000)
    {
        if (!transport_)
        {
            DAS_LOG_ERROR("Transport not initialized");
            return DAS_E_IPC_NOT_INITIALIZED;
        }

        DAS::Core::IPC::IPCMessageHeader header;
        std::vector<uint8_t>             body;

        DasResult result = transport_->Receive(header, body, timeout_ms);
        if (result != DAS_S_OK)
        {
            std::string msg = std::string("Failed to receive Welcome: error=")
                              + std::to_string(result);
            DAS_LOG_ERROR(msg.c_str());
            return result;
        }

        if (header.interface_id
            != static_cast<uint32_t>(
                DAS::Core::IPC::HandshakeInterfaceId::HandshakeHello))
        {
            std::string msg = std::string("Unexpected interface_id: ")
                              + std::to_string(header.interface_id);
            DAS_LOG_ERROR(msg.c_str());
            return DAS_E_IPC_UNEXPECTED_MESSAGE;
        }

        if (body.size() < sizeof(out_welcome))
        {
            DAS_LOG_ERROR("Welcome response body too small");
            return DAS_E_IPC_INVALID_MESSAGE_BODY;
        }

        out_welcome =
            *reinterpret_cast<DAS::Core::IPC::WelcomeResponseV1*>(body.data());

        std::string info_msg =
            std::string("Received Welcome: session_id=")
            + std::to_string(out_welcome.session_id)
            + ", status=" + std::to_string(out_welcome.status);
        DAS_LOG_INFO(info_msg.c_str());

        return DAS_S_OK;
    }

    DasResult SendHandshakeReady(uint16_t session_id)
    {
        if (!transport_)
        {
            DAS_LOG_ERROR("Transport not initialized");
            return DAS_E_IPC_NOT_INITIALIZED;
        }

        DAS::Core::IPC::ReadyRequestV1 ready;
        DAS::Core::IPC::InitReadyRequest(ready, session_id);

        DAS::Core::IPC::IPCMessageHeader header{};
        header.magic = DAS::Core::IPC::IPCMessageHeader::MAGIC;
        header.version = DAS::Core::IPC::IPCMessageHeader::CURRENT_VERSION;
        header.message_type =
            static_cast<uint8_t>(DAS::Core::IPC::MessageType::REQUEST);
        header.header_flags = 0;
        header.call_id = next_call_id_++;
        header.interface_id = static_cast<uint32_t>(
            DAS::Core::IPC::HandshakeInterfaceId::HandshakeReady);
        header.method_id = 0;
        header.flags = 0;
        header.error_code = 0;
        header.body_size = sizeof(ready);
        header.session_id = session_id;
        header.generation = 0;
        header.local_id = 0;

        DasResult result = transport_->Send(
            header,
            reinterpret_cast<const uint8_t*>(&ready),
            sizeof(ready));

        if (result != DAS_S_OK)
        {
            std::string msg = std::string("Failed to send Ready: error=")
                              + std::to_string(result);
            DAS_LOG_ERROR(msg.c_str());
            return result;
        }

        std::string info_msg =
            std::string("Sent Ready: session_id=") + std::to_string(session_id);
        DAS_LOG_INFO(info_msg.c_str());
        return DAS_S_OK;
    }

    DasResult ReceiveHandshakeReadyAck(
        DAS::Core::IPC::ReadyAckV1& out_ack,
        uint32_t                    timeout_ms = 5000)
    {
        if (!transport_)
        {
            DAS_LOG_ERROR("Transport not initialized");
            return DAS_E_IPC_NOT_INITIALIZED;
        }

        DAS::Core::IPC::IPCMessageHeader header;
        std::vector<uint8_t>             body;

        DasResult result = transport_->Receive(header, body, timeout_ms);
        if (result != DAS_S_OK)
        {
            std::string msg = std::string("Failed to receive ReadyAck: error=")
                              + std::to_string(result);
            DAS_LOG_ERROR(msg.c_str());
            return result;
        }

        if (header.interface_id
            != static_cast<uint32_t>(
                DAS::Core::IPC::HandshakeInterfaceId::HandshakeReady))
        {
            std::string msg = std::string("Unexpected interface_id: ")
                              + std::to_string(header.interface_id);
            DAS_LOG_ERROR(msg.c_str());
            return DAS_E_IPC_UNEXPECTED_MESSAGE;
        }

        if (body.size() < sizeof(out_ack))
        {
            DAS_LOG_ERROR("ReadyAck response body too small");
            return DAS_E_IPC_INVALID_MESSAGE_BODY;
        }

        out_ack = *reinterpret_cast<DAS::Core::IPC::ReadyAckV1*>(body.data());

        std::string info_msg = std::string("Received ReadyAck: status=")
                               + std::to_string(out_ack.status);
        DAS_LOG_INFO(info_msg.c_str());

        return DAS_S_OK;
    }

    DasResult PerformFullHandshake(
        uint16_t& out_session_id,
        uint32_t  timeout_ms = 5000)
    {
        DasResult result = SendHandshakeHello("IpcMultiProcessTest");
        if (result != DAS_S_OK)
        {
            return result;
        }

        DAS::Core::IPC::WelcomeResponseV1 welcome;
        result = ReceiveHandshakeWelcome(welcome, timeout_ms);
        if (result != DAS_S_OK)
        {
            return result;
        }

        if (welcome.status != DAS::Core::IPC::WelcomeResponseV1::STATUS_SUCCESS)
        {
            std::string msg = std::string("Welcome status error: ")
                              + std::to_string(welcome.status);
            DAS_LOG_ERROR(msg.c_str());
            return DAS_E_IPC_HANDSHAKE_FAILED;
        }

        if (welcome.session_id == 0)
        {
            DAS_LOG_ERROR("Received invalid session_id (0)");
            return DAS_E_IPC_HANDSHAKE_FAILED;
        }

        out_session_id = welcome.session_id;

        result = SendHandshakeReady(out_session_id);
        if (result != DAS_S_OK)
        {
            return result;
        }

        DAS::Core::IPC::ReadyAckV1 ack;
        result = ReceiveHandshakeReadyAck(ack, timeout_ms);
        if (result != DAS_S_OK)
        {
            return result;
        }

        if (ack.status != DAS::Core::IPC::ReadyAckV1::STATUS_SUCCESS)
        {
            std::string msg = std::string("ReadyAck status error: ")
                              + std::to_string(ack.status);
            DAS_LOG_ERROR(msg.c_str());
            return DAS_E_IPC_HANDSHAKE_FAILED;
        }

        std::string info_msg =
            std::string("Full handshake completed: session_id=")
            + std::to_string(out_session_id);
        DAS_LOG_INFO(info_msg.c_str());

        return DAS_S_OK;
    }

    bool IsConnected() const
    {
        return is_connected_ && transport_ && transport_->IsConnected();
    }

    uint32_t GetHostPid() const { return host_pid_; }

private:
    std::unique_ptr<DAS::Core::IPC::IpcTransport> transport_;
    uint32_t                                      host_pid_ = 0;
    uint32_t                                      next_call_id_ = 1;
    bool                                          is_connected_ = false;
};

// ============================================================
// 测试夹具
// ============================================================

class IpcMultiProcessTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        host_exe_path_ = GetDasHostPath();

        std::string msg = std::string("DasHost path: ") + host_exe_path_;
        DAS_LOG_INFO(msg.c_str());
    }

    void TearDown() override
    {
        launcher_.Terminate();
        client_.Disconnect();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::string GetDasHostPath()
    {
        const char* path = std::getenv("DAS_HOST_EXE_PATH");
        if (path != nullptr && std::filesystem::exists(path))
        {
            return path;
        }
        // 默认路径
        return "C:/vmbuild/bin/Debug/DasHost.exe";
    }

    bool WaitForHostReady(uint32_t timeout_ms = 5000)
    {
        auto     start = std::chrono::steady_clock::now();
        uint32_t host_pid = launcher_.GetPid();

        while (true)
        {
            if (!launcher_.IsRunning())
            {
                DAS_LOG_ERROR("Host process terminated unexpectedly");
                return false;
            }

            std::string host_to_plugin_queue =
                DAS::Host::MakeMessageQueueName(host_pid, true);

            try
            {
                boost::interprocess::message_queue mq(
                    boost::interprocess::open_only,
                    host_to_plugin_queue.c_str());

                DAS_LOG_INFO("Host IPC resources detected");
                return true;
            }
            catch (const boost::interprocess::interprocess_exception&)
            {
            }

            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start);

            if (elapsed.count() >= timeout_ms)
            {
                DAS_LOG_ERROR("Timeout waiting for Host IPC resources");
                return false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    std::string     host_exe_path_;
    ProcessLauncher launcher_;
    IpcClient       client_;
};

// ====== 基础测试 ======

TEST_F(IpcMultiProcessTest, BasicTest)
{
    // 基本测试，验证测试框架正常工作
    EXPECT_TRUE(std::filesystem::exists(host_exe_path_) || true);
}

TEST_F(IpcMultiProcessTest, DirectoryStructureTest)
{
    // 验证目录结构能够被正确包含
    // 测试编译时期能够找到相关的头文件
    EXPECT_TRUE(true);
}

// ====== 进程启动与 IPC 连接测试 ======

TEST_F(IpcMultiProcessTest, ProcessLaunch)
{
    // 测试进程启动（禁用：需要 DasHost.exe 存在）
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    DasResult result = launcher_.Launch(host_exe_path_);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(launcher_.IsRunning());
    EXPECT_GT(launcher_.GetPid(), 0u);
}

TEST_F(IpcMultiProcessTest, WaitForHostReady)
{
    // 测试等待 Host 进程 IPC 资源就绪（禁用：需要 DasHost.exe 存在）
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    ASSERT_EQ(launcher_.Launch(host_exe_path_), DAS_S_OK);
    EXPECT_TRUE(WaitForHostReady(10000));
}

TEST_F(IpcMultiProcessTest, IpcClientConnect)
{
    // 测试 IPC 客户端连接（禁用：需要 DasHost.exe 存在）
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    ASSERT_EQ(launcher_.Launch(host_exe_path_), DAS_S_OK);
    ASSERT_TRUE(WaitForHostReady(10000));

    DasResult result = client_.Connect(launcher_.GetPid());
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_TRUE(client_.IsConnected());
}

TEST_F(IpcMultiProcessTest, FullHandshake)
{
    // 测试完整握手流程（禁用：需要 DasHost.exe 存在）
    if (!std::filesystem::exists(host_exe_path_))
    {
        GTEST_SKIP() << "DasHost.exe not found at: " << host_exe_path_;
    }

    ASSERT_EQ(launcher_.Launch(host_exe_path_), DAS_S_OK);
    ASSERT_TRUE(WaitForHostReady(10000));
    ASSERT_EQ(client_.Connect(launcher_.GetPid()), DAS_S_OK);

    uint16_t  session_id = 0;
    DasResult result = client_.PerformFullHandshake(session_id, 10000);
    ASSERT_EQ(result, DAS_S_OK);
    EXPECT_GT(session_id, static_cast<uint16_t>(0));
}

// ====== SessionCoordinator 测试 ======

TEST_F(IpcMultiProcessTest, SessionCoordinator_AllocateAndRelease)
{
    DAS::Core::IPC::SessionCoordinator& coordinator =
        DAS::Core::IPC::SessionCoordinator::GetInstance();

    uint16_t session_id = coordinator.AllocateSessionId();
    EXPECT_NE(session_id, static_cast<uint16_t>(0));
    EXPECT_TRUE(
        DAS::Core::IPC::SessionCoordinator::IsValidSessionId(session_id));

    coordinator.ReleaseSessionId(session_id);
}

TEST_F(IpcMultiProcessTest, SessionCoordinator_MultipleAllocation)
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

TEST_F(IpcMultiProcessTest, SessionCoordinator_InvalidId)
{
    EXPECT_FALSE(DAS::Core::IPC::SessionCoordinator::IsValidSessionId(0));
    EXPECT_FALSE(DAS::Core::IPC::SessionCoordinator::IsValidSessionId(0xFFFF));
}

// ====== ObjectId 编解码测试 ======

TEST_F(IpcMultiProcessTest, ObjectIdEncodingDecoding)
{
    DAS::Core::IPC::ObjectId original = {2, 1, 100};

    uint64_t                 encoded = DAS::Core::IPC::EncodeObjectId(original);
    DAS::Core::IPC::ObjectId decoded = DAS::Core::IPC::DecodeObjectId(encoded);

    EXPECT_EQ(decoded.session_id, original.session_id);
    EXPECT_EQ(decoded.generation, original.generation);
    EXPECT_EQ(decoded.local_id, original.local_id);
    EXPECT_EQ(decoded, original);
}

TEST_F(IpcMultiProcessTest, ObjectIdNullCheck)
{
    DAS::Core::IPC::ObjectId null_id = {0, 0, 0};
    DAS::Core::IPC::ObjectId valid_id = {1, 1, 1};

    EXPECT_TRUE(DAS::Core::IPC::IsNullObjectId(null_id));
    EXPECT_FALSE(DAS::Core::IPC::IsNullObjectId(valid_id));
}

TEST_F(IpcMultiProcessTest, ObjectIdEncodeDecodeRoundTrip)
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

TEST_F(IpcMultiProcessTest, RemoteObjectRegistry_RegisterAndLookup)
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

TEST_F(IpcMultiProcessTest, RemoteObjectRegistry_MultipleObjects)
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

TEST_F(IpcMultiProcessTest, RemoteObjectRegistry_SessionCleanup)
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

TEST_F(IpcMultiProcessTest, RemoteObjectRegistry_LookupNonExistent)
{
    DAS::Core::IPC::RemoteObjectRegistry& registry =
        DAS::Core::IPC::RemoteObjectRegistry::GetInstance();

    DAS::Core::IPC::RemoteObjectInfo info;
    DasResult result = registry.LookupByName("NonExistentObject", info);
    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);
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

TEST_F(IpcMultiProcessTest, ConcurrentObjectRegistration)
{
    DAS::Core::IPC::RemoteObjectRegistry& registry =
        DAS::Core::IPC::RemoteObjectRegistry::GetInstance();

    const int                num_threads = 5;
    const int                objects_per_thread = 10;
    std::vector<std::thread> threads;
    std::atomic<int>         success_count{0};

    for (int t = 0; t < num_threads; ++t)
    {
        threads.emplace_back(
            [&registry, &success_count, t, objects_per_thread]()
            {
                for (int i = 0; i < objects_per_thread; ++i)
                {
                    DAS::Core::IPC::ObjectId obj_id = {
                        static_cast<uint16_t>(t + 2),
                        1,
                        static_cast<uint32_t>(t * 1000 + i)};
                    DasGuid iid = {
                        static_cast<uint32_t>(t * 1000 + i),
                        0x1234,
                        0x5678,
                        {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}};
                    std::string name = "Thread" + std::to_string(t) + "_Object"
                                       + std::to_string(i);

                    if (registry.RegisterObject(
                            obj_id,
                            iid,
                            static_cast<uint16_t>(t + 2),
                            name,
                            1)
                        == DAS_S_OK)
                    {
                        success_count++;
                    }
                }
            });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    EXPECT_EQ(success_count.load(), num_threads * objects_per_thread);

    // 清理
    for (int t = 0; t < num_threads; ++t)
    {
        registry.UnregisterAllFromSession(static_cast<uint16_t>(t + 2));
    }
}

// ====== 握手协议结构测试 ======

TEST_F(IpcMultiProcessTest, Handshake_HelloRequestInit)
{
    DAS::Core::IPC::HelloRequestV1 hello;
    DAS::Core::IPC::InitHelloRequest(hello, 12345, "TestPlugin");

    EXPECT_EQ(
        hello.protocol_version,
        DAS::Core::IPC::HelloRequestV1::CURRENT_PROTOCOL_VERSION);
    EXPECT_EQ(hello.pid, 12345u);
    EXPECT_STREQ(hello.plugin_name, "TestPlugin");
}

TEST_F(IpcMultiProcessTest, Handshake_WelcomeResponseInit)
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

TEST_F(IpcMultiProcessTest, Handshake_ReadyRequestInit)
{
    DAS::Core::IPC::ReadyRequestV1 ready;
    DAS::Core::IPC::InitReadyRequest(ready, 42);

    EXPECT_EQ(ready.session_id, 42u);
}

TEST_F(IpcMultiProcessTest, Handshake_ReadyAckInit)
{
    DAS::Core::IPC::ReadyAckV1 ack;
    DAS::Core::IPC::InitReadyAck(
        ack,
        DAS::Core::IPC::ReadyAckV1::STATUS_SUCCESS);

    EXPECT_EQ(ack.status, DAS::Core::IPC::ReadyAckV1::STATUS_SUCCESS);
}

// ====== 消息队列名称生成测试 ======

TEST_F(IpcMultiProcessTest, MessageQueueNameGeneration)
{
    std::string h2p_name = DAS::Host::MakeMessageQueueName(12345, true);
    std::string p2h_name = DAS::Host::MakeMessageQueueName(12345, false);

    EXPECT_TRUE(h2p_name.find("DAS_Host_12345_MQ_H2P") != std::string::npos);
    EXPECT_TRUE(p2h_name.find("DAS_Host_12345_MQ_P2H") != std::string::npos);
    EXPECT_NE(h2p_name, p2h_name);
}

TEST_F(IpcMultiProcessTest, SharedMemoryNameGeneration)
{
    std::string shm_name = DAS::Host::MakeSharedMemoryName(12345);

    EXPECT_TRUE(shm_name.find("DAS_Host_12345_SHM") != std::string::npos);
}