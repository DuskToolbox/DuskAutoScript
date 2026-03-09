/**
 * @file HostLauncher.cpp
 * @brief Host 进程启动器实现
 */

#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/Host/HostConfig.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/MainProcess/IHostLauncher.h>
#include <das/Core/IPC/SessionCoordinator.h>
#include <das/DasPtr.hpp>
#include <das/Utils/fmt.h>
#include <stdexec/execution.hpp>
#include <thread>

#ifdef _WIN32
#include <das/Core/IPC/Win32AsyncIpcTransport.h>
#else
#include <das/Core/IPC/UnixAsyncIpcTransport.h>
#endif

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
    explicit Impl(boost::asio::io_context& ctx) : io_ctx(ctx) {}

    boost::asio::io_context& io_ctx; // 引用，由外部管理生命周期
    std::unique_ptr<boost::process::v2::process> process;
    std::unique_ptr<DefaultAsyncIpcTransport>    async_transport;
    uint32_t                                     pid = 0;
    uint16_t                                     session_id = 0;
    uint64_t                                     next_call_id = 1;
    bool                                         is_running = false;
    std::atomic<uint32_t>                        ref_count{1}; // 引用计数，初始为 1（创建时持有）
};

HostLauncher::HostLauncher(boost::asio::io_context& io_ctx)
    : impl_(std::make_unique<Impl>(io_ctx))
{
}

HostLauncher::~HostLauncher() { Stop(); }

DasResult HostLauncher::Start(
    const std::string& host_exe_path,
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

    std::string pid_msg = DAS_FMT_NS::format(
        "HostLauncher::Start - Main process PID: {}",
        main_pid);
    DAS_LOG_INFO(pid_msg.c_str());

    // 构建命令行参数
    std::vector<std::string> args;
    args.push_back("--main-pid");
    args.push_back(std::to_string(main_pid));

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
    // 先发送 GOODBYE 消息让 Host 进程优雅退出
    if (impl_->async_transport && impl_->is_running)
    {
        IPCMessageHeader header{};
        header.magic = IPCMessageHeader::MAGIC;
        header.version = IPCMessageHeader::CURRENT_VERSION;
        header.message_type = static_cast<uint8_t>(MessageType::REQUEST);
        header.interface_id = static_cast<uint32_t>(
            HandshakeInterfaceId::HANDSHAKE_IFACE_GOODBYE);
        header.method_id = 0;
        header.call_id = 0;
        header.flags = 0;
        header.error_code = 0;
        header.session_id = 0; // 控制平面消息: ObjectId = {0, 0, 0}
        header.generation = 0;
        header.local_id = 0;

        GoodbyeV1 goodbye{};
        goodbye.reason = static_cast<uint32_t>(GoodbyeReason::NormalShutdown);

        std::string log_msg = DAS_FMT_NS::format(
            "Sending GOODBYE to Host process: PID={}",
            impl_->pid);
        DAS_LOG_INFO(log_msg.c_str());

        auto validated_header =
            IPCMessageHeaderBuilder()
                .SetMessageType(MessageType::REQUEST)
                .SetControlPlaneCommand(
                    HandshakeInterfaceId::HANDSHAKE_IFACE_GOODBYE)
                .SetBodySize(sizeof(goodbye))
                .Build();

        try
        {
            auto send_result = stdexec::sync_wait(
                impl_->async_transport->Send(
                    validated_header,
                    reinterpret_cast<const uint8_t*>(&goodbye),
                    sizeof(goodbye)));

            if (send_result.has_value())
            {
                auto&& [ec, result] = std::move(*send_result);
                if (ec)
                {
                    DAS_LOG_ERROR("Failed to send GOODBYE: exception occurred");
                }
                else if (result != DAS_S_OK)
                {
                    std::string err_msg = DAS_FMT_NS::format(
                        "Failed to send GOODBYE: error={}",
                        result);
                    DAS_LOG_ERROR(err_msg.c_str());
                }
            }
        }
        catch (const std::exception& e)
        {
            std::string err_msg = DAS_FMT_NS::format(
                "sync_wait failed for GOODBYE: {}",
                e.what());
            DAS_LOG_ERROR(err_msg.c_str());
        }

        // 等待进程退出，最多等待 2 秒
        // 如果进程在 GOODBYE 后正常退出，则不需要 terminate
        bool process_exited = false;
        for (int i = 0; i < 20; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!impl_->process->running())
            {
                process_exited = true;
                break;
            }
        }

        if (process_exited)
        {
            std::string exit_msg = DAS_FMT_NS::format(
                "Host process exited gracefully: PID={}",
                impl_->pid);
            DAS_LOG_INFO(exit_msg.c_str());
        }
    }

    if (impl_->async_transport)
    {
        impl_->async_transport->Close();
        impl_->async_transport.reset();
    }

    if (impl_->process)
    {
        boost::system::error_code ec;

        // 检查进程是否仍在运行
        bool still_running = impl_->process->running(ec);

        if (still_running)
        {
            std::string msg = DAS_FMT_NS::format(
                "HostLauncher::Stop - terminating Host process: pid={}, session_id={}",
                impl_->pid,
                impl_->session_id);
            DAS_LOG_INFO(msg.c_str());
            impl_->process->terminate(ec);
        }

        // 释放进程句柄所有权，避免析构函数中任何潜在的阻塞
        // 注意：这会导致进程句柄泄漏，但避免了测试超时问题
        (void)impl_->process.release();
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

DefaultAsyncIpcTransport* HostLauncher::GetTransport() { return impl_->async_transport.get(); }

DasResult HostLauncher::StartAsync(
    const std::string&            host_exe_path,
    IDasAsyncHandshakeOperation** pp_out_operation)
{
    if (!pp_out_operation)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    // TODO: 实现异步版本 - 使用 io_context 异步执行
    // 暂时返回同步实现
    constexpr uint32_t DEFAULT_TIMEOUT_MS = 5000;
    uint16_t           session_id = 0;
    DasResult result = Start(host_exe_path, session_id, DEFAULT_TIMEOUT_MS);
    *pp_out_operation = nullptr;
    return result;
}

uint32_t HostLauncher::AddRef()
{
    return ++impl_->ref_count;
}

uint32_t HostLauncher::Release()
{
    auto r = --impl_->ref_count;
    if (r == 0)
    {
        delete this;
    }
    return r;
}

DasResult HostLauncher::QueryInterface(const DasGuid& iid, void** pp)
{
    if (iid == DasIidOf<IDasBase>())
    {
        AddRef();
        *pp = static_cast<IDasBase*>(this);
        return DAS_S_OK;
    }
    return DAS_E_NO_INTERFACE;
}

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

        std::string host_to_plugin_pipe = DAS_FMT_NS::format(
            "\\\\.\\pipe\\das_ipc_{}_{}_m2h",
            main_pid,
            host_pid);

        HANDLE h_pipe = CreateFileA(
            host_to_plugin_pipe.c_str(),
            GENERIC_READ,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr);

        if (h_pipe != INVALID_HANDLE_VALUE)
        {
            CloseHandle(h_pipe);
            DAS_LOG_INFO("Host IPC resources detected");
            return DAS_S_OK;
        }

        auto last_error = GetLastError();
        if (last_error != ERROR_FILE_NOT_FOUND)
        {
            DAS_LOG_INFO("Host IPC resources detected");
            return DAS_S_OK;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        if (elapsed.count()
            >= (timeout_ms == 0
                    ? (std::numeric_limits<decltype(timeout_ms)>::max)()
                    : timeout_ms))
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

    std::string host_to_plugin_pipe =
        Host::MakeMessageQueueName(main_pid, host_pid, false);
    std::string plugin_to_host_pipe =
        Host::MakeMessageQueueName(main_pid, host_pid, true);

    std::string msg = DAS_FMT_NS::format(
        "Creating pipes for Host IPC: {}, {} (MainProcess is server)",
        host_to_plugin_pipe,
        plugin_to_host_pipe);
    DAS_LOG_INFO(msg.c_str());

    impl_->async_transport = std::make_unique<DefaultAsyncIpcTransport>(impl_->io_ctx);

    // MainProcess 是服务端：创建管道并等待 Host 进程连接
    // is_server = true 表示服务端角色
    DasResult result =
        impl_->async_transport->Initialize(
            host_to_plugin_pipe,
            plugin_to_host_pipe,
            true,    // is_server = true (MainProcess creates pipes and waits for Host)
            65536);

    if (result != DAS_S_OK)
    {
        std::string err_msg = DAS_FMT_NS::format(
            "Failed to create/wait for Host IPC: error={}",
            result);
        DAS_LOG_ERROR(err_msg.c_str());
        impl_->async_transport.reset();
        return result;
    }

    DAS_LOG_INFO("Host IPC pipes created and Host connected successfully");
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
    if (!impl_->async_transport)
    {
        DAS_LOG_ERROR("Transport not initialized");
        return DAS_E_IPC_NOT_INITIALIZED;
    }

    uint32_t my_pid = static_cast<uint32_t>(GET_CURRENT_PID());

    // 为主进程分配 session_id 给 Host
    uint16_t assigned_session_id =
        SessionCoordinator::GetInstance().AllocateSessionId();
    if (assigned_session_id == 0)
    {
        DAS_LOG_ERROR("Failed to allocate session_id for Host");
        return DAS_E_IPC_SESSION_ALLOC_FAILED;
    }

    HelloRequestV1 hello;
    InitHelloRequest(hello, my_pid, client_name.c_str());
    hello.assigned_session_id = assigned_session_id; // 设置分配的 session_id

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

    auto validated_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::REQUEST)
            .SetControlPlaneCommand(HandshakeInterfaceId::HANDSHAKE_IFACE_HELLO)
            .SetBodySize(sizeof(hello))
            .SetCallId(header.call_id)
            .Build();

    try
    {
        auto send_result_opt = stdexec::sync_wait(
            impl_->async_transport->Send(
                validated_header,
                reinterpret_cast<const uint8_t*>(&hello),
                sizeof(hello)));

        if (!send_result_opt.has_value())
        {
            DAS_LOG_ERROR("Failed to send Hello: sync_wait returned empty");
            return DAS_E_IPC_SEND_FAILED;
        }

        auto&& [ec, result] = std::move(*send_result_opt);
        if (ec)
        {
            try
            {
                std::rethrow_exception(ec);
            }
            catch (const std::exception& e)
            {
                std::string msg = DAS_FMT_NS::format(
                    "Failed to send Hello: {}",
                    e.what());
                DAS_LOG_ERROR(msg.c_str());
            }
            return DAS_E_IPC_SEND_FAILED;
        }

        if (result != DAS_S_OK)
        {
            std::string msg = DAS_FMT_NS::format(
                "Failed to send Hello: error={}",
                result);
            DAS_LOG_ERROR(msg.c_str());
            return result;
        }
    }
    catch (const std::exception& e)
    {
        std::string msg = DAS_FMT_NS::format(
            "sync_wait failed for Hello: {}",
            e.what());
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_SEND_FAILED;
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
    if (!impl_->async_transport)
    {
        DAS_LOG_ERROR("Transport not initialized");
        return DAS_E_IPC_NOT_INITIALIZED;
    }

    // 注意：当前实现没有真正的超时机制，timeout_ms 暂时忽略
    // TODO: 后续使用 stdexec::timeout 算法添加超时支持
    (void)timeout_ms;

    try
    {
        auto receive_result_opt = stdexec::sync_wait(
            impl_->async_transport->Receive());

        if (!receive_result_opt.has_value())
        {
            DAS_LOG_ERROR("Failed to receive Welcome: sync_wait returned empty");
            return DAS_E_IPC_RECEIVE_FAILED;
        }

        auto&& [ec, result_variant] = std::move(*receive_result_opt);

        if (ec)
        {
            try
            {
                std::rethrow_exception(ec);
            }
            catch (const std::exception& e)
            {
                std::string msg = DAS_FMT_NS::format(
                    "Failed to receive Welcome: {}",
                    e.what());
                DAS_LOG_ERROR(msg.c_str());
            }
            return DAS_E_IPC_RECEIVE_FAILED;
        }

        if (result_variant.index() == 0)
        {
            DasResult error_code = std::get<0>(result_variant);
            std::string msg = DAS_FMT_NS::format(
                "Failed to receive Welcome: error={}",
                error_code);
            DAS_LOG_ERROR(msg.c_str());
            return error_code;
        }

        auto&& [header, body] = std::get<1>(result_variant);

        const IPCMessageHeader& raw_header = header.Raw();

        if (raw_header.interface_id
            != static_cast<uint32_t>(HandshakeInterfaceId::HANDSHAKE_IFACE_WELCOME))
        {
            std::string msg = DAS_FMT_NS::format(
                "Unexpected interface_id: {}",
                raw_header.interface_id);
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
            std::string msg = DAS_FMT_NS::format(
                "Welcome status error: {}",
                welcome.status);
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
    catch (const std::exception& e)
    {
        std::string msg = DAS_FMT_NS::format(
            "sync_wait failed for Welcome: {}",
            e.what());
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_TIMEOUT;
    }
}

DasResult HostLauncher::SendHandshakeReady(uint16_t session_id)
{
    if (!impl_->async_transport)
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
    // 控制平面消息: ObjectId = {0, 0, 0}
    header.session_id = 0;
    header.generation = 0;
    header.local_id = 0;

    auto validated_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::REQUEST)
            .SetControlPlaneCommand(HandshakeInterfaceId::HANDSHAKE_IFACE_READY)
            .SetBodySize(sizeof(ready))
            .SetCallId(header.call_id)
            .Build();

    try
    {
        auto send_result_opt = stdexec::sync_wait(
            impl_->async_transport->Send(
                validated_header,
                reinterpret_cast<const uint8_t*>(&ready),
                sizeof(ready)));

        if (!send_result_opt.has_value())
        {
            DAS_LOG_ERROR("Failed to send Ready: sync_wait returned empty");
            return DAS_E_IPC_SEND_FAILED;
        }

        auto&& [ec, result] = std::move(*send_result_opt);
        if (ec)
        {
            try
            {
                std::rethrow_exception(ec);
            }
            catch (const std::exception& e)
            {
                std::string msg = DAS_FMT_NS::format(
                    "Failed to send Ready: {}",
                    e.what());
                DAS_LOG_ERROR(msg.c_str());
            }
            return DAS_E_IPC_SEND_FAILED;
        }

        if (result != DAS_S_OK)
        {
            std::string msg = DAS_FMT_NS::format(
                "Failed to send Ready: error={}",
                result);
            DAS_LOG_ERROR(msg.c_str());
            return result;
        }
    }
    catch (const std::exception& e)
    {
        std::string msg = DAS_FMT_NS::format(
            "sync_wait failed for Ready: {}",
            e.what());
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_SEND_FAILED;
    }

    std::string info_msg =
        DAS_FMT_NS::format("Sent Ready: session_id={}", session_id);
    DAS_LOG_INFO(info_msg.c_str());
    return DAS_S_OK;
}

DasResult HostLauncher::ReceiveHandshakeReadyAck(uint32_t timeout_ms)
{
    if (!impl_->async_transport)
    {
        DAS_LOG_ERROR("Transport not initialized");
        return DAS_E_IPC_NOT_INITIALIZED;
    }

    // 注意：当前实现没有真正的超时机制，timeout_ms 暂时忽略
    // TODO: 后续使用 stdexec::timeout 算法添加超时支持
    (void)timeout_ms;

    try
    {
        auto receive_result_opt = stdexec::sync_wait(
            impl_->async_transport->Receive());

        if (!receive_result_opt.has_value())
        {
            DAS_LOG_ERROR("Failed to receive ReadyAck: sync_wait returned empty");
            return DAS_E_IPC_RECEIVE_FAILED;
        }

        auto&& [ec, result_variant] = std::move(*receive_result_opt);

        if (ec)
        {
            try
            {
                std::rethrow_exception(ec);
            }
            catch (const std::exception& e)
            {
                std::string msg = DAS_FMT_NS::format(
                    "Failed to receive ReadyAck: {}",
                    e.what());
                DAS_LOG_ERROR(msg.c_str());
            }
            return DAS_E_IPC_RECEIVE_FAILED;
        }

        if (result_variant.index() == 0)
        {
            DasResult error_code = std::get<0>(result_variant);
            std::string msg = DAS_FMT_NS::format(
                "Failed to receive ReadyAck: error={}",
                error_code);
            DAS_LOG_ERROR(msg.c_str());
            return error_code;
        }

        auto&& [header, body] = std::get<1>(result_variant);

        const IPCMessageHeader& raw_header = header.Raw();

        if (raw_header.interface_id
            != static_cast<uint32_t>(
                HandshakeInterfaceId::HANDSHAKE_IFACE_READY_ACK))
        {
            std::string msg = DAS_FMT_NS::format(
                "Unexpected interface_id: {}",
                raw_header.interface_id);
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
            std::string msg = DAS_FMT_NS::format(
                "ReadyAck status error: {}",
                ack.status);
            DAS_LOG_ERROR(msg.c_str());
            return DAS_E_IPC_HANDSHAKE_FAILED;
        }

        std::string info_msg =
            DAS_FMT_NS::format("Received ReadyAck: status={}", ack.status);
        DAS_LOG_INFO(info_msg.c_str());

        return DAS_S_OK;
    }
    catch (const std::exception& e)
    {
        std::string msg = DAS_FMT_NS::format(
            "sync_wait failed for ReadyAck: {}",
            e.what());
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_TIMEOUT;
    }
}
DAS_CORE_IPC_NS_END
