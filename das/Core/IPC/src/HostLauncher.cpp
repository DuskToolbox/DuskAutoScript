/**
 * @file HostLauncher.cpp
 * @brief Host 进程启动器实现
 */
#define _CRT_SECURE_NO_WARNINGS

#include <cassert>
#include <cctype>
#include <chrono>
#include <das/Core/IPC/AsioSender.h>
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
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <optional>
#include <thread>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_BOOST_PROCESS_WARNING
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/pid.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/system/error_code.hpp>
DAS_DISABLE_WARNING_END

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_STDEXEC_PARAMETERS
#include <exec/async_scope.hpp>
DAS_DISABLE_WARNING_END

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
    // 跟踪全部异步子任务（StartAsync 握手链 + 成功路径 exit-watcher）。
    // Stop() 不 request_stop（exec::async_scope 的 inplace_stop_source
    // 是 one-way，request_stop 会永久锁死 scope，Start→Stop→Start 复用后新
    // watcher 的 stop_token 立即 stop_requested → exit-watcher 永不触发）。
    // drain 改为自然完成：GOODBYE（+terminate 兜底）让进程退出 → exit-watcher
    // 的 async_wait 收到真实退出码自然完成 → sync_wait(scope.on_empty()) 返回。
    exec::async_scope                            scope;
    std::unique_ptr<boost::process::v2::process> process;
    std::optional<AnyTransport>                  async_transport;
    std::unique_ptr<SharedMemoryPool>            pool_;
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
            {
                delete this;
            }
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
                {
                    handler_->OnCompleted(this, status_.load());
                }
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
            {
                return DAS_E_IPC_INVALID_STATE;
            }
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

            const int32_t new_status =
                DAS::IsOk(result) ? DAS_ASYNC_COMPLETED : DAS_ASYNC_FAILED;
            status_.store(new_status, std::memory_order_release);

            if (handler_)
            {
                handler_->OnCompleted(this, new_status);
            }
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

        // Owner-guarded async operations may borrow this AnyTransport across
        // co_await. Cleanup wakes those operations, but backing storage stays
        // owned by HostLauncher until owner teardown or controlled Start reuse.
        transport->Cleanup();
    }

    std::string DescribeException(std::exception_ptr e)
    {
        try
        {
            std::rethrow_exception(e);
        }
        catch (const std::exception& ex)
        {
            // ToString 处理 Windows 异常消息的 GBK -> UTF-8 编码转换（与既有
            // catch 块的日志用法一致，boost::system::system_error /
            // std::exception 都经此转换）。
            return std::string(ToString(ex.what()).string_view());
        }
        catch (...)
        {
            return "unknown exception";
        }
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

    struct EnvironmentPrepend
    {
        std::string name;
        std::string value;
    };

    constexpr std::string_view VERSION_KEY = "version";
    constexpr std::string_view PREPEND_KEY = "prepend";
    constexpr std::string_view NAME_KEY = "name";
    constexpr std::string_view VALUE_KEY = "value";

    bool IsEnvironmentConfigVersionOne(const yyjson::value& value)
    {
        if (auto version = value.as_sint())
        {
            return *version == 1;
        }

        if (auto version = value.as_uint())
        {
            return *version == 1;
        }

        return false;
    }

    char PathListDelimiter()
    {
#ifdef _WIN32
        return ';';
#else
        return ':';
#endif
    }

    bool EnvironmentNameEquals(std::string_view lhs, std::string_view rhs)
    {
        if (lhs.size() != rhs.size())
        {
            return false;
        }

#ifdef _WIN32
        for (size_t i = 0; i < lhs.size(); ++i)
        {
            const auto lhs_ch = static_cast<unsigned char>(lhs[i]);
            const auto rhs_ch = static_cast<unsigned char>(rhs[i]);
            if (std::tolower(lhs_ch) != std::tolower(rhs_ch))
            {
                return false;
            }
        }
        return true;
#else
        return lhs == rhs;
#endif
    }

    auto FindEnvironmentEntry(
        std::map<std::string, std::string>& values,
        std::string_view name) -> std::map<std::string, std::string>::iterator
    {
        for (auto it = values.begin(); it != values.end(); ++it)
        {
            if (EnvironmentNameEquals(it->first, name))
            {
                return it;
            }
        }

        return values.end();
    }

    DasResult ParseEnvironmentConfig(
        std::string_view                 raw_config,
        std::vector<EnvironmentPrepend>& out_prepends)
    {
        auto parsed = Das::Utils::ParseYyjsonFromString(raw_config);
        if (!parsed)
        {
            return DAS_E_INVALID_ARGUMENT;
        }

        auto root = parsed->as_object();
        if (!root)
        {
            return DAS_E_INVALID_ARGUMENT;
        }

        if (!root->contains(VERSION_KEY)
            || !IsEnvironmentConfigVersionOne((*root)[VERSION_KEY]))
        {
            return DAS_E_INVALID_ARGUMENT;
        }

        if (!root->contains(PREPEND_KEY))
        {
            return DAS_E_INVALID_ARGUMENT;
        }

        auto prepend = (*root)[PREPEND_KEY].as_array();
        if (!prepend)
        {
            return DAS_E_INVALID_ARGUMENT;
        }

        for (auto it = prepend->begin(); it != prepend->end(); ++it)
        {
            auto item = it->as_object();
            if (!item)
            {
                return DAS_E_INVALID_ARGUMENT;
            }

            if (!item->contains(NAME_KEY) || !item->contains(VALUE_KEY))
            {
                return DAS_E_INVALID_ARGUMENT;
            }

            auto name = (*item)[NAME_KEY].as_string();
            auto value = (*item)[VALUE_KEY].as_string();
            if (!name || name->empty() || !value || value->empty())
            {
                return DAS_E_INVALID_ARGUMENT;
            }

            out_prepends.push_back(
                EnvironmentPrepend{std::string(*name), std::string(*value)});
        }

        return DAS_S_OK;
    }

    std::vector<std::string> BuildProcessEnvironment(
        const std::vector<EnvironmentPrepend>& prepends)
    {
        std::map<std::string, std::string> values;
        for (const auto item : boost::process::v2::environment::current())
        {
            values[item.key().string()] = item.value().string();
        }

        const auto delimiter = PathListDelimiter();
        for (const auto& prepend : prepends)
        {
            auto existing = FindEnvironmentEntry(values, prepend.name);
            if (existing == values.end())
            {
                values[prepend.name] = prepend.value;
                continue;
            }

            if (existing->second.empty())
            {
                existing->second = prepend.value;
            }
            else
            {
                existing->second = prepend.value + delimiter + existing->second;
            }
        }

        std::vector<std::string> result;
        result.reserve(values.size());
        for (const auto& [key, value] : values)
        {
            result.push_back(key + "=" + value);
        }
        return result;
    }

    DasResult ReadEnvironmentConfig(
        IDasReadOnlyString*                      p_environment_config,
        std::optional<std::vector<std::string>>& out_environment)
    {
        if (!p_environment_config)
        {
            out_environment.reset();
            return DAS_S_OK;
        }

        std::string raw_config;
        auto        result = ReadUtf8String(p_environment_config, raw_config);
        if (DAS::IsFailed(result) || raw_config.empty())
        {
            return DAS_E_INVALID_ARGUMENT;
        }

        std::vector<EnvironmentPrepend> prepends;
        result = ParseEnvironmentConfig(raw_config, prepends);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        out_environment = BuildProcessEnvironment(prepends);
        return DAS_S_OK;
    }

    std::string DefaultWorkingDirectoryFor(const std::string& exe_path)
    {
        const auto parent = std::filesystem::path(exe_path).parent_path();
        if (!parent.empty())
        {
            return std::string{DAS::Utils::U8AsString(parent.u8string())};
        }
        return std::string{
            DAS::Utils::U8AsString(std::filesystem::current_path().u8string())};
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

HostLauncher::~HostLauncher()
{
    // 析构路径：io_context 可能已停止/析构（~IpcRunLoop 先 stop 再 reset
    // io_context_，随 ~ConnectionManager / io_context 析构释放 hosts_ 与
    // receive loop 帧的引用后本对象才析构）。此时 CleanupTransportAsync 会
    // co_spawn 到已停的 io_context 而永不执行 → .get() 死锁。且无存活
    // io_context 协程借用 transport（receive loop 的 DasPtr 已随协程帧释放），
    // 直接清理安全。
    use_async_transport_cleanup_ = false;
    Stop();
}

DasResult HostLauncher::Start(
    const std::string& host_exe_path,
    uint16_t&          out_session_id,
    uint32_t           timeout_ms)
{
    // 检查可执行文件是否存在
    if (!std::filesystem::exists(host_exe_path))
    {
        const std::string msg =
            DAS_FMT_NS::format("Host executable not found: {}", host_exe_path);
        DAS_CORE_LOG_ERROR(msg.c_str());
        return DAS_E_INVALID_ARGUMENT;
    }

    // 获取当前进程 PID（主进程 PID）
    uint32_t main_pid =
        static_cast<uint32_t>(boost::process::v2::current_pid());

    const std::string pid_msg = DAS_FMT_NS::format(
        "HostLauncher::Start - Main process PID: {}",
        main_pid);
    DAS_LOG_INFO(pid_msg.c_str());

    // 构建命令行参数
    std::vector<std::string> args;
    args.emplace_back("--main-pid");
    args.push_back(std::to_string(main_pid));

    return StartLaunchSequence(
        host_exe_path,
        args,
        std::nullopt,
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

    std::optional<std::vector<std::string>> environment;
    result = ReadEnvironmentConfig(p_desc->p_environment_config, environment);
    if (DAS::IsFailed(result))
    {
        return result;
    }

    return StartLaunchSequence(
        exe_path,
        args,
        working_directory,
        environment,
        *p_out_session_id,
        timeout_ms);
}

DasResult HostLauncher::StartLaunchSequence(
    const std::string&                             exe_path,
    const std::vector<std::string>&                args,
    const std::optional<std::string>&              working_directory,
    const std::optional<std::vector<std::string>>& environment,
    uint16_t&                                      out_session_id,
    uint32_t                                       timeout_ms)
{
    DasResult result =
        LaunchProcess(exe_path, args, working_directory, environment);
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

    const std::string msg = DAS_FMT_NS::format(
        "Host process started successfully: PID={}, session_id={}",
        impl_->pid,
        out_session_id);
    DAS_LOG_INFO(msg.c_str());

    // 异步监控 Host 进程退出，触发回调通知 PluginManager 清理索引。
    // 注意：不再取裸 std::function 副本（CR-01 数据竞争点）。exit-watcher 协程
    // 触发时直接调 slot_.Invoke，持锁执行 callback（D-01）。slot 未 Set 时
    // Invoke 内部判断空则空转，语义等价于旧版 `if (callback)` 判断。
    // 与 StartAsync 成功路径 watcher 一致迁入 async_scope（D-91-06）：watcher
    // 生命周期由 Stop() 的 scope.on_empty() drain 跟踪，替代 use_future 载体
    // （UAF 2/#1338 防护由 scope drain 承载）。
    if (impl_->process)
    {
        auto       session_id = session_id_;
        auto       process_ptr = impl_->process.get();
        const auto stop_token = impl_->scope.get_stop_token();

        impl_->scope.spawn(
            boost::asio::co_spawn(
                impl_->io_ctx,
                [this, process_ptr, session_id, stop_token]()
                    -> boost::asio::awaitable<void>
                {
                    // Pitfall 3 守卫（前置检查，保留为防御性）。Stop()
                    // 不再 request_stop scope，正常生命周期下 stop_token
                    // 永不停， 守卫不触发——但保留它以防御未来任何 stop
                    // 源（scope 的 inplace_stop_source 仍可能被外部
                    // request_stop）。若已停止 则在注册 async_wait 前退出，避免
                    // async_wait 因进程存活 永不完成导致 scope 永不 drain。
                    if (stop_token.stop_requested())
                    {
                        co_return;
                    }
                    boost::system::error_code ec;
                    co_await process_ptr->async_wait(
                        boost::asio::redirect_error(
                            boost::asio::use_awaitable,
                            ec));
                    // Pitfall 3 守卫（后置检查，保留为防御性）。正常路径（无
                    // cancel）async_wait 因进程真实退出完成，ec 为空 → 放行真实
                    // exit_code。仅当外部 request_stop 取消 async_wait 时 ec=
                    // operation_aborted 且 stop_requested() 为 true，此处跳过
                    // Invoke 避免伪 -1 回调（防御性；Stop 驱动 teardown 的伪
                    // 回调抑制由 StopBodyRequestExit 的 ClearCallbacks 承担）。
                    if (stop_token.stop_requested())
                    {
                        co_return;
                    }
                    const int exit_code = ec ? -1 : process_ptr->exit_code();
                    // 持锁执行 callback（INV-01：回调内部不得重入本 slot）
                    on_process_exit_slot_.Invoke(session_id, exit_code);
                    co_return;
                },
                AsioSenderToken{})
            | stdexec::let_error(
                [](std::exception_ptr e)
                {
                    DAS_CORE_LOG_ERROR(
                        "Host exit-watcher failed: {}",
                        DescribeException(e));
                    return stdexec::just_stopped();
                }));
    }

    return DAS_S_OK;
}

std::future<void> HostLauncher::CleanupTransportAsync()
{
    // transport 清理串行化到 io_context 线程（单线程执行），调用线程阻塞等待。
    // 解决：调用线程直接 CleanupTransport 与 io_context 线程 pending
    // ReceiveCoroutine 对同一 transport 的跨线程并发双释放。
    return boost::asio::co_spawn(
        impl_->io_ctx,
        [this]() -> boost::asio::awaitable<void>
        {
            CleanupTransport(impl_->async_transport);
            co_return;
        },
        boost::asio::use_future);
}

void HostLauncher::Stop()
{
    // 并发 Stop() 串行化（stop_mutex_）：RequestStop 与生命周期回调可能同时
    // 对同一 launcher 触发 teardown，双重 teardown 会竞争 process/transport
    // 对象。后到者等待先到者完成；teardown body 幂等，重复执行无害。
    const std::scoped_lock lock(stop_mutex_);

    // Stop() 使用 co_spawn + use_future 阻塞当前线程等待 io_context 完成操作，
    // 因此不能从 io_context 线程调用，否则会死锁。
    assert(
        !impl_->io_ctx.get_executor().running_in_this_thread()
        && "Stop() must not be called from the io_context thread");

    // teardown 两阶段（StopBodyRequestExit + StopBodyCleanup）是运行在
    // io_context 线程上的协程：GOODBYE 用 co_await 发送（不阻塞），transport
    // 清理直接执行（天然与该线程 pending 协程串行，保持 31616073 的双 UAF
    // 修复语义）。仅当 io_context 存活且 transport 已连接（说明 io_context
    // 正被驱动）时 co_spawn+get 阻塞等其完成；析构路径
    // （use_async_transport_cleanup_ == false，io_context 已停/析构）或
    // transport 未连接时不能/无需 co_spawn——co_spawn 到已停的 io_context
    // 永不执行 → .get() 死锁，未驱动的 io_context 也会阻塞，此时走
    // StopBodySync() 直接同步清理（无存活协程借用 transport，GOODBYE
    // 无意义）。
    if (use_async_transport_cleanup_ && !impl_->io_ctx.stopped()
        && impl_->async_transport && impl_->async_transport->IsConnected())
    {
        try
        {
            // 修复：scope 永不 request_stop（exec::async_scope 的
            // inplace_stop_source 是 one-way，request_stop 会永久锁死 scope，
            // Start→Stop→Start 复用后新 exit-watcher 的 stop_token 立即
            // stop_requested → 不注册 async_wait → on_process_exit 对重启后的
            // host 永不触发）。drain 改为自然完成：
            //   1. StopBodyRequestExit 发 GOODBYE（+terminate 兜底）让进程最终
            //      必然退出；
            //   2. exit-watcher 的 async_wait
            //   因进程真实退出完成（真实退出码）；
            //   3. sync_wait(scope.on_empty()) drain 返回（在途 StartAsync 握手
            //      链因进程退出/管道断开/超时自然结束，可接受几秒级延迟）。
            // 顺序约束：terminate 兜底必须先于 on_empty——否则进程卡住不退，
            // watcher 的 async_wait 永不完成，on_empty
            // 永久挂起。process.reset() 必须后于 drain（StopBodyCleanup
            // 执行）——否则 watcher 借用的 process 对象先被销毁会 UAF（#1338
            // 语义）。
            boost::asio::co_spawn(
                impl_->io_ctx,
                StopBodyRequestExit(),
                boost::asio::use_future)
                .get();

            stdexec::sync_wait(impl_->scope.on_empty());

            boost::asio::co_spawn(
                impl_->io_ctx,
                StopBodyCleanup(),
                boost::asio::use_future)
                .get();
        }
        catch (const std::exception& e)
        {
            const std::string msg = DAS_FMT_NS::format(
                "HostLauncher::Stop teardown coroutine failed: {}",
                ToString(e.what()));
            DAS_CORE_LOG_ERROR(msg.c_str());
        }
    }
    else
    {
        StopBodySync();
    }
}

boost::asio::awaitable<void> HostLauncher::StopBodyRequestExit()
{
    // 全清心跳+退出 slot（Pitfall 6 修复：消除旧版只清 on_process_exit_
    // 的不对称）。 ClearCallbacks 持锁 drain（CR-02），避免 Stop
    // 主动关闭进程时残留 callback 被并发触发。心跳 slot 同清，避免 Stop
    // 后心跳线程仍触发回调的窗口。
    //
    // 本阶段保证「进程最终必然退出」（GOODBYE → 超时 → terminate 兜底）。
    // 关键：exit-watcher 的正常路径回调抑制靠这里清空的 slot（watcher 在
    // scope drain 时自然完成并 Invoke，但 callback 已空 → 空转），而非靠
    // stop_token 守卫（Stop 不 request_stop，守卫不触发）。
    ClearCallbacks();

    // 先发送 GOODBYE 消息让 Host 进程优雅退出（所有路径保留：co_await 非阻塞
    // 发送，不依赖 co_spawn+use_future）。
    if (impl_->async_transport && impl_->is_running)
    {
        GoodbyeV1 goodbye{};
        goodbye.reason = static_cast<uint32_t>(GoodbyeReason::NormalShutdown);

        const std::string log_msg = DAS_FMT_NS::format(
            "Sending GOODBYE to Host process: PID={}",
            impl_->pid);
        DAS_LOG_INFO(log_msg.c_str());

        auto validated_header =
            IPCMessageHeaderBuilder()
                .SetMessageType(MessageType::Request)
                .SetControlPlaneCommand(
                    HandshakeInterfaceId::HandshakeIfaceGoodbye)
                .SetBodySize(sizeof(goodbye))
                .Build();

        try
        {
            DasResult result = co_await impl_->async_transport->SendCoroutine(
                validated_header,
                reinterpret_cast<const uint8_t*>(&goodbye),
                sizeof(goodbye));
            if (result != DAS_S_OK)
            {
                const std::string err_msg = DAS_FMT_NS::format(
                    "Failed to send GOODBYE: error={}",
                    result);
                DAS_CORE_LOG_ERROR(err_msg.c_str());
            }
        }
        catch (const boost::system::system_error& e)
        {
            const std::string err_msg = DAS_FMT_NS::format(
                "Send GOODBYE failed with system_error: {}",
                ToString(e.what()));
            DAS_CORE_LOG_ERROR(err_msg.c_str());
        }
        catch (const std::exception& e)
        {
            const std::string err_msg = DAS_FMT_NS::format(
                "Send GOODBYE failed: {}",
                ToString(e.what()));
            DAS_CORE_LOG_ERROR(err_msg.c_str());
        }

        // 等待进程退出，最多等待 2 秒。
        // 如果进程在 GOODBYE 后正常退出，则不需要 terminate。协程版用
        // steady_timer 轮询（co_await 让出 io_context），不阻塞线程；期间
        // io_context 会处理已排队的 process-exit watcher completion（TP
        // drain 的等价物），exit-watcher 在进程退出后自然完成。
        bool       process_exited = false;
        const auto wait_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < wait_deadline)
        {
            if (!impl_->process->running())
            {
                process_exited = true;
                break;
            }
            boost::asio::steady_timer timer(
                impl_->io_ctx,
                std::chrono::milliseconds(100));
            co_await timer.async_wait(boost::asio::use_awaitable);
        }

        if (process_exited)
        {
            const std::string exit_msg = DAS_FMT_NS::format(
                "Host process exited gracefully: PID={}",
                impl_->pid);
            DAS_LOG_INFO(exit_msg.c_str());
        }
    }

    // 进程退出兜底（GOODBYE → 超时 → terminate）：GOODBYE 未在 2s 内退出（或
    // transport 未连接/无 GOODBYE）时，若进程仍存活则 terminate 杀进程兜底。
    // 这是「进程最终必然退出」的保证——否则进程卡住不退，exit-watcher 的
    // async_wait 永不完成，Stop() 的 sync_wait(scope.on_empty()) 会永久挂起。
    // 注意：本阶段只保证进程退出，不 reset process —— process.reset() 由
    // StopBodyCleanup 在 Stop() 完成 scope drain 之后执行（exit-watcher 的
    // async_wait 借用的 process 对象须存活到 watcher 完成，#1338 语义）。
    if (impl_->process)
    {
        boost::system::error_code ec;

        // 检查进程是否仍在运行
        const bool still_running = impl_->process->running(ec);

        if (still_running)
        {
            const std::string msg = DAS_FMT_NS::format(
                "HostLauncher::Stop - terminating Host process: pid={}, session_id={}",
                impl_->pid,
                impl_->session_id);
            DAS_LOG_INFO(msg.c_str());
            impl_->process->terminate(ec);
        }
    }
}

boost::asio::awaitable<void> HostLauncher::StopBodyCleanup()
{
    // transport 清理：本协程在 io_context 线程上执行，与该线程 pending
    // ReceiveCoroutine 按序串行（单线程 io_context），直接清理安全，保持
    // 31616073 的双 UAF 修复语义。
    CleanupTransport(impl_->async_transport);

    // 清理 SharedMemoryPool（transport 清理后，确保无悬空引用）
    impl_->pool_.reset();

    if (impl_->process)
    {
        // 进程已在 StopBodyRequestExit 中退出（GOODBYE 优雅退出或 terminate
        // 兜底），且 Stop() 在两个阶段之间 sync_wait(scope.on_empty()) drain
        // 了全部在途子任务（StartAsync 握手链 + 成功路径 exit-watcher）——
        // watcher 已完成，process.reset() 严格发生在 scope 清空之后
        // （UAF 2/#1338 防护由 scope drain 承载，替代旧 use_future watcher
        // 载体的 wait_for 轮询）。

        // 正常析构：Windows 上 ~process() 会调 UnregisterWaitEx(NULL)，
        // 同步等待已 pending 的 thread-pool wait callback 完成。
        impl_->process.reset();
    }

    impl_->is_running = false;
    impl_->session_id = 0;
    impl_->pid = 0;

    co_return; // 使本函数成为协程（co_spawn 需要 awaitable；本阶段无 co_await）
}

void HostLauncher::StopBodySync()
{
    // 非 io_context 线程的直接同步 teardown（io_context 已停/析构或 transport
    // 未连接时的 fallback，由 Stop() 调用）。此时无存活 io_context 协程借用
    // transport，直接清理安全；GOODBYE 依赖 co_spawn 阻塞等 io_context，此
    // 路径不可用且 transport 未连接时发送无意义，跳过。
    //
    // scope drain 约束（已评估，接受）：本路径不 drain
    // async_scope。正常生命周期 下 scope 由 Stop() 的 async
    // 分支（StopBodyRequestExit → on_empty → StopBodyCleanup）在 io_context
    // 仍存活时 drain，故走到 StopBodySync 时 scope 应为空（exit-watcher
    // 已完成、StartAsync 链已结束）。仅当 io_context 已停而 scope
    // 仍有未完成任务（StartAsync 在途且从未被 async Stop 收口） 时，async_scope
    // 析构会 STDEXEC_ASSERT(__active_ == 0)。此处不能同步 drain：io_context
    // 已停时 sync_wait(on_empty) 无人驱动会死锁（Pitfall 4）。
    // 这是约束而非可运行时处理的场景——保持注释明文化。

    // 全清心跳+退出 slot（Pitfall 6 修复：消除旧版只清 on_process_exit_
    // 的不对称）。 ClearCallbacks 持锁 drain（CR-02），避免 Stop
    // 主动关闭进程时残留 callback 被并发触发。心跳 slot 同清，避免 Stop
    // 后心跳线程仍触发回调的窗口。
    ClearCallbacks();

    CleanupTransport(impl_->async_transport);

    // 清理 SharedMemoryPool（transport 清理后，确保无悬空引用）
    impl_->pool_.reset();

    if (impl_->process)
    {
        boost::system::error_code ec;

        // 检查进程是否仍在运行
        const bool still_running = impl_->process->running(ec);

        if (still_running)
        {
            const std::string msg = DAS_FMT_NS::format(
                "HostLauncher::Stop - terminating Host process: pid={}, session_id={}",
                impl_->pid,
                impl_->session_id);
            DAS_LOG_INFO(msg.c_str());
            impl_->process->terminate(ec);
        }

        // 等子进程真正退出后再析构（terminate 后 wait 瞬间返回），
        // 缩小 io_context 线程处理 exit 事件与 reset() 的竞争窗口。
        // 说明：本路径（io_context 已停 / transport 未连接）下 exit-watcher
        // 的 co_spawn 从未被驱动（async_wait 未注册），直接清理安全；停后
        // 阻塞等 watcher 会死锁（Pitfall 4），不引入同步等待。
        impl_->process->wait(ec);

        // 正常析构：Windows 上 ~process() 会调 UnregisterWaitEx(NULL)，
        // 同步等待已 pending 的 thread-pool wait callback 完成。
        impl_->process.reset();
    }

    impl_->is_running = false;
    impl_->session_id = 0;
    impl_->pid = 0;
}

boost::asio::awaitable<void> HostLauncher::StopFromIoContext()
{
    // 启动链失败路径调用（TeardownThen）：本协程在 io_context 线程上执行
    // （启动链经 io_context hop 在 io_context 上运行）。Stop() 用 co_spawn +
    // use_future 阻塞等 io_context 完成 GOODBYE / transport 清理 /
    // exit-watcher drain，从 io_context 线程调用会 assert + 死锁。本协程
    // 直接 co_await StopBodyRequestExit() + StopBodyCleanup()：GOODBYE 用
    // co_await 发送（不阻塞），transport 清理在 io_context 线程天然与
    // pending 协程串行。失败路径下 scope 无 exit-watcher（watcher 只在链
    // 尾成功分支 spawn），故两阶段间无需 on_empty drain——Stop()（若随后
    // 调用）会在 async 分支统一 drain。
    //
    // 用 try_lock 而非 lock：若另一线程的 Stop()/TerminateIfRunning() 已持
    // stop_mutex_ 且阻塞在 co_spawn+get（等 io_context 处理），本协程若阻塞
    // 在 stop_mutex_ 上会让 io_context 线程卡死 → 死锁。try_lock 失败 = 另一
    // teardown 进行中，其会完成清理（teardown body 幂等，重复执行无害），
    // 本协程跳过自身清理直接返回。
    std::unique_lock lock(stop_mutex_, std::try_to_lock);
    if (!lock.owns_lock())
    {
        co_return;
    }

    assert(
        impl_->io_ctx.get_executor().running_in_this_thread()
        && "StopFromIoContext must be called from the io_context thread");

    co_await StopBodyRequestExit();
    co_await StopBodyCleanup();
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
        const std::string msg =
            DAS_FMT_NS::format("Host executable not found: {}", host_exe_path);
        DAS_CORE_LOG_ERROR(msg.c_str());
        return DAS_E_INVALID_ARGUMENT;
    }

    // 调用线程竞争修复：原 IsRunning() 前置检查在调用者线程读 impl_->process /
    // impl_->is_running，与并发 Stop()（改写同一成员）数据竞争。改为在链内
    // 阶段 1（io_context hop 之后、io_context 线程上）做权威检查，失败经链
    // 尾 op->Complete(DAS_E_IPC_INVALID_STATE, 0) 异步送达（StartAsync 本仓
    // 无生产调用方，同步错误→异步错误不影响既有消费方）。

    constexpr uint32_t DEFAULT_TIMEOUT_MS = 10000;
    auto               alive = std::make_shared<std::atomic<bool>>(true);
    auto*              op = new HandshakeAsyncOperationImpl(*this, alive);

    // 5 阶段启动链 sender 化（D-91-03 方案 A / D-91-06）。握手 awaitable
    // 保留协程实现，经 AsioSenderToken 桥（Plan 01）包成 stdexec sender。
    // let_value 分支（TeardownThen vs 下一阶段）返回同一类型：用
    // std::function<awaitable<DasResult>()> 类型擦除 awaitable，使两分支的
    // co_spawn 结果 sender 类型一致（plan 原文"if (IsFailed) return
    // TeardownThen(r); return AsioSender{...}" 的异类型分支不满足 C++ 单一
    // 返回类型约束，须类型擦除才能编译）。
    using PhaseAwaitableFn = std::function<boost::asio::awaitable<DasResult>()>;

    const auto start_time = std::chrono::steady_clock::now();

    const auto remaining_ms = [start_time,
                               timeout_ms = DEFAULT_TIMEOUT_MS]() -> uint32_t
    {
        if (timeout_ms == 0)
        {
            return 0;
        }
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time)
                .count();
        if (elapsed >= static_cast<int64_t>(timeout_ms))
        {
            return 0;
        }
        return static_cast<uint32_t>(timeout_ms - elapsed);
    };

    // 失败路径 teardown：io 线程 try_lock stop_mutex_ 执行 StopFromIoContext
    // （复用 :955-980 语义），完成时把原始 DasResult 传回链。teardown_done
    // 保证失败后链继续流经后续 let_value 时只真正 teardown 一次（后续只是
    // 透传失败结果，不再重入 StopBody）。
    auto teardown_done = std::make_shared<bool>(false);

    const auto teardown_then = [this, teardown_done](DasResult r)
    {
        PhaseAwaitableFn fn =
            [this, r, teardown_done]() -> boost::asio::awaitable<DasResult>
        {
            // 并发竞争防护：DAS_E_IPC_INVALID_STATE 只由链内阶段 1 的 running
            // 检查 产生（"launcher 已在运行"）。此时绝不能 teardown——StopBody
            // 会 杀掉既有/并发 Start 的存活进程并触发其 exit-watcher UAF。直接
            // 透传失败结果，不做任何清理。
            if (r == DAS_E_IPC_INVALID_STATE)
            {
                co_return r;
            }
            if (!*teardown_done)
            {
                *teardown_done = true;
                co_await StopFromIoContext();
            }
            co_return r;
        };
        return boost::asio::co_spawn(
            impl_->io_ctx,
            std::move(fn),
            AsioSenderToken{});
    };

    // 链首 io_context hop（Pitfall 2）：保证 LaunchProcess/RegisterHostLauncher
    // 等同步步骤跑在 io_context 线程，不跑在 scope.spawn 的调用线程
    // （StartAsync 调用者线程），避免与 io_context 线程的 Stop() 竞争 impl_
    // 成员。
    auto io_ctx_hop = boost::asio::co_spawn(
        impl_->io_ctx,
        []() -> boost::asio::awaitable<void>
        { co_await boost::asio::post(boost::asio::use_awaitable); },
        AsioSenderToken{});

    // PerformFullHandshakeAsync 经引用写出的 session_id；链内多阶段共享。
    auto session_id = std::make_shared<uint16_t>(0);

    const auto build_chain = [&]() -> stdexec::sender auto
    {
        return io_ctx_hop
               // 阶段 1: running 检查 + LaunchProcess（sync）。竞争防护：在
               // io_context 线程做权威 IsRunning() 检查（链首 hop 已把本 then
               // 移到 io_context 线程），取代原调用者线程的前置检查——消除与
               // 并发 Stop() 改写 impl_ 成员的数据竞争。launcher 已在运行时
               // 返回 INVALID_STATE，经 teardown_then 的 INVALID_STATE 特判
               // 透传（不 teardown，避免杀既有进程），最终经链尾 op->Complete
               // 异步送达。
               | stdexec::then(
                   [this, exe_path = host_exe_path]() -> DasResult
                   {
                       if (IsRunning())
                       {
                           return DAS_E_IPC_INVALID_STATE;
                       }
                       const uint32_t main_pid = static_cast<uint32_t>(
                           boost::process::v2::current_pid());
                       std::vector<std::string> args;
                       args.emplace_back("--main-pid");
                       args.push_back(std::to_string(main_pid));
                       return LaunchProcess(exe_path, args);
                   })
               // Phase 2: ConnectToHost (async)
               | stdexec::let_value(
                   [this, teardown_then](DasResult r) -> stdexec::sender auto
                   {
                       if (DAS::IsFailed(r))
                       {
                           return teardown_then(r);
                       }
                       PhaseAwaitableFn fn =
                           [this]() -> boost::asio::awaitable<DasResult>
                       { co_return co_await ConnectToHostAsync(); };
                       return boost::asio::co_spawn(
                           impl_->io_ctx,
                           std::move(fn),
                           AsioSenderToken{});
                   })
               // Phase 3: WaitForHostReady (async)
               | stdexec::let_value(
                   [this, teardown_then, remaining_ms](
                       DasResult r) -> stdexec::sender auto
                   {
                       if (DAS::IsFailed(r))
                       {
                           return teardown_then(r);
                       }
                       const uint32_t   ms = remaining_ms();
                       PhaseAwaitableFn fn =
                           [this, ms]() -> boost::asio::awaitable<DasResult>
                       { co_return co_await WaitForHostReadyAsync(ms); };
                       return boost::asio::co_spawn(
                           impl_->io_ctx,
                           std::move(fn),
                           AsioSenderToken{});
                   })
               // Phase 4: PerformFullHandshake (async)
               | stdexec::let_value(
                   [this, teardown_then, remaining_ms, session_id](
                       DasResult r) -> stdexec::sender auto
                   {
                       if (DAS::IsFailed(r))
                       {
                           return teardown_then(r);
                       }
                       const uint32_t   ms = remaining_ms();
                       PhaseAwaitableFn fn = [this, session_id, ms]()
                           -> boost::asio::awaitable<DasResult>
                       {
                           co_return co_await PerformFullHandshakeAsync(
                               *session_id,
                               ms);
                       };
                       return boost::asio::co_spawn(
                           impl_->io_ctx,
                           std::move(fn),
                           AsioSenderToken{});
                   })
               // Phase 5: RegisterHostLauncher (sync)
               | stdexec::let_value(
                   [this, teardown_then, session_id](
                       DasResult r) -> stdexec::sender auto
                   {
                       if (DAS::IsFailed(r))
                       {
                           return teardown_then(r);
                       }
                       impl_->session_id = *session_id;
                       const DasResult reg_result =
                           on_register_ ? on_register_() : DAS_S_OK;
                       if (DAS::IsFailed(reg_result))
                       {
                           DAS_CORE_LOG_ERROR(
                               "Registration callback failed: error={}",
                               reg_result);
                           return teardown_then(reg_result);
                       }
                       PhaseAwaitableFn fn =
                           [reg_result]() -> boost::asio::awaitable<DasResult>
                       { co_return reg_result; };
                       return boost::asio::co_spawn(
                           impl_->io_ctx,
                           std::move(fn),
                           AsioSenderToken{});
                   })
               // op 完成兜底：把 set_error / set_stopped 都转成值走链尾，保证
               // op 永不被 orphan（被 Stop()/Cancel()/并发 teardown 中断或链内
               // 异常时 op 也 complete，参考旧 RunAsync 的"总是 complete
               // op"）。 若没有这两层适配，链以 set_stopped 完成时 then
               // 链尾被跳过 → op 卡 DAS_ASYNC_STARTED、OnCompleted
               // 永不触发、GetResults 永远
               // INVALID_STATE。upon_error/upon_stopped 是值型适配（非 let_* 的
               // sender 型），函数返回 DasResult 直接 set_value 送达链尾。
               | stdexec::upon_error(
                   [](std::exception_ptr e) -> DasResult
                   {
                       DAS_CORE_LOG_ERROR(
                           "Async host start chain failed: {}",
                           DescribeException(e));
                       return DAS_E_IPC_CANCELED;
                   })
               | stdexec::upon_stopped(
                   []() -> DasResult { return DAS_E_IPC_CANCELED; })
               // 链尾：alive 守卫（Pitfall 6 / UAF 1 防回归，D-91-02）+
               // 成功路径 exit-watcher 迁入 scope（UAF 2 消除，D-91-02）。
               | stdexec::then(
                   [this, op, alive, session_id](DasResult r)
                   {
                       if (!alive->load(std::memory_order_acquire))
                       {
                           return;
                       }
                       if (DAS::IsOk(r))
                       {
                           if (impl_->process)
                           {
                               auto       cb_session_id = *session_id;
                               auto       process_ptr = impl_->process.get();
                               const auto stop_token =
                                   impl_->scope.get_stop_token();
                               impl_->scope.spawn(
                                   boost::asio::co_spawn(
                                       impl_->io_ctx,
                                       [this,
                                        process_ptr,
                                        cb_session_id,
                                        stop_token]()
                                           -> boost::asio::awaitable<void>
                                       {
                                           // Pitfall 3 守卫（前置检查，保留为
                                           // 防御性）：与 StartLaunchSequence
                                           // watcher 同款。Stop() 不再
                                           // request_stop
                                           // scope，正常路径不触发；
                                           // 保留以防未来任何 stop 源在 watcher
                                           // 注册 async_wait 前 request_stop，
                                           // 否则 async_wait 因进程存活永不完成
                                           // → scope 永不 drain → on_empty
                                           // 死锁。
                                           if (stop_token.stop_requested())
                                           {
                                               co_return;
                                           }
                                           boost::system::error_code ec;
                                           co_await process_ptr->async_wait(
                                               boost::asio::redirect_error(
                                                   boost::asio::use_awaitable,
                                                   ec));
                                           // Pitfall 3 守卫（后置检查，保留为
                                           // 防御性）：正常路径 async_wait
                                           // 因进程 真实退出完成（ec 空）→
                                           // 放行真实 exit_code。仅外部
                                           // request_stop 取消 async_wait 时
                                           // ec=operation_aborted 且
                                           // stop_requested() 为 true，此处跳过
                                           // Invoke 避免伪 -1 回调（Stop 驱动
                                           // teardown 的伪回调抑制由
                                           // StopBodyRequestExit 的
                                           // ClearCallbacks 承担）。
                                           if (stop_token.stop_requested())
                                           {
                                               co_return;
                                           }
                                           const int exit_code =
                                               ec ? -1
                                                  : process_ptr->exit_code();
                                           // 持锁执行 callback（INV-01）
                                           on_process_exit_slot_.Invoke(
                                               cb_session_id,
                                               exit_code);
                                           co_return;
                                       },
                                       AsioSenderToken{})
                                   | stdexec::let_error(
                                       [](std::exception_ptr e)
                                       {
                                           DAS_CORE_LOG_ERROR(
                                               "Host exit-watcher failed: {}",
                                               DescribeException(e));
                                           return stdexec::just_stopped();
                                       }));
                           }
                           op->Complete(r, *session_id);
                       }
                       else
                       {
                           op->Complete(r, 0);
                       }
                   });
    };

    // 启动链 spawn 进 scope。链内 let_error/upon_stopped 已把 error/stopped
    // 转成 值走链尾（op 必 complete），故本层 let_error 是防御性兜底（Pitfall 1
    // / async_scope.hpp:718-736：spawn receiver 对 set_error 是 [[noreturn]]
    // rethrow，裸 error completion 会让 io_context 线程 terminate）。
    impl_->scope.spawn(
        build_chain()
        | stdexec::let_error(
            [](std::exception_ptr e)
            {
                DAS_CORE_LOG_ERROR(
                    "Async host start failed: {}",
                    DescribeException(e));
                return stdexec::just_stopped();
            }));

    *pp_out_operation = op;
    return DAS_S_OK;
}

void HostLauncher::SetOnProcessExit(OnProcessExitCallback callback)
{
    on_process_exit_slot_.Set(std::move(callback));
}

void HostLauncher::SetOnHeartbeatTimeout(OnHeartbeatTimeoutCallback callback)
{
    on_heartbeat_timeout_slot_.Set(std::move(callback));
}

void HostLauncher::SetAssociatedGuid(DasGuid guid)
{
    const std::scoped_lock lock(associated_guid_mutex_);
    associated_guid_ = guid;
}

DasGuid HostLauncher::GetAssociatedGuid() const
{
    const std::scoped_lock lock(associated_guid_mutex_);
    return associated_guid_;
}

void HostLauncher::NotifyHeartbeatTimeout()
{
    // 先持 associated_guid_mutex_ 取 guid 副本后释放，再 Invoke（持 slot
    // 锁执行）。 单一锁职责：slot 锁内不嵌套 guid 锁，避免锁层级耦合。
    DasGuid guid_snapshot{};
    {
        const std::scoped_lock lock(associated_guid_mutex_);
        guid_snapshot = associated_guid_;
    }
    on_heartbeat_timeout_slot_.Invoke(guid_snapshot);
}

void HostLauncher::ResetHostLifecycleCallbacks()
{
    // 只清 Host 生命周期监护事件 slot（心跳超时 + 进程退出），保留 on_register_
    // （握手/注册回调）。持锁 drain = 让心跳线程后续 Invoke 空转，碰不到
    // 悬空 [this]（GuardedCallback::Clear 持
    // callback_mutex_，会等在途回调完成）。 与 ClearCallbacks
    // 职责区分：ClearCallbacks 全清三个 slot，由 IpcContext 析构用（~IpcContext
    // 仍调 ClearCallbacks 做最终收尾）。
    on_process_exit_slot_.Clear();
    on_heartbeat_timeout_slot_.Clear();
}

void HostLauncher::TerminateIfRunning()
{
    // 与 Stop() 共用 stop_mutex_：心跳线程的 TerminateIfRunning 可能与
    // RequestStop 的 Stop() 并发，两者都改写 impl_->process/transport/pool，
    // 需串行化避免跨线程竞争。
    const std::scoped_lock lock(stop_mutex_);

    if (impl_->process)
    {
        boost::system::error_code ec;
        const bool                still_running = impl_->process->running(ec);

        if (still_running)
        {
            DAS_CORE_LOG_INFO(
                "Terminating Host process (heartbeat timeout): pid={}, "
                "session_id={}",
                impl_->pid,
                impl_->session_id);
            impl_->process->terminate(ec);
        }

        // 心跳超时是错误恢复路径，从非 io_context 线程调用。
        // 用 release() 而非 reset()：reset() 会触发析构 → UnregisterWaitEx，
        // 但 io_context 线程可能还有 pending 的 async_wait completion，
        // 导致 process handle UAF。release() 泄漏 handle 但避免竞争。
        // 正常 Stop() 路径（从主线程调用、io_context 即将停止）用 reset()。
        (void)impl_->process.release();
    }

    // 与 Stop() 相同：transport 清理串行化到 io_context 线程，避免与
    // pending ReceiveCoroutine 跨线程并发。仅在 transport 已连接时才有
    // io_context 线程协程借用；未连接/无 transport（如单测本地 io_context
    // 从未 run）时直接清理，避免 CleanupTransportAsync 阻塞在永不执行的
    // co_spawn 上。心跳线程在 io_context_ 析构前已被 join（~IpcRunLoop 先
    // StopHeartbeatThread 再 reset io_context_），因此此处 io_context 必存活，
    // stopped() 可安全判断。
    if (!impl_->io_ctx.stopped() && impl_->async_transport
        && impl_->async_transport->IsConnected())
    {
        try
        {
            CleanupTransportAsync().get();
        }
        catch (const std::exception& e)
        {
            const std::string msg = DAS_FMT_NS::format(
                "HostLauncher::TerminateIfRunning transport cleanup failed: {}",
                ToString(e.what()));
            DAS_CORE_LOG_ERROR(msg.c_str());
        }
    }
    else
    {
        CleanupTransport(impl_->async_transport);
    }

    // 清理 SharedMemoryPool（错误恢复路径）
    impl_->pool_.reset();

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
    if (iid == DasIidOf<IHostConnection>())
    {
        AddRef();
        *pp = static_cast<IHostConnection*>(this);
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
    const std::string&                             exe_path,
    const std::vector<std::string>&                args,
    const std::optional<std::string>&              working_directory,
    const std::optional<std::vector<std::string>>& environment)
{
    try
    {
        const auto launch_working_directory =
            working_directory.value_or(DefaultWorkingDirectoryFor(exe_path));

        if (environment)
        {
            boost::process::v2::process_environment child_environment{
                *environment};
            impl_->process = std::make_unique<boost::process::v2::process>(
                impl_->io_ctx,
                exe_path,
                args,
                boost::process::v2::process_start_dir(launch_working_directory),
                std::move(child_environment));
        }
        else
        {
            impl_->process = std::make_unique<boost::process::v2::process>(
                impl_->io_ctx,
                exe_path,
                args,
                boost::process::v2::process_start_dir(
                    launch_working_directory));
        }

        impl_->pid = static_cast<uint32_t>(impl_->process->id());
        impl_->is_running = true;

        const std::string msg =
            DAS_FMT_NS::format("Host process launched: PID={}", impl_->pid);
        DAS_LOG_INFO(msg.c_str());

        return DAS_S_OK;
    }
    catch (const boost::system::system_error& e)
    {
        const std::string msg = DAS_FMT_NS::format(
            "Exception launching Host process (system_error): {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }
    catch (const std::exception& e)
    {
        const std::string msg = DAS_FMT_NS::format(
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
    const uint32_t main_pid =
        static_cast<uint32_t>(boost::process::v2::current_pid());
    const uint32_t host_pid = impl_->pid;

    std::string host_to_plugin_pipe =
        Host::MakeMessageQueueName(main_pid, host_pid, false);
    std::string plugin_to_host_pipe =
        Host::MakeMessageQueueName(main_pid, host_pid, true);

    const std::string msg = DAS_FMT_NS::format(
        "Creating pipes for Host IPC: {}, {} (MainProcess is server)",
        host_to_plugin_pipe,
        plugin_to_host_pipe);
    DAS_LOG_INFO(msg.c_str());

    // 创建 SharedMemoryPool（MainProcess 端使用 Create 模式）
    std::string shm_name = Host::MakeSharedMemoryName(main_pid, host_pid);
    try
    {
        impl_->pool_ = std::make_unique<SharedMemoryPool>(
            shm_name,
            Host::DEFAULT_SHARED_MEMORY_SIZE);
        DAS_LOG_INFO(
            DAS_FMT_NS::format(
                "HostLauncher: Created SHM pool: {}, size={}",
                shm_name,
                Host::DEFAULT_SHARED_MEMORY_SIZE)
                .c_str());
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_ERROR(
            "HostLauncher: Failed to create SHM pool: {}",
            e.what());
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

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
                65536,
                impl_->pool_.get()),
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
            const std::string err_msg = DAS_FMT_NS::format(
                "Failed to create Host IPC transport: error={}",
                transport_result);
            DAS_CORE_LOG_ERROR(err_msg.c_str());
            return transport_result;
        }

        impl_->async_transport = std::move(*maybe_transport);
    }
    catch (const boost::system::system_error& e)
    {
        const std::string err_msg = DAS_FMT_NS::format(
            "Failed to create Host IPC transport (system_error): {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(err_msg.c_str());
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }
    catch (const std::exception& e)
    {
        const std::string err_msg = DAS_FMT_NS::format(
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

    const std::string msg = DAS_FMT_NS::format(
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

    uint32_t my_pid = static_cast<uint32_t>(boost::process::v2::current_pid());

    // session_id 已在构造时预分配
    const uint16_t assigned_session_id = session_id_;
    if (assigned_session_id == 0)
    {
        DAS_CORE_LOG_ERROR("Invalid pre-allocated session_id (0)");
        co_return DAS_E_IPC_SESSION_ALLOC_FAILED;
    }

    HelloRequestV1 hello;
    InitHelloRequest(hello, my_pid, client_name.c_str());
    hello.assigned_session_id = assigned_session_id;

    constexpr uint16_t MAIN_PROCESS_SESSION_ID = 1;

    // V3: 16-bit call_id
    const uint16_t call_id = impl_->next_call_id++;
    if (impl_->next_call_id == 0)
    {
        impl_->next_call_id = 1; // 溢出后从 1 开始
    }
    auto validated_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::Request)
            .SetControlPlaneCommand(HandshakeInterfaceId::HandshakeIfaceHello)
            .SetBodySize(sizeof(hello))
            .SetCallId(call_id)
            .SetSourceSessionId(MAIN_PROCESS_SESSION_ID)
            .SetTargetSessionId(assigned_session_id)
            .Build();

    // 直接使用协程方法，不再用 sync_wait
    DasResult result = co_await impl_->async_transport->SendCoroutine(
        validated_header,
        reinterpret_cast<const uint8_t*>(&hello),
        sizeof(hello));

    if (result != DAS_S_OK)
    {
        const std::string msg =
            DAS_FMT_NS::format("Failed to send Hello: error={}", result);
        DAS_CORE_LOG_ERROR(msg.c_str());
        co_return result;
    }

    const std::string info_msg =
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
        DasResult         error_code = std::get<0>(result_variant);
        const std::string msg = DAS_FMT_NS::format(
            "Failed to receive Welcome: error={}",
            error_code);
        DAS_CORE_LOG_ERROR(msg.c_str());
        co_return error_code;
    }

    auto&& [header, body] = std::get<1>(result_variant);

    const IPCMessageHeader& raw_header = header.Raw();

    if (raw_header.interface_id
        != static_cast<uint32_t>(HandshakeInterfaceId::HandshakeIfaceWelcome))
    {
        const std::string msg = DAS_FMT_NS::format(
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
        const std::string msg =
            DAS_FMT_NS::format("Welcome status error: {}", welcome.status);
        DAS_CORE_LOG_ERROR(msg.c_str());
        co_return DAS_E_IPC_HANDSHAKE_FAILED;
    }

    out_session_id = welcome.session_id;

    const std::string info_msg = DAS_FMT_NS::format(
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

    constexpr uint16_t MAIN_PROCESS_SESSION_ID = 1;

    // V3: 16-bit call_id
    const uint16_t call_id = impl_->next_call_id++;
    if (impl_->next_call_id == 0)
    {
        impl_->next_call_id = 1; // 溢出后从 1 开始
    }
    auto validated_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::Request)
            .SetControlPlaneCommand(HandshakeInterfaceId::HandshakeIfaceReady)
            .SetBodySize(sizeof(ready))
            .SetCallId(call_id)
            .SetSourceSessionId(MAIN_PROCESS_SESSION_ID)
            .SetTargetSessionId(session_id)
            .Build();

    DasResult result = co_await impl_->async_transport->SendCoroutine(
        validated_header,
        reinterpret_cast<const uint8_t*>(&ready),
        sizeof(ready));

    if (result != DAS_S_OK)
    {
        const std::string msg =
            DAS_FMT_NS::format("Failed to send Ready: error={}", result);
        DAS_CORE_LOG_ERROR(msg.c_str());
        co_return result;
    }

    const std::string info_msg =
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
        DasResult         error_code = std::get<0>(result_variant);
        const std::string msg = DAS_FMT_NS::format(
            "Failed to receive ReadyAck: error={}",
            error_code);
        DAS_CORE_LOG_ERROR(msg.c_str());
        co_return error_code;
    }

    auto&& [header, body] = std::get<1>(result_variant);

    const IPCMessageHeader& raw_header = header.Raw();

    if (raw_header.interface_id
        != static_cast<uint32_t>(HandshakeInterfaceId::HandshakeIfaceReadyAck))
    {
        const std::string msg = DAS_FMT_NS::format(
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
        const std::string msg =
            DAS_FMT_NS::format("ReadyAck status error: {}", ack.status);
        DAS_CORE_LOG_ERROR(msg.c_str());
        co_return DAS_E_IPC_HANDSHAKE_FAILED;
    }

    const std::string info_msg =
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

    const std::string msg = DAS_FMT_NS::format(
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
        const std::string msg = DAS_FMT_NS::format(
            "SendHandshakeHello failed (system_error): {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_SEND_FAILED;
    }
    catch (const std::exception& e)
    {
        const std::string msg = DAS_FMT_NS::format(
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
        const std::string msg = DAS_FMT_NS::format(
            "ReceiveHandshakeWelcome failed (system_error): {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_RECEIVE_FAILED;
    }
    catch (const std::exception& e)
    {
        const std::string msg = DAS_FMT_NS::format(
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
        const std::string msg = DAS_FMT_NS::format(
            "SendHandshakeReady failed (system_error): {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_SEND_FAILED;
    }
    catch (const std::exception& e)
    {
        const std::string msg = DAS_FMT_NS::format(
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
        const std::string msg = DAS_FMT_NS::format(
            "ReceiveHandshakeReadyAck failed (system_error): {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_RECEIVE_FAILED;
    }
    catch (const std::exception& e)
    {
        const std::string msg = DAS_FMT_NS::format(
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
    const uint32_t main_pid =
        static_cast<uint32_t>(boost::process::v2::current_pid());
    const uint32_t host_pid = impl_->pid;

    std::string host_to_plugin_pipe =
        Host::MakeMessageQueueName(main_pid, host_pid, false);
    std::string plugin_to_host_pipe =
        Host::MakeMessageQueueName(main_pid, host_pid, true);

    const std::string msg = DAS_FMT_NS::format(
        "Creating pipes for Host IPC: {}, {} (MainProcess is server)",
        host_to_plugin_pipe,
        plugin_to_host_pipe);
    DAS_LOG_INFO(msg.c_str());

    // 创建 SharedMemoryPool（MainProcess 端使用 Create 模式）
    std::string shm_name = Host::MakeSharedMemoryName(main_pid, host_pid);
    try
    {
        impl_->pool_ = std::make_unique<SharedMemoryPool>(
            shm_name,
            Host::DEFAULT_SHARED_MEMORY_SIZE);
        DAS_LOG_INFO(
            DAS_FMT_NS::format(
                "HostLauncher: Created SHM pool: {}, size={}",
                shm_name,
                Host::DEFAULT_SHARED_MEMORY_SIZE)
                .c_str());
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_ERROR(
            "HostLauncher: Failed to create SHM pool: {}",
            e.what());
        co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    try
    {
        auto [transport_result, maybe_transport] =
            co_await AnyTransport::CreateAsync(
                impl_->io_ctx,
                host_to_plugin_pipe,
                plugin_to_host_pipe,
                true, // is_server = true (MainProcess creates pipes)
                65536,
                impl_->pool_.get());

        if (DAS::IsFailed(transport_result) || !maybe_transport)
        {
            const std::string err_msg = DAS_FMT_NS::format(
                "Failed to create Host IPC transport: error = {}",
                transport_result);
            DAS_CORE_LOG_ERROR(err_msg.c_str());
            co_return transport_result;
        }

        impl_->async_transport = std::move(*maybe_transport);
    }
    catch (const boost::system::system_error& e)
    {
        const std::string err_msg = DAS_FMT_NS::format(
            "Failed to create Host IPC transport (system_error): {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(err_msg.c_str());
        co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }
    catch (const std::exception& e)
    {
        const std::string err_msg = DAS_FMT_NS::format(
            "Failed to create Host IPC transport: {}",
            ToString(e.what()));
        DAS_CORE_LOG_ERROR(err_msg.c_str());
        co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    DAS_LOG_INFO("Host IPC pipes created and Host connected successfully");
    co_return DAS_S_OK;
}

DAS_CORE_IPC_NS_END
