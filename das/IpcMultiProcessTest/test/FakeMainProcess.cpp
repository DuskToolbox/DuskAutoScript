/**
 * @file FakeMainProcess.cpp
 * @brief 假主进程实现 - 用于测试 Host 的父进程监控功能
 *
 * 支持完整握手模式：
 * 1. 等待测试框架通过共享内存传递 host_pid
 * 2. 创建正确命名的管道 (das_ipc_{main_pid}_{host_pid}_m2h/h2m)
 * 3. 等待 DasHost 连接并完成握手
 * 4. 通知测试框架握手完成
 * 5. 继续运行等待被杀（测试父进程监控）
 */

#include "FakeMainProcess.h"

#include <das/Core/IPC/DefaultAsyncIpcTransport.h>
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/Host/HostConfig.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Utils/fmt.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_future.hpp>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace FakeMainProcess
{

    /**
     * @brief RAII 包装类，管理 io_context 和工作线程的生命周期
     *
     * 构造时创建 io_context、work_guard 和工作线程
     * 析构时自动清理 work_guard、停止 io_context 并等待线程结束
     */
    class IoThreadGuard
    {
    public:
        IoThreadGuard() : work_guard_(boost::asio::make_work_guard(io_ctx_))
        {
            io_thread_ = std::thread([this]() { io_ctx_.run(); });
        }

        ~IoThreadGuard()
        {
            if (io_thread_.joinable())
            {
                work_guard_.reset();
                io_ctx_.stop();
                io_thread_.join();
            }
        }

        // 禁止拷贝
        IoThreadGuard(const IoThreadGuard&) = delete;
        IoThreadGuard& operator=(const IoThreadGuard&) = delete;

        boost::asio::io_context& context() { return io_ctx_; }

    private:
        boost::asio::io_context                        io_ctx_;
        decltype(boost::asio::make_work_guard(
            std::declval<boost::asio::io_context&>())) work_guard_;
        std::thread                                    io_thread_;
    };

    std::string GenerateUniqueSignalName()
    {
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch())
                      .count();

#ifdef _WIN32
        uint32_t pid = GetCurrentProcessId();
#else
        uint32_t pid = getpid();
#endif

        return DAS_FMT_NS::format("das_fake_main_{}_{}", pid, ms);
    }

    // ====== FakeMainReadySignal 实现 ======

    FakeMainReadySignal::FakeMainReadySignal(const std::string& signal_name)
        : signal_name_(signal_name)
    {
        mutex_ = std::make_unique<boost::interprocess::named_mutex>(
            boost::interprocess::open_or_create,
            signal_name_.c_str());
    }

    FakeMainReadySignal::~FakeMainReadySignal()
    {
        try
        {
            boost::interprocess::named_mutex::remove(signal_name_.c_str());
        }
        catch (...)
        {
        }
    }

    void FakeMainReadySignal::AcquireAndRelease()
    {
        // 等待获取锁（Test 持有），获取后立即释放（表示就绪）
        mutex_->lock();
        mutex_->unlock();
    }

    void FakeMainReadySignal::Cleanup(const std::string& signal_name)
    {
        try
        {
            boost::interprocess::named_mutex::remove(signal_name.c_str());
        }
        catch (...)
        {
        }
    }

    // ====== SignalHolder 实现 ======

    SignalHolder::SignalHolder(const std::string& signal_name)
        : signal_name_(signal_name), holds_lock_(false)
    {
        // 先清理可能残留的旧锁
        try
        {
            boost::interprocess::named_mutex::remove(signal_name_.c_str());
        }
        catch (...)
        {
        }

        mutex_ = std::make_unique<boost::interprocess::named_mutex>(
            boost::interprocess::open_or_create,
            signal_name_.c_str());

        // 获取并持有锁
        mutex_->lock();
        holds_lock_ = true;
    }

    SignalHolder::~SignalHolder()
    {
        if (holds_lock_)
        {
            try
            {
                mutex_->unlock();
            }
            catch (...)
            {
            }
        }

        try
        {
            boost::interprocess::named_mutex::remove(signal_name_.c_str());
        }
        catch (...)
        {
        }
    }

    bool SignalHolder::ReleaseAndWait(std::chrono::milliseconds timeout_ms)
    {
        // 释放锁，让 FakeMain 可以获取
        if (holds_lock_)
        {
            mutex_->unlock();
            holds_lock_ = false;
        }

        auto deadline = std::chrono::steady_clock::now() + timeout_ms;

        // 等待能够获取锁（表示 FakeMain 已经获取并释放了）
        while (std::chrono::steady_clock::now() < deadline)
        {
            try
            {
                if (mutex_->try_lock())
                {
                    mutex_->unlock();
                    return true;
                }
            }
            catch (...)
            {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

    // ====== KillParentSharedMemory 实现 ======

    KillParentSharedMemory::KillParentSharedMemory(const std::string& shm_name)
        : shm_name_(shm_name), shm_(nullptr), data_(nullptr)
    {
        // 先清理可能存在的旧共享内存
        try
        {
            boost::interprocess::shared_memory_object::remove(
                shm_name_.c_str());
        }
        catch (...)
        {
        }

        // 创建共享内存
        try
        {
            shm_ = new boost::interprocess::managed_shared_memory(
                boost::interprocess::open_or_create,
                shm_name_.c_str(),
                4096);

            // 创建数据对象
            data_ = shm_->construct<KillParentSharedData>("KillParentData")();
            data_->host_pid = 0;
            data_->host_pid_ready = 0;
            data_->handshake_done = 0;
            data_->reserved = 0;

            // 创建同步互斥锁
            std::string mutex_name = shm_name_ + "_mutex";
            mutex_ = std::make_unique<boost::interprocess::named_mutex>(
                boost::interprocess::open_or_create,
                mutex_name.c_str());
        }
        catch (...)
        {
            delete shm_;
            shm_ = nullptr;
            throw;
        }
    }

    KillParentSharedMemory::~KillParentSharedMemory()
    {
        if (shm_)
        {
            try
            {
                shm_->destroy<KillParentSharedData>("KillParentData");
            }
            catch (...)
            {
            }
            delete shm_;

            try
            {
                boost::interprocess::shared_memory_object::remove(
                    shm_name_.c_str());
            }
            catch (...)
            {
            }

            try
            {
                std::string mutex_name = shm_name_ + "_mutex";
                boost::interprocess::named_mutex::remove(mutex_name.c_str());
            }
            catch (...)
            {
            }
        }
    }

    uint32_t KillParentSharedMemory::WaitForHostPid(
        std::chrono::milliseconds timeout_ms)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout_ms;

        while (std::chrono::steady_clock::now() < deadline)
        {
            if (data_->host_pid_ready == 1)
            {
                return data_->host_pid;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return 0;
    }

    void KillParentSharedMemory::SetHandshakeDone()
    {
        data_->handshake_done = 1;
    }

    void KillParentSharedMemory::WriteHostPid(
        const std::string& shm_name,
        uint32_t           host_pid)
    {
        // 打开已存在的共享内存
        boost::interprocess::managed_shared_memory shm(
            boost::interprocess::open_only,
            shm_name.c_str());

        auto* data = shm.find<KillParentSharedData>("KillParentData").first;
        if (data)
        {
            data->host_pid = host_pid;
            data->host_pid_ready = 1;
        }
    }

    bool KillParentSharedMemory::WaitForHandshakeDone(
        const std::string&        shm_name,
        std::chrono::milliseconds timeout_ms)
    {
        // 打开已存在的共享内存
        boost::interprocess::managed_shared_memory shm(
            boost::interprocess::open_only,
            shm_name.c_str());

        auto* data = shm.find<KillParentSharedData>("KillParentData").first;
        if (!data)
        {
            return false;
        }

        auto deadline = std::chrono::steady_clock::now() + timeout_ms;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (data->handshake_done == 1)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

    bool KillParentSharedMemory::WaitForSharedMemoryReady(
        const std::string&        shm_name,
        std::chrono::milliseconds timeout_ms)
    {
        auto deadline = std::chrono::steady_clock::now() + timeout_ms;

        while (std::chrono::steady_clock::now() < deadline)
        {
            try
            {
                boost::interprocess::managed_shared_memory shm(
                    boost::interprocess::open_only,
                    shm_name.c_str());

                auto* data =
                    shm.find<KillParentSharedData>("KillParentData").first;
                if (data != nullptr)
                {
                    return true;
                }
            }
            catch (...)
            {
                // 共享内存还不存在，继续等待
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

    void KillParentSharedMemory::Cleanup(const std::string& shm_name)
    {
        try
        {
            boost::interprocess::shared_memory_object::remove(shm_name.c_str());
        }
        catch (...)
        {
        }

        try
        {
            std::string mutex_name = shm_name + "_mutex";
            boost::interprocess::named_mutex::remove(mutex_name.c_str());
        }
        catch (...)
        {
        }
    }

    // ====== RunFakeMainProcessMode 实现 ======

    int RunFakeMainProcessMode(const std::string& signal_name)
    {
#ifdef _WIN32
        uint32_t main_pid = GetCurrentProcessId();
#else
        uint32_t main_pid = getpid();
#endif

        DAS_LOG_INFO(
            DAS_FMT_NS::format(
                "[FakeMain] Starting with signal: {}, main_pid: {}",
                signal_name,
                main_pid)
                .c_str());

        // 1. 创建共享内存（用于接收 host_pid）
        std::unique_ptr<KillParentSharedMemory> shm;
        try
        {
            shm =
                std::make_unique<KillParentSharedMemory>(KILL_PARENT_SHM_NAME);
        }
        catch (const std::exception& e)
        {
            DAS_LOG_ERROR(
                DAS_FMT_NS::format(
                    "[FakeMain] Failed to create shared memory: {}",
                    e.what())
                    .c_str());
            return 1;
        }

        // 2. 创建 IPC 上下文（作为主进程）
        auto ctx = DAS::Core::IPC::MainProcess::CreateIpcContextShared();
        if (!ctx)
        {
            DAS_LOG_ERROR("[FakeMain] Failed to create IPC context");
            return 1;
        }

        DAS_LOG_INFO("[FakeMain] IPC context created");

        // 3. 先等待测试框架写入 host_pid（需要 host_pid 来构造管道名）
        // 注意：此时还没有发送 ready 信号，Test 框架会等待
        uint32_t host_pid = shm->WaitForHostPid(std::chrono::seconds(30));
        if (host_pid == 0)
        {
            DAS_LOG_ERROR("[FakeMain] Timeout waiting for host_pid");
            return 1;
        }

        DAS_LOG_INFO(
            DAS_FMT_NS::format("[FakeMain] Received host_pid: {}", host_pid)
                .c_str());

        // 4. 创建管道（MainProcess 是服务端）
        std::string m2h_pipe = DAS::Core::IPC::Host::MakeMessageQueueName(
            main_pid,
            host_pid,
            true); // main_to_host
        std::string h2m_pipe = DAS::Core::IPC::Host::MakeMessageQueueName(
            main_pid,
            host_pid,
            false); // host_to_main

        DAS_LOG_INFO(
            DAS_FMT_NS::format(
                "[FakeMain] Creating pipes: m2h={}, h2m={}",
                m2h_pipe,
                h2m_pipe)
                .c_str());

        // 5. 创建 io_context 和工作线程（RAII 管理）
        IoThreadGuard                                             io_guard;
        std::unique_ptr<DAS::Core::IPC::DefaultAsyncIpcTransport> transport;

        try
        {
            // 注意：CreateAsync 参数顺序是 (read_endpoint, write_endpoint)
            // read_endpoint = h2m_pipe (Main 读取 Host 发送的数据)
            // write_endpoint = m2h_pipe (Main 写入数据发送给 Host)
            // 这与 HostLauncher::ConnectToHost() 的参数顺序一致
            auto future = boost::asio::co_spawn(
                io_guard.context(),
                DAS::Core::IPC::DefaultAsyncIpcTransport::CreateAsync(
                    io_guard.context(),
                    h2m_pipe,
                    m2h_pipe,
                    true, // is_server = true
                    65536),
                boost::asio::use_future);

            auto result = future.get();

            if (!result.has_value())
            {
                DAS_LOG_ERROR(
                    DAS_FMT_NS::format(
                        "[FakeMain] Failed to create transport: error={}",
                        result.error())
                        .c_str());
                return 1;
            }

            transport = std::move(*result);
            DAS_LOG_INFO("[FakeMain] Pipes created successfully");
        }
        catch (const std::exception& e)
        {
            DAS_LOG_ERROR(
                DAS_FMT_NS::format(
                    "[FakeMain] Exception creating transport: {}",
                    e.what())
                    .c_str());
            return 1;
        }

        // 6. 管道已创建，发送 ready 信号通知测试框架可以启动 DasHost
        {
            FakeMainReadySignal ready_signal(signal_name);
            DAS_LOG_INFO("[FakeMain] Waiting for test to release lock...");
            ready_signal.AcquireAndRelease();
            DAS_LOG_INFO(
                "[FakeMain] Ready signal sent, DasHost can connect now");
        }

        // 8. 完成握手流程（MainProcess 主动发起）
        // 正确流程：MainProcess 发送 HELLO → Host 响应 WELCOME
        //          → Host 发送 READY → MainProcess 响应 READY_ACK
        bool handshake_complete = false;

        try
        {
            // 8.1 先启动接收操作（避免丢失响应）
            auto recv_future = boost::asio::co_spawn(
                io_guard.context(),
                transport->ReceiveCoroutine(),
                boost::asio::use_future);

            // 8.2 发送 HELLO（分配 session_id = 1）
            DAS::Core::IPC::HelloRequestV1 hello;
            DAS::Core::IPC::InitHelloRequest(
                hello,
                host_pid,
                "FakeMainProcess");

            // 设置分配的 session_id（模拟 MainProcess 分配）
            hello.assigned_session_id = 1;

            auto hello_header =
                DAS::Core::IPC::IPCMessageHeaderBuilder()
                    .SetMessageType(DAS::Core::IPC::MessageType::REQUEST)
                    .SetControlPlaneCommand(
                        DAS::Core::IPC::HandshakeInterfaceId::
                            HANDSHAKE_IFACE_HELLO)
                    .SetBodySize(sizeof(hello))
                    .SetCallId(1)
                    .Build();

            DAS_LOG_INFO(
                DAS_FMT_NS::format(
                    "[FakeMain] Sending HELLO: pid={}, session_id={}",
                    hello.pid,
                    hello.assigned_session_id)
                    .c_str());

            auto send_future = boost::asio::co_spawn(
                io_guard.context(),
                transport->SendCoroutine(
                    hello_header,
                    reinterpret_cast<const uint8_t*>(&hello),
                    sizeof(hello)),
                boost::asio::use_future);

            auto send_result = send_future.get();
            if (send_result != DAS_S_OK)
            {
                DAS_LOG_ERROR(
                    DAS_FMT_NS::format(
                        "[FakeMain] Failed to send HELLO: {}",
                        send_result)
                        .c_str());
                return 1;
            }

            DAS_LOG_INFO("[FakeMain] Sent HELLO, waiting for WELCOME...");

            // 8.3 等待接收 WELCOME（接收操作已在前面启动）
            auto recv_result = recv_future.get();
            if (recv_result.index() == 0)
            {
                DAS_LOG_ERROR("[FakeMain] Failed to receive WELCOME");
                return 1;
            }

            auto&& [welcome_header, welcome_body] = std::get<1>(recv_result);
            DAS_LOG_INFO(
                DAS_FMT_NS::format(
                    "[FakeMain] Received message: interface_id={}",
                    welcome_header.Raw().interface_id)
                    .c_str());

            // 验证是 WELCOME 消息
            if (welcome_header.Raw().interface_id
                != static_cast<uint32_t>(DAS::Core::IPC::HandshakeInterfaceId::
                                             HANDSHAKE_IFACE_WELCOME))
            {
                DAS_LOG_ERROR("[FakeMain] Expected WELCOME message");
                return 1;
            }

            // 解析 WELCOME 响应
            if (welcome_body.size() < sizeof(DAS::Core::IPC::WelcomeResponseV1))
            {
                DAS_LOG_ERROR("[FakeMain] WELCOME body too small");
                return 1;
            }

            auto* welcome =
                reinterpret_cast<const DAS::Core::IPC::WelcomeResponseV1*>(
                    welcome_body.data());

            DAS_LOG_INFO(
                DAS_FMT_NS::format(
                    "[FakeMain] Received WELCOME: session_id={}, status={}",
                    welcome->session_id,
                    welcome->status)
                    .c_str());

            if (welcome->status
                != DAS::Core::IPC::WelcomeResponseV1::STATUS_SUCCESS)
            {
                DAS_LOG_ERROR("[FakeMain] WELCOME status indicates failure");
                return 1;
            }

            // 8.3 发送 READY（MainProcess 主动发送）
            DAS::Core::IPC::ReadyRequestV1 ready;
            DAS::Core::IPC::InitReadyRequest(ready, welcome->session_id);

            auto ready_header =
                DAS::Core::IPC::IPCMessageHeaderBuilder()
                    .SetMessageType(DAS::Core::IPC::MessageType::REQUEST)
                    .SetControlPlaneCommand(
                        DAS::Core::IPC::HandshakeInterfaceId::
                            HANDSHAKE_IFACE_READY)
                    .SetBodySize(sizeof(ready))
                    .SetCallId(2)
                    .Build();

            DAS_LOG_INFO(
                DAS_FMT_NS::format(
                    "[FakeMain] Sending READY: session_id={}",
                    ready.session_id)
                    .c_str());

            send_future = boost::asio::co_spawn(
                io_guard.context(),
                transport->SendCoroutine(
                    ready_header,
                    reinterpret_cast<const uint8_t*>(&ready),
                    sizeof(ready)),
                boost::asio::use_future);

            send_result = send_future.get();
            if (send_result != DAS_S_OK)
            {
                DAS_LOG_ERROR(
                    DAS_FMT_NS::format(
                        "[FakeMain] Failed to send READY: {}",
                        send_result)
                        .c_str());
                return 1;
            }

            DAS_LOG_INFO("[FakeMain] Sent READY, waiting for READY_ACK...");

            // 8.4 接收 READY_ACK
            recv_future = boost::asio::co_spawn(
                io_guard.context(),
                transport->ReceiveCoroutine(),
                boost::asio::use_future);

            recv_result = recv_future.get();
            if (recv_result.index() == 0)
            {
                DAS_LOG_ERROR("[FakeMain] Failed to receive READY_ACK");
                return 1;
            }

            auto&& [ack_header, ack_body] = std::get<1>(recv_result);

            if (ack_header.Raw().interface_id
                != static_cast<uint32_t>(DAS::Core::IPC::HandshakeInterfaceId::
                                             HANDSHAKE_IFACE_READY_ACK))
            {
                DAS_LOG_ERROR("[FakeMain] Expected READY_ACK message");
                return 1;
            }

            if (ack_body.size() < sizeof(DAS::Core::IPC::ReadyAckV1))
            {
                DAS_LOG_ERROR("[FakeMain] READY_ACK body too small");
                return 1;
            }

            auto* ready_ack =
                reinterpret_cast<const DAS::Core::IPC::ReadyAckV1*>(
                    ack_body.data());

            DAS_LOG_INFO(
                DAS_FMT_NS::format(
                    "[FakeMain] Received READY_ACK: status={}",
                    ready_ack->status)
                    .c_str());

            if (ready_ack->status != DAS::Core::IPC::ReadyAckV1::STATUS_SUCCESS)
            {
                DAS_LOG_ERROR("[FakeMain] READY_ACK status indicates failure");
                return 1;
            }

            DAS_LOG_INFO("[FakeMain] Handshake complete!");

            handshake_complete = true;

            // 9. 通知测试框架握手完成
            shm->SetHandshakeDone();
        }
        catch (const std::exception& e)
        {
            DAS_LOG_ERROR(
                DAS_FMT_NS::format(
                    "[FakeMain] Exception during handshake: {}",
                    e.what())
                    .c_str());
            return 1;
        }

        if (!handshake_complete)
        {
            DAS_LOG_ERROR("[FakeMain] Handshake failed");
            return 1;
        }

        // 10. 继续运行，等待测试框架杀掉我们
        DAS_LOG_INFO("[FakeMain] Running, waiting to be killed by test...");

        // 简单地休眠等待被杀
        for (int i = 0; i < 300; ++i) // 最多等待 30 秒
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        DAS_LOG_INFO("[FakeMain] Exiting (timeout or not killed)");
        return 0;
    }

} // namespace FakeMainProcess
