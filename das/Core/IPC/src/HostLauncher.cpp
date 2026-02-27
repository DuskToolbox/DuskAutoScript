/**
 * @file HostLauncher.cpp
 * @brief Host 进程启动器实现
 */

#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/Host/HostConfig.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcTransport.h>
#include <das/Utils/fmt.h>
#include <thread>

// 获取当前进程 PID 的跨平台方法
#ifdef _WIN32
// 必须在 windows.h 之前定义，避免 WinSock 冲突
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define GET_CURRENT_PID() GetCurrentProcessId()
#else
#include <unistd.h>
#define GET_CURRENT_PID() getpid()
#endif

// Disable warnings from boost::process headers
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4100) // unreferenced formal parameter
#pragma warning(disable : 4459) // declaration hides global declaration
#pragma warning(disable : 4244) // conversion, possible loss of data
#endif

#include <boost/asio/io_context.hpp>
#include <boost/interprocess/exceptions.hpp>
#include <boost/interprocess/ipc/message_queue.hpp>
#include <boost/process/v2/pid.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/system/error_code.hpp>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <chrono>
#include <filesystem>
#include <thread>
DAS_CORE_IPC_NS_BEGIN

/**
 * @brief HostLauncher 实现类
 *
 * 使用 PIMPL 模式隐藏 boost::process 等依赖
 */
struct HostLauncher::Impl
{
    boost::asio::io_context                      io_ctx;
    std::unique_ptr<boost::process::v2::process> process;
    std::unique_ptr<IpcTransport>                transport;
    uint32_t                                     pid = 0;
    uint16_t                                     session_id = 0;
    uint64_t                                     next_call_id = 1;
    bool                                         is_running = false;
};

HostLauncher::HostLauncher() : impl_(std::make_unique<Impl>()) {}

HostLauncher::~HostLauncher() { Stop(); }

DasResult HostLauncher::Start(
    const std::string& host_exe_path,
    const std::string& plugin_path,
    uint16_t&          out_session_id,
    uint32_t           timeout_ms)
{
    // 检查可执行文件是否存在
    if (!std::filesystem::exists(host_exe_path))
    {
        std::string msg =
            DAS_FMT_NS::format("Host executable not found: {}", host_exe_path);
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_INVALID_ARGUMENT;
    }

    // 获取当前进程 PID（主进程 PID）
    uint32_t main_pid = static_cast<uint32_t>(GET_CURRENT_PID());

    // 构建命令行参数
    std::vector<std::string> args;
    args.push_back("--main-pid");
    args.push_back(std::to_string(main_pid));
    if (!plugin_path.empty())
    {
        args.push_back("--plugin");
        args.push_back(plugin_path);
    }

    // 启动进程
    DasResult result = LaunchProcess(host_exe_path, args);
    if (result != DAS_S_OK)
    {
        return result;
    }

    // 等待 Host IPC 资源就绪
    result = WaitForHostReady(timeout_ms);
    if (result != DAS_S_OK)
    {
        Stop();
        return result;
    }

    // 连接到 Host IPC
    result = ConnectToHost();
    if (result != DAS_S_OK)
    {
        Stop();
        return result;
    }

    // 执行握手
    result = PerformFullHandshake(out_session_id, timeout_ms);
    if (result != DAS_S_OK)
    {
        Stop();
        return result;
    }

    impl_->session_id = out_session_id;

    std::string msg = DAS_FMT_NS::format(
        "Host process started successfully: PID={}, session_id={}",
        impl_->pid,
        out_session_id);
    DAS_LOG_INFO(msg.c_str());

    return DAS_S_OK;
}

void HostLauncher::Stop()
{
    if (impl_->transport)
    {
        impl_->transport->Shutdown();
        impl_->transport.reset();
    }

    if (impl_->process)
    {
        std::string msg =
            DAS_FMT_NS::format("Terminating Host process: PID={}", impl_->pid);
        DAS_LOG_INFO(msg.c_str());

        boost::system::error_code ec;
        impl_->process->terminate(ec);
        impl_->process.reset();
    }

    impl_->is_running = false;
    impl_->session_id = 0;
    impl_->pid = 0;
}

bool HostLauncher::IsRunning() const
{
    return impl_->process && impl_->is_running;
}

uint32_t HostLauncher::GetPid() const { return impl_->pid; }

uint16_t HostLauncher::GetSessionId() const { return impl_->session_id; }

IpcTransport* HostLauncher::GetTransport() { return impl_->transport.get(); }

DasResult HostLauncher::LaunchProcess(
    const std::string&              exe_path,
    const std::vector<std::string>& args)
{
    try
    {
        impl_->process = std::make_unique<boost::process::v2::process>(
            impl_->io_ctx,
            exe_path,
            args,
            boost::process::v2::process_start_dir(
                std::filesystem::path(exe_path).parent_path().string()));

        impl_->pid = static_cast<uint32_t>(impl_->process->id());
        impl_->is_running = true;

        std::string msg =
            DAS_FMT_NS::format("Host process launched: PID={}", impl_->pid);
        DAS_LOG_INFO(msg.c_str());

        return DAS_S_OK;
    }
    catch (const std::exception& e)
    {
        std::string msg = DAS_FMT_NS::format(
            "Exception launching Host process: {}",
            e.what());
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }
}

DasResult HostLauncher::WaitForHostReady(uint32_t timeout_ms)
{
    auto     start = std::chrono::steady_clock::now();
    uint32_t main_pid = static_cast<uint32_t>(GET_CURRENT_PID());
    uint32_t host_pid = impl_->pid;

    while (true)
    {
        if (!IsRunning())
        {
            DAS_LOG_ERROR("Host process terminated unexpectedly");
            return DAS_E_IPC_CONNECTION_LOST;
        }

        std::string host_to_plugin_queue =
            Host::MakeMessageQueueName(main_pid, host_pid, false);

        try
        {
            boost::interprocess::message_queue mq(
                boost::interprocess::open_only,
                host_to_plugin_queue.c_str());

            DAS_LOG_INFO("Host IPC resources detected");
            return DAS_S_OK;
        }
        catch (const boost::interprocess::interprocess_exception&)
        {
            // 继续等待
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        if (elapsed.count() >= timeout_ms)
        {
            DAS_LOG_ERROR("Timeout waiting for Host IPC resources");
            return DAS_E_IPC_TIMEOUT;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

DasResult HostLauncher::ConnectToHost()
{
    uint32_t main_pid = static_cast<uint32_t>(GET_CURRENT_PID());
    uint32_t host_pid = impl_->pid;

    std::string host_to_plugin_queue =
        Host::MakeMessageQueueName(main_pid, host_pid, false);
    std::string plugin_to_host_queue =
        Host::MakeMessageQueueName(main_pid, host_pid, true);

    std::string msg = DAS_FMT_NS::format(
        "Connecting to Host IPC: {}, {}",
        host_to_plugin_queue,
        plugin_to_host_queue);
    DAS_LOG_INFO(msg.c_str());

    impl_->transport = std::make_unique<IpcTransport>();

    DasResult result =
        impl_->transport->Connect(host_to_plugin_queue, plugin_to_host_queue);

    if (result != DAS_S_OK)
    {
        std::string err_msg = DAS_FMT_NS::format(
            "Failed to connect to Host IPC: error={}",
            result);
        DAS_LOG_ERROR(err_msg.c_str());
        impl_->transport.reset();
        return result;
    }

    DAS_LOG_INFO("Connected to Host IPC successfully");
    return DAS_S_OK;
}

DasResult HostLauncher::PerformFullHandshake(
    uint16_t& out_session_id,
    uint32_t  timeout_ms)
{
    DasResult result = SendHandshakeHello("HostLauncher");
    if (result != DAS_S_OK)
    {
        return result;
    }

    result = ReceiveHandshakeWelcome(out_session_id, timeout_ms);
    if (result != DAS_S_OK)
    {
        return result;
    }

    if (out_session_id == 0)
    {
        DAS_LOG_ERROR("Received invalid session_id (0)");
        return DAS_E_IPC_HANDSHAKE_FAILED;
    }

    result = SendHandshakeReady(out_session_id);
    if (result != DAS_S_OK)
    {
        return result;
    }

    result = ReceiveHandshakeReadyAck(timeout_ms);
    if (result != DAS_S_OK)
    {
        return result;
    }

    std::string msg = DAS_FMT_NS::format(
        "Full handshake completed: session_id={}",
        out_session_id);
    DAS_LOG_INFO(msg.c_str());

    return DAS_S_OK;
}

DasResult HostLauncher::SendHandshakeHello(const std::string& client_name)
{
    if (!impl_->transport)
    {
        DAS_LOG_ERROR("Transport not initialized");
        return DAS_E_IPC_NOT_INITIALIZED;
    }

    uint32_t my_pid = static_cast<uint32_t>(GET_CURRENT_PID());

    HelloRequestV1 hello;
    InitHelloRequest(hello, my_pid, client_name.c_str());

    IPCMessageHeader header{};
    header.magic = IPCMessageHeader::MAGIC;
    header.version = IPCMessageHeader::CURRENT_VERSION;
    header.message_type = static_cast<uint8_t>(MessageType::REQUEST);
    header.header_flags = 0;
    header.call_id = impl_->next_call_id++;
    header.interface_id =
        static_cast<uint32_t>(HandshakeInterfaceId::HANDSHAKE_IFACE_HELLO);
    header.method_id = 0;
    header.flags = 0;
    header.error_code = 0;
    header.body_size = sizeof(hello);
    header.session_id = 0;
    header.generation = 0;
    header.local_id = 0;

    DasResult result = impl_->transport->Send(
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

    std::string info_msg =
        DAS_FMT_NS::format("Sent Hello: pid={}, name={}", my_pid, client_name);
    DAS_LOG_INFO(info_msg.c_str());
    return DAS_S_OK;
}

DasResult HostLauncher::ReceiveHandshakeWelcome(
    uint16_t& out_session_id,
    uint32_t  timeout_ms)
{
    if (!impl_->transport)
    {
        DAS_LOG_ERROR("Transport not initialized");
        return DAS_E_IPC_NOT_INITIALIZED;
    }

    IPCMessageHeader     header;
    std::vector<uint8_t> body;

    DasResult result = impl_->transport->Receive(header, body, timeout_ms);
    if (result != DAS_S_OK)
    {
        std::string msg =
            DAS_FMT_NS::format("Failed to receive Welcome: error={}", result);
        DAS_LOG_ERROR(msg.c_str());
        return result;
    }

    if (header.interface_id
        != static_cast<uint32_t>(HandshakeInterfaceId::HANDSHAKE_IFACE_WELCOME))
    {
        std::string msg = DAS_FMT_NS::format(
            "Unexpected interface_id: {}",
            header.interface_id);
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_UNEXPECTED_MESSAGE;
    }

    if (body.size() < sizeof(WelcomeResponseV1))
    {
        DAS_LOG_ERROR("Welcome response body too small");
        return DAS_E_IPC_INVALID_MESSAGE_BODY;
    }

    WelcomeResponseV1 welcome =
        *reinterpret_cast<WelcomeResponseV1*>(body.data());

    if (welcome.status != WelcomeResponseV1::STATUS_SUCCESS)
    {
        std::string msg =
            DAS_FMT_NS::format("Welcome status error: {}", welcome.status);
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_HANDSHAKE_FAILED;
    }

    out_session_id = welcome.session_id;

    std::string info_msg = DAS_FMT_NS::format(
        "Received Welcome: session_id={}, status={}",
        welcome.session_id,
        welcome.status);
    DAS_LOG_INFO(info_msg.c_str());

    return DAS_S_OK;
}

DasResult HostLauncher::SendHandshakeReady(uint16_t session_id)
{
    if (!impl_->transport)
    {
        DAS_LOG_ERROR("Transport not initialized");
        return DAS_E_IPC_NOT_INITIALIZED;
    }

    ReadyRequestV1 ready;
    InitReadyRequest(ready, session_id);

    IPCMessageHeader header{};
    header.magic = IPCMessageHeader::MAGIC;
    header.version = IPCMessageHeader::CURRENT_VERSION;
    header.message_type = static_cast<uint8_t>(MessageType::REQUEST);
    header.header_flags = 0;
    header.call_id = impl_->next_call_id++;
    header.interface_id =
        static_cast<uint32_t>(HandshakeInterfaceId::HANDSHAKE_IFACE_READY);
    header.method_id = 0;
    header.flags = 0;
    header.error_code = 0;
    header.body_size = sizeof(ready);
    header.session_id = session_id;
    header.generation = 0;
    header.local_id = 0;

    DasResult result = impl_->transport->Send(
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

DasResult HostLauncher::ReceiveHandshakeReadyAck(uint32_t timeout_ms)
{
    if (!impl_->transport)
    {
        DAS_LOG_ERROR("Transport not initialized");
        return DAS_E_IPC_NOT_INITIALIZED;
    }

    IPCMessageHeader     header;
    std::vector<uint8_t> body;

    DasResult result = impl_->transport->Receive(header, body, timeout_ms);
    if (result != DAS_S_OK)
    {
        std::string msg =
            DAS_FMT_NS::format("Failed to receive ReadyAck: error={}", result);
        DAS_LOG_ERROR(msg.c_str());
        return result;
    }

    if (header.interface_id
        != static_cast<uint32_t>(
            HandshakeInterfaceId::HANDSHAKE_IFACE_READY_ACK))
    {
        std::string msg = DAS_FMT_NS::format(
            "Unexpected interface_id: {}",
            header.interface_id);
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_UNEXPECTED_MESSAGE;
    }

    if (body.size() < sizeof(ReadyAckV1))
    {
        DAS_LOG_ERROR("ReadyAck response body too small");
        return DAS_E_IPC_INVALID_MESSAGE_BODY;
    }

    ReadyAckV1 ack = *reinterpret_cast<ReadyAckV1*>(body.data());

    if (ack.status != ReadyAckV1::STATUS_SUCCESS)
    {
        std::string msg =
            DAS_FMT_NS::format("ReadyAck status error: {}", ack.status);
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_HANDSHAKE_FAILED;
    }

    std::string info_msg =
        DAS_FMT_NS::format("Received ReadyAck: status={}", ack.status);
    DAS_LOG_INFO(info_msg.c_str());

    return DAS_S_OK;
}
DAS_CORE_IPC_NS_END
