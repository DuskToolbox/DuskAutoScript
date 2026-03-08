#include <boost/asio/post.hpp>
#include <boost/process/v2/pid.hpp>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/Host/HandshakeHandler.h>
#include <das/Core/IPC/Host/HostConfig.h>
#include <das/Core/IPC/Host/IIpcContext.h>
#include <das/Core/IPC/Host/IpcContext.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/IpcTransport.h>
#include <das/Core/IPC/MessageHandlerRef.h>
#include <das/Core/IPC/SessionCoordinator.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/IDasAsyncCallback.h>
#include <das/Utils/fmt.h>

#include <atomic>
#include <chrono>
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
} // namespace

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        namespace Host
        {
            /**
             * @brief IpcContext 的实现类
             */
            class IpcContextImpl
            {
            public:
                explicit IpcContextImpl(
                    IpcContext*             owner,
                    const IpcContextConfig& config)
                    : owner_(owner), config_(config), is_connected_(false),
                      is_running_(false), handshake_complete_handler_(nullptr),
                      handshake_user_data_(nullptr)
                {
                }

                ~IpcContextImpl() = default;

                DasResult Initialize()
                {
                    // 获取当前进程 PID
                    host_pid_ = static_cast<uint32_t>(
                        boost::process::v2::current_pid());

                    // 如果 config_.main_pid == 0，返回错误
                    if (config_.main_pid == 0)
                    {
                        DAS_LOG_ERROR(
                            "IpcContext: main_pid 不能为 0，必须连接主进程");
                        return DAS_E_INVALID_ARGUMENT;
                    }

                    main_pid_ = config_.main_pid;
                    std::string msg = DAS_FMT_NS::format(
                        "IpcContext: Host 模式初始化，main_pid={}, host_pid={}",
                        main_pid_,
                        host_pid_);
                    DAS_LOG_INFO(msg.c_str());
                    return InitializeAsHost();
                }

                /**
                 * @brief Host 模式初始化（简化版）
                 *
                 * session_id 由 HandshakeHandler 在握手时分配。
                 * Host 进程启动后等待主进程连接，握手完成后获取 session_id。
                 */
                DasResult InitializeAsHost()
                {
                    DasResult result = DAS_S_OK;

                    // session_id 初始为 0，握手时由 HandshakeHandler 分配
                    session_id_ = 0;

                    std::string msg = DAS_FMT_NS::format(
                        "IpcContext: Host 模式初始化，main_pid={}, host_pid={}, session_id=待分配",
                        main_pid_,
                        host_pid_);
                    DAS_LOG_INFO(msg.c_str());

                    // 创建 Host 资源
                    result = CreateHostResources();
                    if (result != DAS_S_OK)
                    {
                        std::string err_msg = DAS_FMT_NS::format(
                            "IpcContext: 创建 Host 资源失败，result=0x{:08X}",
                            result);
                        DAS_LOG_ERROR(err_msg.c_str());
                        return result;
                    }

                    is_initialized_ = true;
                    msg = DAS_FMT_NS::format(
                        "IpcContext: Host 模式初始化完成，等待主进程连接分配 session_id");
                    DAS_LOG_INFO(msg.c_str());
                    return DAS_S_OK;
                }

                /**
                 * @brief 创建 Host 资源
                 * - 创建 Host 专用队列 das_ipc_{main_pid}_{host_pid}_m2h/h2m
                 * - 初始化 DistributedObjectManager
                 * - 初始化 SharedMemoryPool
                 * - 创建 RunLoop, CommandHandler, HandshakeHandler
                 * - 等待主进程连接并完成握手
                 */
                DasResult CreateHostResources()
                {
                    DasResult result = DAS_S_OK;

                    // 1. 创建 DistributedObjectManager（不再需要 Initialize）
                    object_manager_ =
                        std::make_unique<DistributedObjectManager>();

                    // 2. 创建 Host 专用队列名称（新格式）
                    std::string host_m2h =
                        MakeMessageQueueName(main_pid_, host_pid_, true);
                    std::string host_h2m =
                        MakeMessageQueueName(main_pid_, host_pid_, false);
                    std::string shm_name =
                        MakeSharedMemoryName(main_pid_, host_pid_);

                    std::string msg = DAS_FMT_NS::format(
                        "IpcContext: Creating Host resources, M2H={}, H2M={}, SHM={}",
                        host_m2h,
                        host_h2m,
                        shm_name);
                    DAS_LOG_INFO(msg.c_str());

                    // 3. 初始化 SharedMemoryPool
                    shared_memory_ = std::make_unique<SharedMemoryPool>();
                    result = shared_memory_->Initialize(
                        shm_name,
                        DEFAULT_SHARED_MEMORY_SIZE);
                    if (result != DAS_S_OK)
                    {
                        std::string err_msg = DAS_FMT_NS::format(
                            "IpcContext: SharedMemoryPool init failed, result=0x{:08X}",
                            result);
                        DAS_LOG_ERROR(err_msg.c_str());
                        object_manager_.reset();
                        return result;
                    }

                    // 4. 创建 IpcRunLoop 并使用新的 Initialize 重载
                    // Host 从 m2h (Main->Host) 读取，向 h2m (Host->Main) 写入
                    run_loop_ = std::make_unique<IpcRunLoop>();
                    result = run_loop_->Initialize(
                        host_m2h, // read queue (M2H - Main writes, Host reads)
                        host_h2m, // write queue (H2M - Host writes, Main reads)
                        true);    // is_server (Host creates queues)

                    if (result != DAS_S_OK)
                    {
                        std::string err_msg = DAS_FMT_NS::format(
                            "IpcContext: IpcRunLoop init failed, result=0x{:08X}",
                            result);
                        DAS_LOG_ERROR(err_msg.c_str());
                        shared_memory_.reset();
                        object_manager_.reset();
                        return result;
                    }

                    // 5. 创建并初始化 IpcCommandHandler
                    command_handler_ = std::make_unique<IpcCommandHandler>();
                    command_handler_->SetSessionId(session_id_);

                    // 6. 创建并初始化 HandshakeHandler
                    handshake_handler_ = std::make_unique<HandshakeHandler>();
                    result = handshake_handler_->Initialize(session_id_);
                    if (result != DAS_S_OK)
                    {
                        std::string err_msg = DAS_FMT_NS::format(
                            "IpcContext: HandshakeHandler init failed, result=0x{:08X}",
                            result);
                        DAS_LOG_ERROR(err_msg.c_str());
                        run_loop_.reset();
                        object_manager_.reset();
                        return result;
                    }

                    // 7. 设置 HandshakeHandler 的客户端连接回调
                    handshake_handler_->SetOnClientConnected(
                        [this](const ConnectedClient& /*client*/)
                        {
                            is_connected_ = true;

                            if (handshake_complete_handler_ != nullptr)
                            {
                                handshake_complete_handler_(
                                    static_cast<IIpcContext*>(owner_),
                                    DAS_S_OK,
                                    handshake_user_data_);
                            }
                        });

                    handshake_handler_->SetOnClientDisconnected(
                        [this](uint16_t /*session_id*/)
                        { is_connected_ = false; });

                    // 收到 GOODBYE 时请求进程退出
                    // 直接设置 running_ 标志为 false，io_thread_
                    // 会在下次循环检查时退出 不能在 io_thread_ 上调用
                    // Stop()（会 join 自己导致死锁）
                    handshake_handler_->SetOnShutdownRequested(
                        [this]()
                        {
                            std::string msg = DAS_FMT_NS::format(
                                "IpcContext: 收到 GOODBYE，请求退出");
                            DAS_LOG_INFO(msg.c_str());

                            // 仅设置标志，不 join 线程
                            if (run_loop_)
                            {
                                run_loop_->RequestStop();
                            }
                        });

                    // 9. 注册消息处理器
                    run_loop_->RegisterHandler(
                        std::make_unique<MessageHandlerRef>(
                            handshake_handler_.get()));

                    run_loop_->RegisterHandler(
                        std::make_unique<MessageHandlerRef>(
                            command_handler_.get()));

                    DAS_LOG_INFO(msg.c_str());

                    return DAS_S_OK;
                }

                DasResult Shutdown()
                {
                    if (!is_initialized_)
                    {
                        return DAS_S_OK;
                    }

                    DasResult result = DAS_S_OK;

                    // 停止父进程监控线程
                    StopParentProcessMonitor();

                    // 停止事件循环
                    if (run_loop_)
                    {
                        DasResult stop_result = run_loop_->Stop();
                        if (stop_result != DAS_S_OK)
                        {
                            result = stop_result;
                        }
                    }

                    // 关闭 HandshakeHandler
                    if (handshake_handler_)
                    {
                        DasResult handshake_result =
                            handshake_handler_->Shutdown();
                        if (handshake_result != DAS_S_OK)
                        {
                            result = handshake_result;
                        }
                        handshake_handler_.reset();
                    }

                    // 关闭 SharedMemoryPool
                    if (shared_memory_)
                    {
                        shared_memory_->Shutdown();
                        shared_memory_.reset();
                    }

                    // 关闭 IpcRunLoop
                    if (run_loop_)
                    {
                        DasResult loop_result = run_loop_->Shutdown();
                        if (loop_result != DAS_S_OK)
                        {
                            result = loop_result;
                        }
                        run_loop_.reset();
                    }

                    // 关闭 DistributedObjectManager（析构函数自动清理）
                    object_manager_.reset();

                    // 释放 session_id
                    if (session_id_ != 0)
                    {
                        auto& coordinator = SessionCoordinator::GetInstance();
                        coordinator.ReleaseSessionId(session_id_);
                        session_id_ = 0;
                    }

                    is_initialized_ = false;
                    is_connected_ = false;
                    return result;
                }

                IpcRunLoop& GetRunLoop() { return *run_loop_; }

                IpcCommandHandler& GetCommandHandler()
                {
                    return *command_handler_;
                }

                IDistributedObjectManager& GetObjectManager()
                {
                    return *object_manager_;
                }

                void RegisterCommandHandler(
                    uint32_t       cmd_type,
                    CommandHandler handler)
                {
                    command_handler_->RegisterHandler(
                        static_cast<IpcCommandType>(cmd_type),
                        handler);
                }

                void SetOnHandshakeComplete(
                    OnHandshakeComplete handler,
                    void*               user_data)
                {
                    handshake_complete_handler_ = handler;
                    handshake_user_data_ = user_data;
                }

                DasResult Run()
                {
                    if (!is_initialized_)
                    {
                        return DAS_E_FAIL;
                    }

                    if (is_running_)
                    {
                        return DAS_S_OK;
                    }

                    is_running_ = true;

                    // 启动父进程存活检测线程
                    StartParentProcessMonitor();

                    DasResult result = run_loop_->Run();
                    is_running_ = false;

                    // 停止父进程监控线程
                    StopParentProcessMonitor();

                    return result;
                }

                void RequestStop()
                {
                    if (run_loop_)
                    {
                        run_loop_->RequestStop();
                    }
                }

                DasResult LoadPlugin(
                    const std::filesystem::path& /*json_path*/,
                    ObjectId* /*object_id*/)
                {
                    // TODO: 实现插件加载逻辑
                    // 1. 读取 JSON 配置文件
                    // 2. 加载插件 DLL
                    // 3. 创建插件对象
                    // 4. 注册对象到 DistributedObjectManager
                    // 5. 返回对象 ID
                    return DAS_E_IPC_PLUGIN_LOAD_FAILED;
                }

                bool IsConnected() const { return is_connected_; }

                void PostCallback(IDasAsyncCallback* callback)
                {
                    if (!callback || !run_loop_)
                        return;

                    // DasPtr 在 post 之前获取所有权（AddRef），保证生命周期安全
                    DasPtr<IDasAsyncCallback> ptr(callback);
                    boost::asio::post(
                        run_loop_->GetIoContext(),
                        [ptr = std::move(ptr)]() { ptr->Do(); });
                }

                DasResult RegisterLocalObject(
                    void*     object_ptr,
                    ObjectId& out_object_id)
                {
                    if (!object_manager_)
                        return DAS_E_FAIL;
                    return object_manager_->RegisterLocalObject(
                        object_ptr,
                        out_object_id);
                }

            private:
                IpcContext*                               owner_ = nullptr;
                IpcContextConfig                          config_;
                uint16_t                                  session_id_ = 0;
                std::unique_ptr<DistributedObjectManager> object_manager_;
                std::unique_ptr<IpcRunLoop>               run_loop_;
                std::unique_ptr<IpcCommandHandler>        command_handler_;
                std::unique_ptr<HandshakeHandler>         handshake_handler_;
                std::unique_ptr<SharedMemoryPool>         shared_memory_;

                bool is_initialized_ = false;
                bool is_connected_ = false;
                bool is_running_ = false;

                OnHandshakeComplete handshake_complete_handler_;
                void*               handshake_user_data_ = nullptr;

                // Host 模式专用成员变量
                uint32_t host_pid_ = 0;
                uint32_t main_pid_ = 0;

                // 父进程存活检测线程
                std::thread       parent_monitor_thread_;
                std::atomic<bool> parent_monitor_running_{false};

                /**
                 * @brief 启动父进程存活检测线程
                 *
                 * 仅当 main_pid_ != 0 时启动。
                 * 定期检查主进程是否存活，若主进程退出则请求 Host 退出。
                 */
                void StartParentProcessMonitor()
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
                                    if (run_loop_)
                                    {
                                        run_loop_->RequestStop();
                                    }
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

                /**
                 * @brief 停止父进程存活检测线程
                 */
                void StopParentProcessMonitor()
                {
                    parent_monitor_running_.store(false);
                    if (parent_monitor_thread_.joinable())
                    {
                        parent_monitor_thread_.join();
                    }
                }
            };

            // ====== IpcContext 实现 ======

            IpcContext::IpcContext(const IpcContextConfig& config)
                : impl_(std::make_unique<IpcContextImpl>(this, config))
            {
                impl_->Initialize();
            }

            IpcContext::~IpcContext() { impl_->Shutdown(); }

            IpcRunLoop& IpcContext::GetRunLoop() { return impl_->GetRunLoop(); }

            IpcCommandHandler& IpcContext::GetCommandHandler()
            {
                return impl_->GetCommandHandler();
            }
            IDistributedObjectManager& IpcContext::GetObjectManager()
            {
                return impl_->GetObjectManager();
            }

            void IpcContext::RegisterCommandHandler(
                uint32_t       cmd_type,
                CommandHandler handler)
            {
                impl_->RegisterCommandHandler(cmd_type, handler);
            }

            void IpcContext::SetOnHandshakeComplete(
                OnHandshakeComplete handler,
                void*               user_data)
            {
                impl_->SetOnHandshakeComplete(handler, user_data);
            }

            DasResult IpcContext::Run() { return impl_->Run(); }

            void IpcContext::RequestStop() { impl_->RequestStop(); }

            DasResult IpcContext::LoadPlugin(
                const std::filesystem::path& json_path,
                ObjectId*                    object_id)
            {
                return impl_->LoadPlugin(json_path, object_id);
            }

            bool IpcContext::IsConnected() const
            {
                return impl_->IsConnected();
            }

            void IpcContext::PostCallback(IDasAsyncCallback* callback)
            {
                impl_->PostCallback(callback);
            }

            DasResult IpcContext::RegisterLocalObject(
                void*     object_ptr,
                ObjectId& out_object_id)
            {
                return impl_->RegisterLocalObject(object_ptr, out_object_id);
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
                catch (...)
                {
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
