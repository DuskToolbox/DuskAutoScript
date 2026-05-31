#include <IpcProxyFactory.h>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/process/v2/pid.hpp>
#include <das/Core/IPC/BusinessControlRequestRaw.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <das/Core/IPC/DasProxyBase.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/HandshakeSerialization.h>
#include <das/Core/IPC/Host/HandshakeHandler.h>
#include <das/Core/IPC/Host/HostConfig.h>
#include <das/Core/IPC/Host/IIpcContext.h>
#include <das/Core/IPC/Host/IpcContext.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/IpcTransport.h>
#include <das/Core/IPC/ManualProxyRegistry.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/IDasAsyncCallback.h>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace
{
    /**
     * @brief 跨平台检测指定 PID 的进程是否存活
     * @param pid 目标进程 PID
     * @return true 表示进程存在，false 表示进程已退出
     */
    bool IsProcessAlive(uint32_t pid)
    {
        if (pid == 0)
        {
            return false;
        }
#ifdef _WIN32
        // 首先尝试用 SYNCHRONIZE 权限打开进程，这样可以等待进程
        HANDLE process = OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            static_cast<DWORD>(pid));
        if (process == nullptr)
        {
            // 无法打开进程句柄，说明进程不存在或无权限
            DWORD err = GetLastError();
            DAS_CORE_LOG_WARN(
                "IsProcessAlive: OpenProcess failed for pid={}, error={}",
                pid,
                err);
            return false;
        }

        // 使用 WaitForSingleObject 检测进程是否仍在运行
        // 0 超时意味着立即返回，不等待
        DWORD wait_result = WaitForSingleObject(process, 0);
        CloseHandle(process);

        if (wait_result == WAIT_TIMEOUT)
        {
            // 进程仍在运行（等待超时意味着进程没有结束）
            return true;
        }
        else if (wait_result == WAIT_OBJECT_0)
        {
            // 进程已经退出
            DAS_CORE_LOG_WARN(
                "IsProcessAlive: Process pid={} has exited (WAIT_OBJECT_0)",
                pid);
            return false;
        }
        else
        {
            // 其他错误
            DAS_CORE_LOG_WARN(
                "IsProcessAlive: WaitForSingleObject failed for pid={}, result={}",
                pid,
                wait_result);
            return false;
        }
#else
        // Linux/macOS: kill(pid, 0) 检查进程是否存在
        return kill(static_cast<pid_t>(pid), 0) == 0;
#endif
    }

    // 父进程存活检测间隔（毫秒）
    constexpr uint32_t PARENT_PROCESS_CHECK_INTERVAL_MS = 1000;

    constexpr uint32_t BEFORE_SHUTDOWN_TIMEOUT_MS = 2000;

    bool HasText(const char* value) noexcept
    {
        return value != nullptr && value[0] != '\0';
    }
} // namespace

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        namespace Host
        {
            // ====== IpcContext 实现（无 pimpl）======

            IpcContext::IpcContext(const IpcContextConfig& config)
                : config_(config), proxy_factory_(std::in_place),
                  run_loop_(false, &inbound_queue_, *proxy_factory_, registry_)
            {
                // 获取当前进程 PID
                host_pid_ =
                    static_cast<uint32_t>(boost::process::v2::current_pid());

                if (config_.main_pid == 0 && !HasText(config_.connect_url))
                {
                    throw std::invalid_argument(
                        "IpcContext: main_pid 不能为 0 且 connect_url 为空，"
                        "必须指定 main_pid 或 connect_url");
                }

                main_pid_ = config_.main_pid;

                std::string msg = DAS_FMT_NS::format(
                    "IpcContext: Host 模式初始化，main_pid={}, host_pid={}",
                    main_pid_,
                    host_pid_);
                DAS_LOG_INFO(msg.c_str());

                // session_id 初始为 0，握手时由 HandshakeHandler 分配
                session_id_ = 0;

                // 创建 Host 专用队列名称（新格式）
                host_read_queue_ =
                    MakeMessageQueueName(main_pid_, host_pid_, true);
                host_write_queue_ =
                    MakeMessageQueueName(main_pid_, host_pid_, false);
                host_is_server_ =
                    false; // Host 进程是客户端，连接 MainProcess 创建的管道
                std::string shm_name =
                    MakeSharedMemoryName(main_pid_, host_pid_);

                msg = DAS_FMT_NS::format(
                    "IpcContext: Creating Host resources (async mode), M2H={}, H2M={}, SHM={}",
                    host_read_queue_,
                    host_write_queue_,
                    shm_name);
                DAS_LOG_INFO(msg.c_str());

                // 初始化 SharedMemoryPool（optional，因为 shm_name 在此处计算）
                shared_memory_.emplace(shm_name, DEFAULT_SHARED_MEMORY_SIZE);

                proxy_factory_->GetObjectManager().SetSessionId(session_id_);
                run_loop_.SetSessionId(session_id_);

                // 创建 BusinessThread（构造即启动线程）
                business_thread_ = std::make_shared<BusinessThread>(
                    inbound_queue_,
                    run_loop_,
                    *this,
                    *proxy_factory_,
                    registry_);

                // transport 由 ConnectionManager 拥有，接收循环通过
                // FindTransport 在需要时解析 AnyTransport 引用。

                // 创建并初始化 IpcCommandHandler
                command_handler_ = IpcCommandHandler::Create(registry_);
                command_handler_->SetSessionId(session_id_);

                // 创建 HandshakeHandler（使用 DasPtr 管理生命周期）
                handshake_handler_ = HandshakeHandler::Create(session_id_);
                if (!handshake_handler_)
                {
                    throw std::runtime_error(
                        "IpcContext: HandshakeHandler::Create failed");
                }

                // 设置 HandshakeHandler 的客户端连接回调
                // 握手完成后，同步 session_id 到 IpcCommandHandler 和
                // IpcRunLoop
                handshake_handler_->SetOnClientConnected(
                    [this](const ConnectedClient& client)
                    {
                        session_id_ = client.session_id;
                        if (command_handler_)
                        {
                            command_handler_->SetSessionId(client.session_id);
                        }
                        run_loop_.SetSessionId(client.session_id);
                        if (proxy_factory_)
                        {
                            proxy_factory_->GetObjectManager().SetSessionId(
                                client.session_id);
                        }

                        is_connected_ = true;
                    });

                handshake_handler_->SetOnClientDisconnected(
                    [this](uint16_t /*session_id*/) { is_connected_ = false; });

                // 收到 GOODBYE 时请求进程退出
                handshake_handler_->SetOnShutdownRequested(
                    [this]()
                    {
                        std::string msg = DAS_FMT_NS::format(
                            "IpcContext: 收到 GOODBYE，请求退出");
                        DAS_LOG_INFO(msg.c_str());

                        RequestShutdown(HOST_SHUTDOWN_REASON_GOODBYE);
                    });

                // 注册消息处理器
                // HandshakeHandler 处理所有控制平面消息（协程版本）
                // 注册为 CONTROL_PLANE 标志，按 interface_id 路由
                // HELLO (interface_id=1)
                run_loop_.RegisterHandler(
                    HeaderFlags::CONTROL_PLANE,
                    static_cast<uint32_t>(
                        HandshakeInterfaceId::HANDSHAKE_IFACE_HELLO),
                    handshake_handler_.Get());
                // READY (interface_id=3)
                run_loop_.RegisterHandler(
                    HeaderFlags::CONTROL_PLANE,
                    static_cast<uint32_t>(
                        HandshakeInterfaceId::HANDSHAKE_IFACE_READY),
                    handshake_handler_.Get());
                // HEARTBEAT (interface_id=6)
                run_loop_.RegisterHandler(
                    HeaderFlags::CONTROL_PLANE,
                    static_cast<uint32_t>(
                        HandshakeInterfaceId::HANDSHAKE_IFACE_HEARTBEAT),
                    handshake_handler_.Get());
                // GOODBYE (interface_id=7)
                run_loop_.RegisterHandler(
                    HeaderFlags::CONTROL_PLANE,
                    static_cast<uint32_t>(
                        HandshakeInterfaceId::HANDSHAKE_IFACE_GOODBYE),
                    handshake_handler_.Get());

                // REMOTE_RELEASE (fire-and-forget EVENT)
                run_loop_.RegisterHandler(
                    HeaderFlags::BUSINESS_CONTROL,
                    static_cast<uint32_t>(IpcCommandType::REMOTE_RELEASE),
                    command_handler_.Get());

                // RELEASE_SHM_BLOCK (fire-and-forget EVENT)
                run_loop_.RegisterHandler(
                    HeaderFlags::BUSINESS_CONTROL,
                    static_cast<uint32_t>(IpcCommandType::RELEASE_SHM_BLOCK),
                    command_handler_.Get());

                // HTTP/WebSocket transport mode
                if (HasText(config_.connect_url))
                {
                    DAS_CORE_LOG_INFO(
                        "IpcContext: HTTP/WebSocket transport mode enabled");

                    // 解析 URL: ws://host:port 或 http://host:port
                    std::string url = config_.connect_url;
                    if (url.starts_with("ws://"))
                    {
                        url = url.substr(5);
                    }
                    else if (url.starts_with("http://"))
                    {
                        url = url.substr(7);
                    }

                    auto colon_pos = url.find(':');
                    if (colon_pos != std::string::npos)
                    {
                        http_host_ = url.substr(0, colon_pos);
                        http_port_ = url.substr(colon_pos + 1);
                    }
                    else
                    {
                        http_host_ = url;
                        http_port_ = "9527";
                    }

                    DAS_CORE_LOG_INFO(
                        "  parsed: host={}, port={}",
                        http_host_,
                        http_port_);

                    use_http_transport_ = true;
                }

                msg = DAS_FMT_NS::format(
                    "IpcContext: Host 模式初始化完成，等待主进程连接分配 session_id");
                DAS_LOG_INFO(msg.c_str());
            }

            IpcContext::~IpcContext()
            {
                if (proxy_factory_)
                {
                    // Flip the runtime token before tearing down transport,
                    // run loop, and proxy tables.
                    proxy_factory_->BeginShutdown();
                }

                // 停止父进程监控线程
                StopParentProcessMonitor();

                // 1. 关闭入站队列，让业务线程的 Pop() 返回 nullopt
                inbound_queue_.Uninitialize();

                // 2. 停止业务线程（会 join）
                if (business_thread_)
                {
                    business_thread_->Stop();
                    business_thread_.reset();
                }

                // 3. 停止事件循环（必须在 transport 析构之前）
                //    确保 io_context 已停止，这样 AsyncMutex 析构时走安全路径
                run_loop_.RequestStop();

                // 4. 关闭 HTTP 客户端（如果存在）
                http_client_.reset();

                // 5. IpcRunLoop 是值成员，析构时自动清理
                //    必须在关闭 HandshakeHandler 之前，因为 handlers_by_flags_
                //    中存储的 DasPtr 会调用 handler->Release()

                // 6. HandshakeHandler 和 CommandHandler 使用 DasPtr 管理
                //    析构时自动减少引用计数，无需手动 reset
                //    注意：必须确保 run_loop_ 先析构，这样 handlers_ 中的
                //    DasPtr 会先释放

                // 7. 关闭 SharedMemoryPool（optional，reset 清除）
                shared_memory_.reset();

                // 8. proxy_factory_ 是 optional 值成员，自动析构

                is_connected_ = false;
            }

            void IpcContext::StartParentProcessMonitor()
            {
                if (main_pid_ == 0)
                {
                    return;
                }

                parent_monitor_running_.store(true);
                parent_monitor_thread_ = std::thread(
                    [this]()
                    {
                        std::string start_msg = DAS_FMT_NS::format(
                            "IpcContext: 父进程监控线程启动，监控 main_pid={}",
                            main_pid_);
                        DAS_LOG_INFO(start_msg.c_str());

                        while (parent_monitor_running_.load())
                        {
                            if (!IsProcessAlive(main_pid_))
                            {
                                std::string msg = DAS_FMT_NS::format(
                                    "IpcContext: 检测到主进程 (PID={}) 已退出，Host 将自动退出",
                                    main_pid_);
                                DAS_LOG_INFO(msg.c_str());

                                // 请求 RunLoop 停止
                                RequestShutdown(
                                    HOST_SHUTDOWN_REASON_PARENT_PROCESS_EXITED);
                                break;
                            }

                            // 等待一段时间后再次检测
                            for (uint32_t i = 0;
                                 i < PARENT_PROCESS_CHECK_INTERVAL_MS / 100
                                 && parent_monitor_running_.load();
                                 ++i)
                            {
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(100));
                            }
                        }

                        std::string stop_msg = DAS_FMT_NS::format(
                            "IpcContext: 父进程监控线程结束");
                        DAS_LOG_INFO(stop_msg.c_str());
                    });
            }

            void IpcContext::StopParentProcessMonitor()
            {
                parent_monitor_running_.store(false);
                if (parent_monitor_thread_.joinable())
                {
                    parent_monitor_thread_.join();
                }
            }

            IpcRunLoop& IpcContext::GetRunLoop() { return run_loop_; }

            IpcCommandHandler& IpcContext::GetCommandHandler()
            {
                return *command_handler_;
            }

            IDistributedObjectManager& IpcContext::GetObjectManager()
            {
                return proxy_factory_->GetObjectManager();
            }

            void IpcContext::RegisterCommandHandler(
                uint32_t       cmd_type,
                CommandHandler handler)
            {
                command_handler_->RegisterHandler(
                    static_cast<IpcCommandType>(cmd_type),
                    handler);
                // IpcContext 同时拥有 run_loop_ 和 command_handler_，由它注册
                run_loop_.RegisterHandler(
                    HeaderFlags::BUSINESS_CONTROL,
                    cmd_type,
                    command_handler_.Get());
            }

            DasResult IpcContext::Run()
            {
                ScopedCurrentIpcContext scope(this);

                if (is_running_)
                {
                    return DAS_S_OK;
                }

                is_running_ = true;
                StartParentProcessMonitor();

                auto& io = run_loop_.GetIoContext();

                // post 异步连接任务到 io_context
                // 这样连接操作会在 io_context 开始运行后执行
                if (use_http_transport_)
                {
                    boost::asio::post(
                        io,
                        [this, &io]()
                        {
                            boost::asio::co_spawn(
                                io,
                                ConnectHttp(),
                                boost::asio::detached);
                        });
                }
                else
                {
                    // Named Pipe 模式
                    boost::asio::post(
                        io,
                        [this, &io]()
                        {
                            boost::asio::co_spawn(
                                io,
                                [this]() -> boost::asio::awaitable<void>
                                {
                                    try
                                    {
                                        DAS_CORE_LOG_INFO(
                                            "Host: async connecting...");
                                        DAS_CORE_LOG_INFO(
                                            "  read: {}",
                                            host_read_queue_);
                                        DAS_CORE_LOG_INFO(
                                            "  write: {}",
                                            host_write_queue_);

                                        // 1. 创建并异步连接 transport
                                        auto pipe_transport =
                                            AnyTransport::CreateUninitialized(
                                                run_loop_.GetIoContext());
                                        pipe_transport.SetSharedMemoryPool(
                                            &*shared_memory_);

                                        auto result = co_await pipe_transport
                                                          .InitializeAsync(
                                                              host_read_queue_,
                                                              host_write_queue_,
                                                              host_is_server_);

                                        if (DAS::IsFailed(result))
                                        {
                                            DAS_CORE_LOG_ERROR(
                                                "Host: connect failed: "
                                                "{}",
                                                result);
                                            run_loop_.RequestStop();
                                            co_return;
                                        }

                                        // 2. 将所有权转移至
                                        // ConnectionManager，借用引用
                                        auto& conn_mgr =
                                            run_loop_.GetConnectionManager();
                                        uint16_t main_process_session_id = 1;
                                        conn_mgr.RegisterHostLocalTransport(
                                            main_process_session_id,
                                            std::move(pipe_transport));

                                        DAS_CORE_LOG_INFO(
                                            "Host: connected, starting "
                                            "receive loop");

                                        // 3. 启动接收循环协程
                                        co_await ReceiveLoopCoroutine();
                                    }
                                    catch (const boost::system::system_error& e)
                                    {
                                        DAS_CORE_LOG_ERROR(
                                            "Host: system_error: {}",
                                            ToString(e.what()));
                                        run_loop_.RequestStop();
                                    }
                                    catch (const std::exception& e)
                                    {
                                        DAS_CORE_LOG_ERROR(
                                            "Host: exception: {}",
                                            ToString(e.what()));
                                        run_loop_.RequestStop();
                                    }
                                },
                                boost::asio::detached);
                        });
                }

                // 运行 io_context（连接任务已 post）
                DasResult result = run_loop_.Run();

                is_running_ = false;
                StopParentProcessMonitor();
                return result;
            }

            boost::asio::awaitable<void> IpcContext::ReceiveLoopCoroutine()
            {
                DAS_CORE_LOG_INFO(
                    "ReceiveLoopCoroutine: starting, run_loop_.IsRunning()={}",
                    run_loop_.IsRunning());
                while (run_loop_.IsRunning())
                {
                    try
                    {
                        DAS_CORE_LOG_INFO(
                            "ReceiveLoopCoroutine: waiting for message...");
                        auto& conn_mgr = run_loop_.GetConnectionManager();
                        constexpr uint16_t main_process_session_id = 1;
                        auto [lookup_result, maybe_transport] =
                            conn_mgr.FindHostLocalTransport(
                                main_process_session_id);
                        if (DAS::IsFailed(lookup_result) || !maybe_transport)
                        {
                            DAS_CORE_LOG_ERROR(
                                "ReceiveLoopCoroutine: transport lookup failed, result={}",
                                lookup_result);
                            RequestShutdown(
                                HOST_SHUTDOWN_REASON_TRANSPORT_DISCONNECTED);
                            co_return;
                        }

                        AnyTransport& transport = maybe_transport->get();
                        auto result = co_await transport.ReceiveCoroutine();
                        DAS_CORE_LOG_INFO(
                            "ReceiveLoopCoroutine: received result, index={}",
                            result.index());

                        if (!run_loop_.IsRunning())
                        {
                            co_return;
                        }

                        if (result.index() == 0)
                        {
                            DasResult error = std::get<0>(result);
                            if (error != DAS_S_OK)
                            {
                                DAS_CORE_LOG_ERROR("Receive failed: {}", error);
                            }
                            RequestShutdown(
                                HOST_SHUTDOWN_REASON_TRANSPORT_DISCONNECTED);
                            co_return;
                        }

                        // 成功接收消息
                        auto&& [header, body] = std::get<1>(result);

                        co_await run_loop_.RouteIncomingMessage(
                            header,
                            std::move(body));
                    }
                    catch (const boost::system::system_error& e)
                    {
                        if (e.code() == boost::asio::error::operation_aborted
                            || e.code() == boost::asio::error::eof)
                        {
                            DAS_CORE_LOG_DEBUG(
                                "Receive loop stopped: {}",
                                ToString(e.what()));
                            if (e.code() == boost::asio::error::eof)
                            {
                                RequestShutdown(
                                    HOST_SHUTDOWN_REASON_TRANSPORT_DISCONNECTED);
                            }
                        }
                        else
                        {
                            DAS_CORE_LOG_ERROR(
                                "Receive error: {}",
                                ToString(e.what()));
                            RequestShutdown(
                                HOST_SHUTDOWN_REASON_TRANSPORT_DISCONNECTED);
                        }
                        co_return;
                    }
                }
            }

            boost::asio::awaitable<void> IpcContext::ConnectHttp()
            {
                try
                {
                    DAS_CORE_LOG_INFO(
                        "Host: HTTP connecting to {}:{}",
                        http_host_,
                        http_port_);

                    auto connect_result = co_await HttpIpcClient::Connect(
                        run_loop_.GetIoContext(),
                        http_host_,
                        http_port_,
                        host_pid_);

                    if (auto* err = std::get_if<DasResult>(&connect_result))
                    {
                        DAS_CORE_LOG_ERROR("HTTP connect failed: {}", *err);
                        run_loop_.RequestStop();
                        co_return;
                    }

                    http_client_ = std::move(
                        std::get<std::unique_ptr<HttpIpcClient>>(
                            connect_result));
                    session_id_ = http_client_->GetSessionId();

                    DAS_CORE_LOG_INFO(
                        "HTTP connected: session_id={}",
                        session_id_);

                    // 同步 session_id 到各组件
                    if (command_handler_)
                    {
                        command_handler_->SetSessionId(session_id_);
                    }
                    run_loop_.SetSessionId(session_id_);
                    if (proxy_factory_)
                    {
                        proxy_factory_->GetObjectManager().SetSessionId(
                            session_id_);
                    }

                    // 将 HTTP transport 转移至 ConnectionManager，借用引用
                    auto http_transport = http_client_->ReleaseTransport();
                    if (http_transport)
                    {
                        auto&    conn_mgr = run_loop_.GetConnectionManager();
                        uint16_t main_session_id = 1;
                        conn_mgr.RegisterHostLocalTransport(
                            main_session_id,
                            std::move(*http_transport));
                        http_transport.reset();

                        DAS_CORE_LOG_INFO(
                            "HTTP transport registered for "
                            "MainProcess session_id={}",
                            main_session_id);

                        // 启动统一接收循环
                        co_await ReceiveLoopCoroutine();
                    }

                    is_connected_ = true;
                }
                catch (const boost::system::system_error& e)
                {
                    DAS_CORE_LOG_ERROR(
                        "Host HTTP: system_error: {}",
                        ToString(e.what()));
                    run_loop_.RequestStop();
                }
                catch (const std::exception& e)
                {
                    DAS_CORE_LOG_ERROR(
                        "Host HTTP: exception: {}",
                        ToString(e.what()));
                    run_loop_.RequestStop();
                }
            }

            void IpcContext::RequestStop()
            {
                InvokeOnBeforeShutdown(HOST_SHUTDOWN_REASON_REQUEST_STOP);

                if (proxy_factory_)
                {
                    proxy_factory_->BeginShutdown();
                }

                // 1. 请求 runloop 停止；ConnectionManager 拥有的 transport 会在
                //    run_loop_ 析构时自动清理。
                run_loop_.RequestStop();
            }

            void IpcContext::InvokeOnBeforeShutdown(HostShutdownReason reason)
            {
                if (before_shutdown_invoked_.exchange(
                        true,
                        std::memory_order_acq_rel))
                {
                    return;
                }

                const auto& events = config_.events;
                if (!events.on_before_shutdown)
                {
                    return;
                }

                const DasResult result = events.on_before_shutdown(
                    events.p_on_before_shutdown_context,
                    reason,
                    BEFORE_SHUTDOWN_TIMEOUT_MS);
                if (DAS::IsFailed(result))
                {
                    DAS_CORE_LOG_WARN(
                        "IpcContext: OnBeforeShutdown returned error={}",
                        result);
                }
            }

            void IpcContext::RequestShutdown(HostShutdownReason reason)
            {
                InvokeOnBeforeShutdown(reason);

                if (proxy_factory_)
                {
                    proxy_factory_->BeginShutdown();
                }

                if (run_loop_.IsRunning())
                {
                    run_loop_.RequestStop();
                }
            }

            bool IpcContext::IsConnected() const { return is_connected_; }

            void IpcContext::PostCallback(IDasAsyncCallback* callback)
            {
                if (!callback)
                {
                    return;
                }

                // DasPtr 在 post 之前获取所有权（AddRef），保证生命周期安全
                DasPtr<IDasAsyncCallback> ptr(callback);
                boost::asio::post(
                    run_loop_.GetIoContext(),
                    [ptr = std::move(ptr)]() { ptr->Do(); });
            }

            DasResult IpcContext::ResolveMainProcessInterface(
                const DasGuid& iid,
                IDasBase**     pp_out_object)
            {
                if (!pp_out_object)
                {
                    return DAS_E_INVALID_ARGUMENT;
                }
                DAS::DasOutPtr<IDasBase> out_object(pp_out_object);

                if (!business_thread_)
                {
                    DAS_CORE_LOG_ERROR(
                        "IpcRunLoop or BusinessThread not initialized");
                    return DAS_E_IPC_NOT_INITIALIZED;
                }

                DAS_CORE_LOG_TRACE("Resolving main process interface");

                // Build LOOKUP_BY_INTERFACE request body
                LookupByInterfacePayload lookup_payload{.iid = iid};
                std::vector<uint8_t>     response;

                DasResult result = SendBusinessControlRequestRaw(
                    run_loop_,
                    business_thread_,
                    session_id_,
                    1, // target_session_id = main process
                    IpcCommandType::LOOKUP_BY_INTERFACE,
                    reinterpret_cast<const uint8_t*>(&lookup_payload),
                    sizeof(lookup_payload),
                    response);

                if (DAS::IsFailed(result))
                {
                    DAS_CORE_LOG_ERROR(
                        "Failed to resolve main process interface, "
                        "error = {}",
                        static_cast<int>(result));
                    return result;
                }

                // Parse response as ObjectInfoResponsePayload
                // Layout: ObjectId(8) + DasGuid(16) + session_id(2) +
                // version(2)
                // + name_len(2) + name[name_len]
                if (response.size() < sizeof(ObjectId) + sizeof(DasGuid))
                {
                    DAS_CORE_LOG_ERROR(
                        "Response too small to parse ObjectInfoResponsePayload, "
                        "size = {}",
                        response.size());
                    return DAS_E_IPC_INVALID_MESSAGE_BODY;
                }

                ObjectId object_id;
                std::memcpy(&object_id, response.data(), sizeof(ObjectId));

                // Compute interface hash from the IID
                uint32_t interface_hash =
                    RemoteObjectRegistry::ComputeInterfaceId(iid);

                // Create proxy using unified GetOrCreateProxy (cache +
                // RegisterRemoteObject)
                auto [create_result, proxy] = proxy_factory_->GetOrCreateProxy(
                    run_loop_,
                    business_thread_,
                    object_id,
                    interface_hash);

                if (!proxy)
                {
                    DAS_CORE_LOG_ERROR(
                        "Failed to create proxy, result = {}, interface_id = 0x{:08X}",
                        create_result,
                        interface_hash);
                    return create_result;
                }

                *out_object.Put() = proxy.Get();
                out_object->AddRef();

                DAS_CORE_LOG_TRACE(
                    "Successfully resolved main process interface");
                out_object.Keep();
                return DAS_S_OK;
            }

            DasResult IpcContext::ResolveMainProcessInterfaceByName(
                const char* name,
                IDasBase**  pp_out_object)
            {
                if (!name || !pp_out_object)
                {
                    return DAS_E_INVALID_ARGUMENT;
                }
                DAS::DasOutPtr<IDasBase> out_object(pp_out_object);

                if (!business_thread_)
                {
                    DAS_CORE_LOG_ERROR(
                        "IpcRunLoop or BusinessThread not initialized");
                    return DAS_E_IPC_NOT_INITIALIZED;
                }

                DAS_CORE_LOG_TRACE(
                    "Resolving main process interface by name: {}",
                    name);

                // Build LOOKUP_BY_NAME request body: [uint16_t
                // name_len][char[] name]
                std::vector<uint8_t> payload;
                DAS::Core::IPC::SerializeString(payload, std::string(name));

                std::vector<uint8_t> response;

                DasResult result = SendBusinessControlRequestRaw(
                    run_loop_,
                    business_thread_,
                    session_id_,
                    1, // target_session_id = main process
                    IpcCommandType::LOOKUP_BY_NAME,
                    payload.data(),
                    payload.size(),
                    response);

                if (DAS::IsFailed(result))
                {
                    DAS_CORE_LOG_ERROR(
                        "Failed to resolve main process interface by name, "
                        "error = {}",
                        static_cast<int>(result));
                    return result;
                }

                // Parse response as ObjectInfoResponsePayload
                // Layout: ObjectId(8) + DasGuid(16) + session_id(2) +
                // version(2) + name_len(2) + name[name_len]
                if (response.size() < sizeof(ObjectId) + sizeof(DasGuid))
                {
                    DAS_CORE_LOG_ERROR(
                        "Response too small to parse ObjectInfoResponsePayload, "
                        "size = {}",
                        response.size());
                    return DAS_E_IPC_INVALID_MESSAGE_BODY;
                }

                ObjectId object_id;
                std::memcpy(&object_id, response.data(), sizeof(ObjectId));

                DasGuid resolved_iid;
                std::memcpy(
                    &resolved_iid,
                    response.data() + sizeof(ObjectId),
                    sizeof(DasGuid));

                // Compute interface hash from the resolved IID
                uint32_t interface_hash =
                    RemoteObjectRegistry::ComputeInterfaceId(resolved_iid);

                // Create proxy using unified GetOrCreateProxy
                auto [create_result, proxy] = proxy_factory_->GetOrCreateProxy(
                    run_loop_,
                    business_thread_,
                    object_id,
                    interface_hash);

                if (!proxy)
                {
                    DAS_CORE_LOG_ERROR(
                        "Failed to create proxy for name = {}, result = {}, interface_hash = 0x{:08X}",
                        name,
                        create_result,
                        interface_hash);
                    return create_result;
                }

                *out_object.Put() = proxy.Get();
                out_object->AddRef();

                DAS_CORE_LOG_TRACE(
                    "Successfully resolved main process interface by name: {}",
                    name);
                out_object.Keep();
                return DAS_S_OK;
            }

            DasResult IpcContext::RegisterService(
                IDasBase*      p_object,
                const DasGuid& iid)
            {
                (void)p_object;
                (void)iid;
                return DAS_E_NO_IMPLEMENTATION;
            }

            DasResult IpcContext::UnregisterService(const DasGuid& iid)
            {
                (void)iid;
                return DAS_E_NO_IMPLEMENTATION;
            }

            DasResult IpcContext::RegisterLocalObject(
                IDasBase* object_ptr,
                ObjectId& out_object_id)
            {
                return proxy_factory_->GetObjectManager().RegisterLocalObject(
                    object_ptr,
                    out_object_id);
            }

            // ====== C API 实现 =====

            DAS_API IIpcContext* CreateIpcContext(
                const IpcContextConfig& config)
            {
                try
                {
                    auto* context{new IpcContext(config)};
                    return context;
                }
                catch (const std::exception& e)
                {
                    DAS_CORE_LOG_ERROR(
                        "CreateIpcContext failed: {}",
                        ToString(e.what()));
                    return nullptr;
                }
                catch (...)
                {
                    DAS_CORE_LOG_ERROR(
                        "CreateIpcContext failed: unknown exception");
                    return nullptr;
                }
            }

            DAS_API void DestroyIpcContext(IIpcContext* ctx)
            {
                if (ctx != nullptr)
                {
                    delete ctx;
                }
            }

            // ====== IpcContextDeleter 实现 ======

            void IpcContextDeleter::operator()(IIpcContext* ctx) const
            {
                DestroyIpcContext(ctx);
            }

        } // namespace Host
    } // namespace IPC
} // namespace Core
DAS_NS_END
