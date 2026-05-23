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
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/IDasAsyncHandshakeOperation.h>
#include <das/IDasAsyncOperation.h>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>
#include <optional>
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

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/interprocess/exceptions.hpp>
#include <boost/interprocess/ipc/message_queue.hpp>
#include <boost/process/v2/pid.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/system/error_code.hpp>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <cassert>
#include <chrono>
#include <filesystem>
#include <limits>
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
    std::optional<AnyTransport>                  async_transport;
    uint32_t                                     pid = 0;
    uint16_t                                     session_id = 0;
    uint16_t              next_call_id = 1; // V3: 16-bit call_id
    bool                  is_running = false;
    std::atomic<uint32_t> ref_count{1}; // 引用计数，初始为 1（创建时持有）
};

namespace
{

    /**
     * @brief IDasAsyncHandshakeOperation 的 COM 实现类
     *
     * 用于 StartAsync 异步操作的返回对象。
     * 调用方通过此对象查询异步状态和结果。
     */
    class HandshakeAsyncOperationImpl final : public IDasAsyncHandshakeOperation
    {
    public:
        HandshakeAsyncOperationImpl(
            HostLauncher&                      launcher,
            std::shared_ptr<std::atomic<bool>> alive)
            : launcher_(launcher), alive_(std::move(alive))
        {
        }

        ~HandshakeAsyncOperationImpl() { *alive_ = false; }

        HandshakeAsyncOperationImpl(const HandshakeAsyncOperationImpl&) =
            delete;
        HandshakeAsyncOperationImpl& operator=(
            const HandshakeAsyncOperationImpl&) = delete;

        //=========================================================================
        // IDasBase
        //=========================================================================
        uint32_t AddRef() override { return ++ref_count_; }

        uint32_t Release() override
        {
            auto r = --ref_count_;
            if (r == 0)
                delete this;
            return r;
        }

        DasResult QueryInterface(const DasGuid& iid, void** pp) override
        {
            if (iid == DasIidOf<IDasAsyncHandshakeOperation>()
                || iid == DasIidOf<IDasAsyncOperation>()
                || iid == DasIidOf<IDasBase>())
            {
                AddRef();
                *pp = static_cast<IDasAsyncHandshakeOperation*>(this);
                return DAS_S_OK;
            }
            return DAS_E_NO_INTERFACE;
        }

        //=========================================================================
        // IDasAsyncOperation
        //=========================================================================
        int32_t GetStatus() override
        {
            return status_.load(std::memory_order_acquire);
        }

        DasResult SetCompleted(IDasAsyncCompletedHandler* p_handler) override
        {
            handler_ = p_handler;
            // 如果已完成，立即回调（与 AsyncOperationImpl 一致）
            if (status_.load(std::memory_order_acquire) != DAS_ASYNC_STARTED)
            {
                if (handler_)
                    handler_->OnCompleted(this, status_.load());
            }
            return DAS_S_OK;
        }

        DasResult Cancel() override
        {
            int32_t expected = DAS_ASYNC_STARTED;
            if (status_.compare_exchange_strong(
                    expected,
                    DAS_ASYNC_CANCELED,
                    std::memory_order_acq_rel))
            {
                launcher_.Stop();
                return DAS_S_OK;
            }
            return DAS_E_IPC_INVALID_STATE;
        }

        //=========================================================================
        // IDasAsyncHandshakeOperation
        //=========================================================================
        DasResult GetResults(uint16_t* p_out_session_id) override
        {
            if (status_.load(std::memory_order_acquire) != DAS_ASYNC_COMPLETED)
                return DAS_E_IPC_INVALID_STATE;
            *p_out_session_id = session_id_;
            return result_;
        }

        //=========================================================================
        // 内部完成方法
        //=========================================================================
        void Complete(DasResult result, uint16_t sid)
        {
            session_id_ = sid;
            result_ = result;

            int32_t new_status =
                DAS::IsOk(result) ? DAS_ASYNC_COMPLETED : DAS_ASYNC_FAILED;
            status_.store(new_status, std::memory_order_release);

            if (handler_)
                handler_->OnCompleted(this, new_status);
        }

        [[nodiscard]]
        bool IsAlive() const
        {
            return alive_->load(std::memory_order_acquire);
        }

    private:
        HostLauncher&                      launcher_;
        std::shared_ptr<std::atomic<bool>> alive_;

        std::atomic<uint32_t>             ref_count_{1};
        std::atomic<int32_t>              status_{DAS_ASYNC_STARTED};
        DasResult                         result_{DAS_E_UNDEFINED_RETURN_VALUE};
        uint16_t                          session_id_{0};
        DasPtr<IDasAsyncCompletedHandler> handler_;
    };

    void CleanupTransport(std::optional<AnyTransport>& transport)
    {
        if (!transport)
        {
            return;
        }

        transport->Cleanup();
        transport.reset();
    }

    DasResult ReadUtf8String(
        IDasReadOnlyString* p_string,
        std::string&        out_value)
    {
        if (!p_string)
        {
            return DAS_E_INVALID_ARGUMENT;
        }

        const char* p_utf8 = nullptr;
        const auto  result = p_string->GetUtf8(&p_utf8);
        if (DAS::IsFailed(result) || !p_utf8)
        {
            return DAS_E_INVALID_ARGUMENT;
        }

        out_value = p_utf8;
        return DAS_S_OK;
    }

    std::string DefaultWorkingDirectoryFor(const std::string& exe_path)
    {
        const auto parent = std::filesystem::path(exe_path).parent_path();
        if (!parent.empty())
        {
            return parent.string();
        }
        return std::filesystem::current_path().string();
    }

} // anonymous namespace

HostLauncher::HostLauncher(
    boost::asio::io_context& io_ctx,
    uint16_t                 session_id,
    OnRegisterCallback       on_register)
    : impl_(std::make_unique<Impl>(io_ctx)), session_id_(session_id),
      on_register_(std::move(on_register))
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
        DAS_CORE_LOG_ERROR(msg.c_str());
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

    return StartLaunchSequence(
        host_exe_path,
        args,
        std::nullopt,
        out_session_id,
        timeout_ms);
}

DasResult HostLauncher::StartWithDesc(
    const HostLaunchDesc* p_desc,
    uint32_t              timeout_ms,
    uint16_t*             p_out_session_id)
{
    if (!p_desc || !p_out_session_id || !p_desc->p_executable_path)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    if (p_desc->arg_count > 0 && !p_desc->pp_args)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    std::string exe_path;
    auto        result = ReadUtf8String(p_desc->p_executable_path, exe_path);
    if (DAS::IsFailed(result) || exe_path.empty())
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    std::vector<std::string> args;
    args.reserve(p_desc->arg_count);
    for (size_t i = 0; i < p_desc->arg_count; ++i)
    {
        if (!p_desc->pp_args[i])
        {
            return DAS_E_INVALID_ARGUMENT;
        }

        std::string arg;
        result = ReadUtf8String(p_desc->pp_args[i], arg);
        if (DAS::IsFailed(result))
        {
            return DAS_E_INVALID_ARGUMENT;
        }
        args.push_back(std::move(arg));
    }

    std::optional<std::string> working_directory;
    if (p_desc->p_working_directory)
    {
        std::string value;
        result = ReadUtf8String(p_desc->p_working_directory, value);
        if (DAS::IsFailed(result) || value.empty())
        {
            return DAS_E_INVALID_ARGUMENT;
        }
        working_directory = std::move(value);
    }

    return StartLaunchSequence(
        exe_path,
        args,
        working_directory,
        *p_out_session_id,
        timeout_ms);
}

DasResult HostLauncher::StartLaunchSequence(
    const std::string&              exe_path,
    const std::vector<std::string>& args,
    const std::optional<std::string>& working_directory,
    uint16_t&                       out_session_id,
    uint32_t                        timeout_ms)
{
    DasResult result = LaunchProcess(exe_path, args, working_directory);
    if (result != DAS_S_OK)
    {
        return result;
    }

    // 先创建管道并等待 Host 连接（MainProcess 是服务端）
    result = ConnectToHost(timeout_ms);
    if (result != DAS_S_OK)
    {
        Stop();
        return result;
    }

    // 然后等待 Host IPC 资源就绪（管道已创建，Host 已连接）
    result = WaitForHostReady(timeout_ms);
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

    // IpcContext 通过 on_register 回调自行注册
    if (on_register_)
    {
        DasResult reg_result = on_register_();
        if (reg_result != DAS_S_OK)
        {
            DAS_CORE_LOG_ERROR(
                "Registration callback failed after Start: error={}",
                reg_result);
            Stop();
            return reg_result;
        }
    }

    std::string msg = DAS_FMT_NS::format(
        "Host process started successfully: PID={}, session_id={}",
        impl_->pid,
        out_session_id);
    DAS_LOG_INFO(msg.c_str());

    // 异步监控 Host 进程退出，触发回调通知 PluginManager 清理索引
    if (impl_->process && on_process_exit_)
    {
        auto callback = on_process_exit_;
        auto session_id = session_id_;
        auto process_ptr = impl_->process.get();

        boost::asio::co_spawn(
            impl_->io_ctx,
            [process_ptr,
             callback,
             session_id]() -> boost::asio::awaitable<void>
            {
                boost::system::error_code ec;
                co_await process_ptr->async_wait(
                    boost::asio::redirect_error(
                        boost::asio::use_awaitable,
                        ec));
                int exit_code = ec ? -1 : process_ptr->exit_code();
                if (callback)
                {
                    callback(session_id, exit_code);
                }
                co_return;
            },
            boost::asio::detached);
    }

    return DAS_S_OK;
}

void HostLauncher::Stop()
{
    // Stop() 使用 co_spawn + use_future 阻塞当前线程等待 io_context 完成操作，
    // 因此不能从 io_context 线程调用，否则会死锁。
    assert(
        !impl_->io_ctx.get_executor().running_in_this_thread()
        && "Stop() must not be called from the io_context thread");

    // 清除进程退出回调，避免 Stop() 主动关闭进程时触发回调
    on_process_exit_ = nullptr;

    // 先发送 GOODBYE 消息让 Host 进程优雅退出
    if (impl_->async_transport && impl_->is_running)
    {
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

        // 使用 co_spawn + use_future 发送 GOODBYE
        try
        {
            auto future = boost::asio::co_spawn(
                impl_->io_ctx,
                impl_->async_transport->SendCoroutine(
                    validated_header,
                    reinterpret_cast<const uint8_t*>(&goodbye),
                    sizeof(goodbye)),
                boost::asio::use_future);

            DasResult result = future.get();
            if (result != DAS_S_OK)
            {
                std::string err_msg = DAS_FMT_NS::format(
                    "Failed to send GOODBYE: error={}",
                    result);
                DAS_CORE_LOG_ERROR(err_msg.c_str());
            }
        }
        catch (const boost::system::system_error& e)
        {
            std::string err_msg = DAS_FMT_NS::format(
                "Send GOODBYE failed with system_error: {}",
                ToString(e.what()));
            DAS_CORE_LOG_ERROR(err_msg.c_str());
        }
        catch (const std::exception& e)
        {
            std::string err_msg = DAS_FMT_NS::format(
                "Send GOODBYE failed: {}",
                ToString(e.what()));
            DAS_CORE_LOG_ERROR(err_msg.c_str());
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

    CleanupTransport(impl_->async_transport);

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

boost::asio::io_context& HostLauncher::GetIoContext() { return impl_->io_ctx; }

TransportLookupResult HostLauncher::GetTransport()
{
    if (!impl_->async_transport)
    {
        return {DAS_E_IPC_NOT_INITIALIZED, std::nullopt};
    }

    if (!impl_->async_transport->IsConnected())
    {
        return {DAS_E_IPC_CONNECTION_LOST, std::nullopt};
    }

    return {DAS_S_OK, std::ref(*impl_->async_transport)};
}

DasResult HostLauncher::StartAsync(
    const std::string&            host_exe_path,
    IDasAsyncHandshakeOperation** pp_out_operation)
{
    if (!pp_out_operation)
    {
        return DAS_E_INVALID_ARGUMENT;
    }

    if (!std::filesystem::exists(host_exe_path))
    {
        std::string msg =
            DAS_FMT_NS::format("Host executable not found: {}", host_exe_path);
        DAS_CORE_LOG_ERROR(msg.c_str());
        return DAS_E_INVALID_ARGUMENT;
    }

    if (IsRunning())
    {
        DAS_CORE_LOG_ERROR("Host process already running");
        return DAS_E_IPC_INVALID_STATE;
    }

    constexpr uint32_t DEFAULT_TIMEOUT_MS = 10000;
    auto               alive = std::make_shared<std::atomic<bool>>(true);
    auto*              op = new HandshakeAsyncOperationImpl(*this, alive);

    boost::asio::co_spawn(
        impl_->io_ctx,
        RunAsync(op, alive, host_exe_path, DEFAULT_TIMEOUT_MS),
        boost::asio::detached);

    *pp_out_operation = op;
    return DAS_S_OK;
}

void HostLauncher::SetOnProcessExit(OnProcessExitCallback callback)
{
    on_process_exit_ = std::move(callback);
}

void HostLauncher::SetOnHeartbeatTimeout(OnHeartbeatTimeoutCallback callback)
{
    on_heartbeat_timeout_ = std::move(callback);
}

void HostLauncher::SetAssociatedGuid(DasGuid guid) { associated_guid_ = guid; }

DasGuid HostLauncher::GetAssociatedGuid() const { return associated_guid_; }

void HostLauncher::NotifyHeartbeatTimeout()
{
    if (on_heartbeat_timeout_)
    {
        on_heartbeat_timeout_(associated_guid_);
    }
}

void HostLauncher::TerminateIfRunning()
{
    if (impl_->process)
    {
        boost::system::error_code ec;
        bool                      still_running = impl_->process->running(ec);

        if (still_running)
        {
            DAS_CORE_LOG_INFO(
                "Terminating Host process (heartbeat timeout): pid={}, "
                "session_id={}",
                impl_->pid,
                impl_->session_id);
            impl_->process->terminate(ec);
        }

        // 释放进程句柄所有权
        (void)impl_->process.release();
    }

    CleanupTransport(impl_->async_transport);

    impl_->is_running = false;
    impl_->session_id = 0;
    impl_->pid = 0;
}

uint32_t HostLauncher::AddRef() { return ++impl_->ref_count; }

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
    if (iid == DasIidOf<IInternalHost>())
    {
        AddRef();
        *pp = static_cast<IInternalHost*>(this);
        return DAS_S_OK;
    }

    if (iid == DasIidOf<IDasBase>())
    {
        AddRef();
        *pp = static_cast<IDasBase*>(static_cast<IHostLauncher*>(this));
        return DAS_S_OK;
    }
    return DAS_E_NO_INTERFACE;
}

DasResult HostLauncher::LaunchProcess(
    const std::string&              exe_path,
    const std::vector<std::string>& args,
    const std::optional<std::string>& working_directory)
{
    try
    {
        const auto launch_working_directory =
            working_directory.value_or(DefaultWorkingDirectoryFor(exe_path));

        impl_->process = std::make_unique<boost::process::v2::process>(
            impl_->io_ctx,
            exe_path,
            args,
            boost::process::v2::process_start_dir(
                launch_working_directory));

        impl_->pid = static_cast<uint32_t>(impl_->process->id());
        impl_->is_running = true;

        std::string msg =
            DAS_FMT_NS::format("Host process launched: PID={}", impl_->pid);
        DAS_LOG_INFO(msg.c_str());

        return DAS_S_OK;
    }
    catch (const boost::system::system_error& e)
    {
        std::string msg = DAS_FMT_NS::format(
            "Exception launching Host process (system_error): {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }
    catch (const std::exception& e)
    {
        std::string msg = DAS_FMT_NS::format(
            "Exception launching Host process: {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }
}

DasResult HostLauncher::WaitForHostReady(uint32_t timeout_ms)
{
    auto start = std::chrono::steady_clock::now();

    while (true)
    {
        if (!IsRunning())
        {
            DAS_CORE_LOG_ERROR("Host process terminated unexpectedly");
            return DAS_E_IPC_CONNECTION_LOST;
        }

        // Transport-agnostic check: if ConnectToHost succeeded,
        // the transport is established and Host is connected.
        if (impl_->async_transport)
        {
            DAS_CORE_LOG_INFO("Host IPC transport connected");
            return DAS_S_OK;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);

        if (elapsed.count()
            >= (timeout_ms == 0
                    ? (std::numeric_limits<decltype(timeout_ms)>::max)()
                    : timeout_ms))
        {
            DAS_CORE_LOG_ERROR("Timeout waiting for Host IPC transport");
            return DAS_E_IPC_TIMEOUT;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

DasResult HostLauncher::ConnectToHost(uint32_t timeout_ms)
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

    // MainProcess 是服务端：使用 CreateAsync 异步创建管道
    // 通过 co_spawn + use_future 在 io_context 上运行协程
    try
    {
        auto duration = (timeout_ms == 0)
                            ? std::chrono::milliseconds(
                                  (std::numeric_limits<uint32_t>::max)())
                            : std::chrono::milliseconds(timeout_ms);

        auto future = boost::asio::co_spawn(
            impl_->io_ctx,
            AnyTransport::CreateAsync(
                impl_->io_ctx,
                host_to_plugin_pipe,
                plugin_to_host_pipe,
                true, // is_server = true (MainProcess creates pipes and waits
                      // for Host)
                65536),
            boost::asio::use_future);

        auto status = future.wait_for(duration);
        if (status == std::future_status::timeout)
        {
            DAS_CORE_LOG_ERROR("Timeout creating Host IPC transport");
            return DAS_E_IPC_TIMEOUT;
        }

        auto [transport_result, maybe_transport] = future.get();
        if (DAS::IsFailed(transport_result) || !maybe_transport)
        {
            std::string err_msg = DAS_FMT_NS::format(
                "Failed to create Host IPC transport: error={}",
                transport_result);
            DAS_CORE_LOG_ERROR(err_msg.c_str());
            return transport_result;
        }

        impl_->async_transport = std::move(*maybe_transport);
    }
    catch (const boost::system::system_error& e)
    {
        std::string err_msg = DAS_FMT_NS::format(
            "Failed to create Host IPC transport (system_error): {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(err_msg.c_str());
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }
    catch (const std::exception& e)
    {
        std::string err_msg = DAS_FMT_NS::format(
            "Failed to create Host IPC transport: exception={}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(err_msg.c_str());
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
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
        DAS_CORE_LOG_ERROR("Received invalid session_id (0)");
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

//=============================================================================
// 协程版本的握手方法
//=============================================================================

boost::asio::awaitable<DasResult> HostLauncher::SendHandshakeHelloAsync(
    const std::string& client_name)
{
    if (!impl_->async_transport)
    {
        DAS_CORE_LOG_ERROR("Transport not initialized");
        co_return DAS_E_IPC_NOT_INITIALIZED;
    }

    uint32_t my_pid = static_cast<uint32_t>(GET_CURRENT_PID());

    // session_id 已在构造时预分配
    uint16_t assigned_session_id = session_id_;
    if (assigned_session_id == 0)
    {
        DAS_CORE_LOG_ERROR("Invalid pre-allocated session_id (0)");
        co_return DAS_E_IPC_SESSION_ALLOC_FAILED;
    }

    HelloRequestV1 hello;
    InitHelloRequest(hello, my_pid, client_name.c_str());
    hello.assigned_session_id = assigned_session_id;

    // V3: 16-bit call_id
    uint16_t call_id = impl_->next_call_id++;
    if (impl_->next_call_id == 0)
    {
        impl_->next_call_id = 1; // 溢出后从 1 开始
    }
    auto validated_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::REQUEST)
            .SetControlPlaneCommand(HandshakeInterfaceId::HANDSHAKE_IFACE_HELLO)
            .SetBodySize(sizeof(hello))
            .SetCallId(call_id)
            .Build();

    // 直接使用协程方法，不再用 sync_wait
    DasResult result = co_await impl_->async_transport->SendCoroutine(
        validated_header,
        reinterpret_cast<const uint8_t*>(&hello),
        sizeof(hello));

    if (result != DAS_S_OK)
    {
        std::string msg =
            DAS_FMT_NS::format("Failed to send Hello: error={}", result);
        DAS_CORE_LOG_ERROR(msg.c_str());
        co_return result;
    }

    std::string info_msg =
        DAS_FMT_NS::format("Sent Hello: pid={}, name={}", my_pid, client_name);
    DAS_LOG_INFO(info_msg.c_str());
    co_return DAS_S_OK;
}

boost::asio::awaitable<DasResult> HostLauncher::ReceiveHandshakeWelcomeAsync(
    uint16_t& out_session_id,
    uint32_t  timeout_ms)
{
    (void)timeout_ms; // 暂时忽略超时

    if (!impl_->async_transport)
    {
        DAS_CORE_LOG_ERROR("Transport not initialized");
        co_return DAS_E_IPC_NOT_INITIALIZED;
    }

    // 直接使用协程方法
    auto result_variant = co_await impl_->async_transport->ReceiveCoroutine();

    if (result_variant.index() == 0)
    {
        DasResult   error_code = std::get<0>(result_variant);
        std::string msg = DAS_FMT_NS::format(
            "Failed to receive Welcome: error={}",
            error_code);
        DAS_CORE_LOG_ERROR(msg.c_str());
        co_return error_code;
    }

    auto&& [header, body] = std::get<1>(result_variant);

    const IPCMessageHeader& raw_header = header.Raw();

    if (raw_header.interface_id
        != static_cast<uint32_t>(HandshakeInterfaceId::HANDSHAKE_IFACE_WELCOME))
    {
        std::string msg = DAS_FMT_NS::format(
            "Unexpected interface_id: {}",
            raw_header.interface_id);
        DAS_CORE_LOG_ERROR(msg.c_str());
        co_return DAS_E_IPC_UNEXPECTED_MESSAGE;
    }

    if (body.size() < sizeof(WelcomeResponseV1))
    {
        DAS_CORE_LOG_ERROR("Welcome response body too small");
        co_return DAS_E_IPC_INVALID_MESSAGE_BODY;
    }

    WelcomeResponseV1 welcome =
        *reinterpret_cast<const WelcomeResponseV1*>(body.data());

    if (welcome.status != WelcomeResponseV1::STATUS_SUCCESS)
    {
        std::string msg =
            DAS_FMT_NS::format("Welcome status error: {}", welcome.status);
        DAS_CORE_LOG_ERROR(msg.c_str());
        co_return DAS_E_IPC_HANDSHAKE_FAILED;
    }

    out_session_id = welcome.session_id;

    std::string info_msg = DAS_FMT_NS::format(
        "Received Welcome: session_id={}, status={}",
        welcome.session_id,
        welcome.status);
    DAS_LOG_INFO(info_msg.c_str());

    co_return DAS_S_OK;
}

boost::asio::awaitable<DasResult> HostLauncher::SendHandshakeReadyAsync(
    uint16_t session_id)
{
    if (!impl_->async_transport)
    {
        DAS_CORE_LOG_ERROR("Transport not initialized");
        co_return DAS_E_IPC_NOT_INITIALIZED;
    }

    ReadyRequestV1 ready;
    InitReadyRequest(ready, session_id);

    // V3: 16-bit call_id
    uint16_t call_id = impl_->next_call_id++;
    if (impl_->next_call_id == 0)
    {
        impl_->next_call_id = 1; // 溢出后从 1 开始
    }
    auto validated_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::REQUEST)
            .SetControlPlaneCommand(HandshakeInterfaceId::HANDSHAKE_IFACE_READY)
            .SetBodySize(sizeof(ready))
            .SetCallId(call_id)
            .Build();

    DasResult result = co_await impl_->async_transport->SendCoroutine(
        validated_header,
        reinterpret_cast<const uint8_t*>(&ready),
        sizeof(ready));

    if (result != DAS_S_OK)
    {
        std::string msg =
            DAS_FMT_NS::format("Failed to send Ready: error={}", result);
        DAS_CORE_LOG_ERROR(msg.c_str());
        co_return result;
    }

    std::string info_msg =
        DAS_FMT_NS::format("Sent Ready: session_id={}", session_id);
    DAS_LOG_INFO(info_msg.c_str());
    co_return DAS_S_OK;
}

boost::asio::awaitable<DasResult> HostLauncher::ReceiveHandshakeReadyAckAsync(
    uint32_t timeout_ms)
{
    (void)timeout_ms; // 暂时忽略超时

    if (!impl_->async_transport)
    {
        DAS_CORE_LOG_ERROR("Transport not initialized");
        co_return DAS_E_IPC_NOT_INITIALIZED;
    }

    auto result_variant = co_await impl_->async_transport->ReceiveCoroutine();

    if (result_variant.index() == 0)
    {
        DasResult   error_code = std::get<0>(result_variant);
        std::string msg = DAS_FMT_NS::format(
            "Failed to receive ReadyAck: error={}",
            error_code);
        DAS_CORE_LOG_ERROR(msg.c_str());
        co_return error_code;
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
        DAS_CORE_LOG_ERROR(msg.c_str());
        co_return DAS_E_IPC_UNEXPECTED_MESSAGE;
    }

    if (body.size() < sizeof(ReadyAckV1))
    {
        DAS_CORE_LOG_ERROR("ReadyAck response body too small");
        co_return DAS_E_IPC_INVALID_MESSAGE_BODY;
    }

    ReadyAckV1 ack = *reinterpret_cast<const ReadyAckV1*>(body.data());

    if (ack.status != ReadyAckV1::STATUS_SUCCESS)
    {
        std::string msg =
            DAS_FMT_NS::format("ReadyAck status error: {}", ack.status);
        DAS_CORE_LOG_ERROR(msg.c_str());
        co_return DAS_E_IPC_HANDSHAKE_FAILED;
    }

    std::string info_msg =
        DAS_FMT_NS::format("Received ReadyAck: status={}", ack.status);
    DAS_LOG_INFO(info_msg.c_str());

    co_return DAS_S_OK;
}

boost::asio::awaitable<DasResult> HostLauncher::PerformFullHandshakeAsync(
    uint16_t& out_session_id,
    uint32_t  timeout_ms)
{
    DasResult result = co_await SendHandshakeHelloAsync("HostLauncher");
    if (result != DAS_S_OK)
    {
        co_return result;
    }

    result = co_await ReceiveHandshakeWelcomeAsync(out_session_id, timeout_ms);
    if (result != DAS_S_OK)
    {
        co_return result;
    }

    if (out_session_id == 0)
    {
        DAS_CORE_LOG_ERROR("Received invalid session_id (0)");
        co_return DAS_E_IPC_HANDSHAKE_FAILED;
    }

    result = co_await SendHandshakeReadyAsync(out_session_id);
    if (result != DAS_S_OK)
    {
        co_return result;
    }

    result = co_await ReceiveHandshakeReadyAckAsync(timeout_ms);
    if (result != DAS_S_OK)
    {
        co_return result;
    }

    std::string msg = DAS_FMT_NS::format(
        "Full handshake completed: session_id={}",
        out_session_id);
    DAS_LOG_INFO(msg.c_str());

    co_return DAS_S_OK;
}

//=============================================================================
// 同步版本的握手方法（使用 co_spawn + use_future）
//=============================================================================

DasResult HostLauncher::SendHandshakeHello(const std::string& client_name)
{
    try
    {
        auto future = boost::asio::co_spawn(
            impl_->io_ctx,
            SendHandshakeHelloAsync(client_name),
            boost::asio::use_future);
        return future.get();
    }
    catch (const boost::system::system_error& e)
    {
        std::string msg = DAS_FMT_NS::format(
            "SendHandshakeHello failed (system_error): {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_SEND_FAILED;
    }
    catch (const std::exception& e)
    {
        std::string msg = DAS_FMT_NS::format(
            "SendHandshakeHello failed: {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_SEND_FAILED;
    }
}

DasResult HostLauncher::ReceiveHandshakeWelcome(
    uint16_t& out_session_id,
    uint32_t  timeout_ms)
{
    try
    {
        auto future = boost::asio::co_spawn(
            impl_->io_ctx,
            ReceiveHandshakeWelcomeAsync(out_session_id, timeout_ms),
            boost::asio::use_future);
        auto duration = (timeout_ms == 0)
                            ? std::chrono::milliseconds(
                                  (std::numeric_limits<uint32_t>::max)())
                            : std::chrono::milliseconds(timeout_ms);
        auto status = future.wait_for(duration);
        if (status == std::future_status::timeout)
        {
            DAS_CORE_LOG_ERROR("Timeout receiving Welcome message");
            return DAS_E_IPC_TIMEOUT;
        }
        return future.get();
    }
    catch (const boost::system::system_error& e)
    {
        std::string msg = DAS_FMT_NS::format(
            "ReceiveHandshakeWelcome failed (system_error): {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_RECEIVE_FAILED;
    }
    catch (const std::exception& e)
    {
        std::string msg = DAS_FMT_NS::format(
            "ReceiveHandshakeWelcome failed: {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_RECEIVE_FAILED;
    }
}

DasResult HostLauncher::SendHandshakeReady(uint16_t session_id)
{
    try
    {
        auto future = boost::asio::co_spawn(
            impl_->io_ctx,
            SendHandshakeReadyAsync(session_id),
            boost::asio::use_future);
        return future.get();
    }
    catch (const boost::system::system_error& e)
    {
        std::string msg = DAS_FMT_NS::format(
            "SendHandshakeReady failed (system_error): {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_SEND_FAILED;
    }
    catch (const std::exception& e)
    {
        std::string msg = DAS_FMT_NS::format(
            "SendHandshakeReady failed: {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_SEND_FAILED;
    }
}

DasResult HostLauncher::ReceiveHandshakeReadyAck(uint32_t timeout_ms)
{
    try
    {
        auto future = boost::asio::co_spawn(
            impl_->io_ctx,
            ReceiveHandshakeReadyAckAsync(timeout_ms),
            boost::asio::use_future);
        auto duration = (timeout_ms == 0)
                            ? std::chrono::milliseconds(
                                  (std::numeric_limits<uint32_t>::max)())
                            : std::chrono::milliseconds(timeout_ms);
        auto status = future.wait_for(duration);
        if (status == std::future_status::timeout)
        {
            DAS_CORE_LOG_ERROR("Timeout receiving ReadyAck message");
            return DAS_E_IPC_TIMEOUT;
        }
        return future.get();
    }
    catch (const boost::system::system_error& e)
    {
        std::string msg = DAS_FMT_NS::format(
            "ReceiveHandshakeReadyAck failed (system_error): {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_RECEIVE_FAILED;
    }
    catch (const std::exception& e)
    {
        std::string msg = DAS_FMT_NS::format(
            "ReceiveHandshakeReadyAck failed: {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_RECEIVE_FAILED;
    }
}

//=============================================================================
// 异步启动相关方法（T2/T3）
//=============================================================================

boost::asio::awaitable<DasResult> HostLauncher::WaitForHostReadyAsync(
    uint32_t timeout_ms)
{
    auto start = std::chrono::steady_clock::now();

    while (true)
    {
        if (!IsRunning())
        {
            DAS_CORE_LOG_ERROR("Host process terminated unexpectedly");
            co_return DAS_E_IPC_CONNECTION_LOST;
        }

        // Transport-agnostic check: if ConnectToHost succeeded,
        // the transport is established and Host is connected.
        if (impl_->async_transport)
        {
            DAS_CORE_LOG_INFO("Host IPC transport connected");
            co_return DAS_S_OK;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (timeout_ms != 0
            && elapsed.count() >= static_cast<int64_t>(timeout_ms))
        {
            DAS_CORE_LOG_ERROR("Timeout waiting for Host IPC transport");
            co_return DAS_E_IPC_TIMEOUT;
        }

        auto timer = boost::asio::steady_timer(
            impl_->io_ctx,
            std::chrono::milliseconds(100));
        co_await timer.async_wait(boost::asio::use_awaitable);
    }
}

boost::asio::awaitable<DasResult> HostLauncher::ConnectToHostAsync()
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

    try
    {
        auto [transport_result, maybe_transport] =
            co_await AnyTransport::CreateAsync(
            impl_->io_ctx,
            host_to_plugin_pipe,
            plugin_to_host_pipe,
            true, // is_server = true (MainProcess creates pipes)
            65536);

        if (DAS::IsFailed(transport_result) || !maybe_transport)
        {
            std::string err_msg = DAS_FMT_NS::format(
                "Failed to create Host IPC transport: error = {}",
                transport_result);
            DAS_CORE_LOG_ERROR(err_msg.c_str());
            co_return transport_result;
        }

        impl_->async_transport = std::move(*maybe_transport);
    }
    catch (const boost::system::system_error& e)
    {
        std::string err_msg = DAS_FMT_NS::format(
            "Failed to create Host IPC transport (system_error): {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(err_msg.c_str());
        co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }
    catch (const std::exception& e)
    {
        std::string err_msg = DAS_FMT_NS::format(
            "Failed to create Host IPC transport: {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(err_msg.c_str());
        co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    DAS_LOG_INFO("Host IPC pipes created and Host connected successfully");
    co_return DAS_S_OK;
}

boost::asio::awaitable<void> HostLauncher::RunAsync(
    IDasAsyncHandshakeOperation*       op_interface,
    std::shared_ptr<std::atomic<bool>> alive,
    std::string                        exe_path,
    uint32_t                           timeout_ms)
{
    // Cast to concrete type for internal Complete() access
    auto* op = static_cast<HandshakeAsyncOperationImpl*>(op_interface);

    auto start_time = std::chrono::steady_clock::now();

    auto remaining_ms = [&]() -> uint32_t
    {
        if (timeout_ms == 0)
            return 0;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start_time)
                           .count();
        if (elapsed >= static_cast<int64_t>(timeout_ms))
            return 0;
        return static_cast<uint32_t>(timeout_ms - elapsed);
    };

    try
    {
        // Phase 1: LaunchProcess (sync, fast)
        if (!alive->load(std::memory_order_acquire))
        {
            op->Complete(DAS_E_IPC_CANCELED, 0);
            co_return;
        }

        uint32_t main_pid = static_cast<uint32_t>(GET_CURRENT_PID());
        std::vector<std::string> args;
        args.push_back("--main-pid");
        args.push_back(std::to_string(main_pid));

        DasResult result = LaunchProcess(exe_path, args);
        if (DAS::IsFailed(result))
        {
            op->Complete(result, 0);
            co_return;
        }

        // Phase 2: ConnectToHost (async)
        if (!alive->load(std::memory_order_acquire))
        {
            Stop();
            op->Complete(DAS_E_IPC_CANCELED, 0);
            co_return;
        }

        result = co_await ConnectToHostAsync();
        if (DAS::IsFailed(result))
        {
            Stop();
            op->Complete(result, 0);
            co_return;
        }

        // Phase 3: WaitForHostReady (async)
        if (!alive->load(std::memory_order_acquire))
        {
            Stop();
            op->Complete(DAS_E_IPC_CANCELED, 0);
            co_return;
        }

        result = co_await WaitForHostReadyAsync(remaining_ms());
        if (DAS::IsFailed(result))
        {
            Stop();
            op->Complete(result, 0);
            co_return;
        }

        // Phase 4: PerformFullHandshake (async)
        if (!alive->load(std::memory_order_acquire))
        {
            Stop();
            op->Complete(DAS_E_IPC_CANCELED, 0);
            co_return;
        }

        uint16_t session_id = 0;
        result = co_await PerformFullHandshakeAsync(session_id, remaining_ms());
        if (DAS::IsFailed(result))
        {
            Stop();
            op->Complete(result, 0);
            co_return;
        }

        impl_->session_id = session_id;

        // Phase 5: RegisterHostLauncher (sync, fast)
        if (!alive->load(std::memory_order_acquire))
        {
            Stop();
            op->Complete(DAS_E_IPC_CANCELED, 0);
            co_return;
        }

        if (on_register_)
        {
            result = on_register_();
            if (DAS::IsFailed(result))
            {
                DAS_CORE_LOG_ERROR(
                    "Registration callback failed: error={}",
                    result);
                Stop();
                op->Complete(result, 0);
                co_return;
            }
        }

        std::string success_msg = DAS_FMT_NS::format(
            "Host process started successfully: PID = {}, session_id = {}",
            impl_->pid,
            session_id);
        DAS_LOG_INFO(success_msg.c_str());

        // 异步监控 Host 进程退出，触发回调通知 PluginManager 清理索引
        if (impl_->process && on_process_exit_)
        {
            auto callback = on_process_exit_;
            auto cb_session_id = session_id;
            auto process_ptr = impl_->process.get();

            boost::asio::co_spawn(
                impl_->io_ctx,
                [process_ptr,
                 callback,
                 cb_session_id]() -> boost::asio::awaitable<void>
                {
                    boost::system::error_code ec;
                    co_await process_ptr->async_wait(
                        boost::asio::redirect_error(
                            boost::asio::use_awaitable,
                            ec));
                    int exit_code = ec ? -1 : process_ptr->exit_code();
                    if (callback)
                    {
                        callback(cb_session_id, exit_code);
                    }
                    co_return;
                },
                boost::asio::detached);
        }

        op->Complete(DAS_S_OK, session_id);
    }
    catch (const std::exception& e)
    {
        std::string err_msg = DAS_FMT_NS::format(
            "Async Host start failed: {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(err_msg.c_str());
        Stop();
        op->Complete(DAS_E_IPC_HANDSHAKE_FAILED, 0);
    }
}

DAS_CORE_IPC_NS_END
