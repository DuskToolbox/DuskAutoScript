/**
 * @file IpcMultiProcessTestCommon.h
 * @brief IPC 多进程测试共享组件
 *
 * 包含用于 IPC 多进程测试的辅助类和测试夹具：
 * - ProcessLauncher: 启动外部进程
 * - IpcClient: IPC 客户端，用于与 Host 进程通信
 * - IpcMultiProcessTest: 测试夹具
 */

#pragma once

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/Host/HostConfig.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/MessageQueueTransport.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/IPC/SessionCoordinator.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>
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
            std::string msg =
                DAS_FMT_NS::format("Executable not found: {}", exe_path);
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
                DAS_FMT_NS::format("Process launched: PID={}", pid_);
            DAS_LOG_INFO(msg.c_str());
            return DAS_S_OK;
        }
        catch (const std::exception& e)
        {
            std::string msg =
                DAS_FMT_NS::format("Exception launching process: {}", e.what());
            DAS_LOG_ERROR(msg.c_str());
            return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
        }
    }

    void Terminate()
    {
        if (process_)
        {
            std::string msg =
                DAS_FMT_NS::format("Terminating process: PID={}", pid_);
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
            DAS::Core::IPC::Host::MakeMessageQueueName(host_pid, true);
        std::string plugin_to_host_queue =
            DAS::Core::IPC::Host::MakeMessageQueueName(host_pid, false);

        std::string msg = DAS_FMT_NS::format(
            "Connecting to Host IPC: {}, {}",
            host_to_plugin_queue,
            plugin_to_host_queue);
        DAS_LOG_INFO(msg.c_str());

        transport_ = std::make_unique<DAS::Core::IPC::IpcTransport>();

        DasResult result =
            transport_->Connect(host_to_plugin_queue, plugin_to_host_queue);

        if (result != DAS_S_OK)
        {
            std::string err_msg = DAS_FMT_NS::format(
                "Failed to connect to Host IPC: error={}",
                result);
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
            std::string msg =
                DAS_FMT_NS::format("Failed to send Hello: error={}", result);
            DAS_LOG_ERROR(msg.c_str());
            return result;
        }

        std::string info_msg = DAS_FMT_NS::format(
            "Sent Hello: pid={}, name={}",
            my_pid,
            plugin_name);
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
            std::string msg = DAS_FMT_NS::format(
                "Failed to receive Welcome: error={}",
                result);
            DAS_LOG_ERROR(msg.c_str());
            return result;
        }

        if (header.interface_id
            != static_cast<uint32_t>(
                DAS::Core::IPC::HandshakeInterfaceId::HandshakeHello))
        {
            std::string msg = DAS_FMT_NS::format(
                "Unexpected interface_id: {}",
                header.interface_id);
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

        std::string info_msg = DAS_FMT_NS::format(
            "Received Welcome: session_id={}, status={}",
            out_welcome.session_id,
            out_welcome.status);
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
            std::string msg =
                DAS_FMT_NS::format("Failed to send Ready: error={}", result);
            DAS_LOG_ERROR(msg.c_str());
            return result;
        }

        std::string info_msg =
            DAS_FMT_NS::format("Sent Ready: session_id={}", session_id);
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
            std::string msg = DAS_FMT_NS::format(
                "Failed to receive ReadyAck: error={}",
                result);
            DAS_LOG_ERROR(msg.c_str());
            return result;
        }

        if (header.interface_id
            != static_cast<uint32_t>(
                DAS::Core::IPC::HandshakeInterfaceId::HandshakeReady))
        {
            std::string msg = DAS_FMT_NS::format(
                "Unexpected interface_id: {}",
                header.interface_id);
            DAS_LOG_ERROR(msg.c_str());
            return DAS_E_IPC_UNEXPECTED_MESSAGE;
        }

        if (body.size() < sizeof(out_ack))
        {
            DAS_LOG_ERROR("ReadyAck response body too small");
            return DAS_E_IPC_INVALID_MESSAGE_BODY;
        }

        out_ack = *reinterpret_cast<DAS::Core::IPC::ReadyAckV1*>(body.data());

        std::string info_msg =
            DAS_FMT_NS::format("Received ReadyAck: status={}", out_ack.status);
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
            std::string msg =
                DAS_FMT_NS::format("Welcome status error: {}", welcome.status);
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
            std::string msg =
                DAS_FMT_NS::format("ReadyAck status error: {}", ack.status);
            DAS_LOG_ERROR(msg.c_str());
            return DAS_E_IPC_HANDSHAKE_FAILED;
        }

        std::string info_msg = DAS_FMT_NS::format(
            "Full handshake completed: session_id={}",
            out_session_id);
        DAS_LOG_INFO(info_msg.c_str());

        return DAS_S_OK;
    }

    DasResult SendLoadPlugin(const std::string& json_manifest_path)
    {
        if (!transport_)
        {
            DAS_LOG_ERROR("Transport not initialized");
            return DAS_E_IPC_NOT_INITIALIZED;
        }

        uint16_t path_len = static_cast<uint16_t>(json_manifest_path.size());

        size_t               payload_size = sizeof(uint16_t) + path_len;
        std::vector<uint8_t> payload_buffer(payload_size);

        size_t offset = 0;
        std::memcpy(
            payload_buffer.data() + offset,
            &path_len,
            sizeof(path_len));
        offset += sizeof(path_len);
        std::memcpy(
            payload_buffer.data() + offset,
            json_manifest_path.c_str(),
            path_len);

        DAS::Core::IPC::IPCMessageHeader header{};
        header.magic = DAS::Core::IPC::IPCMessageHeader::MAGIC;
        header.version = DAS::Core::IPC::IPCMessageHeader::CURRENT_VERSION;
        header.message_type =
            static_cast<uint8_t>(DAS::Core::IPC::MessageType::REQUEST);
        header.header_flags = 0;
        header.call_id = next_call_id_++;
        header.interface_id =
            static_cast<uint32_t>(DAS::Core::IPC::IpcCommandType::LOAD_PLUGIN);
        header.method_id = 0;
        header.flags = 0;
        header.error_code = 0;
        header.body_size = static_cast<uint32_t>(payload_buffer.size());
        header.session_id = 0;
        header.generation = 0;
        header.local_id = 0;

        DasResult result = transport_->Send(
            header,
            payload_buffer.data(),
            static_cast<uint32_t>(payload_buffer.size()));

        if (result != DAS_S_OK)
        {
            std::string msg = DAS_FMT_NS::format(
                "Failed to send LoadPlugin: error={}",
                result);
            DAS_LOG_ERROR(msg.c_str());
            return result;
        }

        std::string info_msg =
            DAS_FMT_NS::format("Sent LoadPlugin: path={}", json_manifest_path);
        DAS_LOG_INFO(info_msg.c_str());

        return DAS_S_OK;
    }

    DasResult ReceiveLoadPluginResponse(
        DAS::Core::IPC::ObjectId& out_object_id,
        uint32_t&                 out_error_code,
        uint32_t                  timeout_ms = 5000)
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
            std::string msg = DAS_FMT_NS::format(
                "Failed to receive LoadPlugin response: error={}",
                result);
            DAS_LOG_ERROR(msg.c_str());
            return result;
        }

        if (header.interface_id
            != static_cast<uint32_t>(
                DAS::Core::IPC::IpcCommandType::LOAD_PLUGIN))
        {
            std::string msg = DAS_FMT_NS::format(
                "Unexpected interface_id: {}",
                header.interface_id);
            DAS_LOG_ERROR(msg.c_str());
            return DAS_E_IPC_UNEXPECTED_MESSAGE;
        }

        out_error_code = header.error_code;

        // 如果返回错误码，直接返回成功（表示成功接收了错误响应）
        if (out_error_code != DAS_S_OK)
        {
            std::string err_msg = DAS_FMT_NS::format(
                "LoadPlugin failed with error code: {}",
                out_error_code);
            DAS_LOG_ERROR(err_msg.c_str());
            return DAS_S_OK; // DAS_S_OK 表示成功接收响应,但操作失败(error_code
                             // 包含失败信息)
        }

        // 只有在成功时才解析 body
        using LoadPluginResponsePayload =
            DAS::Core::IPC::LoadPluginResponsePayload;
        if (body.size() < sizeof(LoadPluginResponsePayload))
        {
            DAS_LOG_ERROR("LoadPlugin response body too small");
            return DAS_E_IPC_INVALID_MESSAGE_BODY;
        }

        const LoadPluginResponsePayload* response =
            reinterpret_cast<const LoadPluginResponsePayload*>(body.data());
        out_object_id = response->object_id;

        std::string info_msg = DAS_FMT_NS::format(
            "Received LoadPlugin response: object_id=[{}, {}, {}]",
            out_object_id.session_id,
            out_object_id.generation,
            out_object_id.local_id);
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
// 测试夹具 - 用于真正启动进程的集成测试
// ============================================================

class IpcMultiProcessTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        host_exe_path_ = GetDasHostPath();

        std::string msg =
            DAS_FMT_NS::format("DasHost path: {}", host_exe_path_);
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
        if (path == nullptr || strlen(path) == 0)
        {
            throw std::runtime_error(
                "DAS_HOST_EXE_PATH environment variable is not set");
        }
        if (!std::filesystem::exists(path))
        {
            throw std::runtime_error(
                std::string("DasHost.exe not found at: ") + path);
        }
        return path;
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
                DAS::Core::IPC::Host::MakeMessageQueueName(host_pid, true);

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
