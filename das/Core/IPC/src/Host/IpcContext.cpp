#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/Handshake.h>
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
#include <das/DasApi.h>
#include <das/Utils/fmt.h>

#include <das/Core/IPC/IpcMessageHeader.h>

#include <atomic>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

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
#ifdef _WIN32
                    host_pid_ = GetCurrentProcessId();
#else
                    host_pid_ = getpid();
#endif

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
                 * @brief Host 模式初始化（两阶段）
                 * Phase 1: 连接主进程获取 session_id
                 * Phase 2: 创建 Host 资源
                 */
                DasResult InitializeAsHost()
                {
                    DasResult result = DAS_S_OK;

                    // Phase 1: 连接主进程获取 session_id
                    result = ConnectToMainProcess();
                    if (result != DAS_S_OK)
                    {
                        std::string msg = DAS_FMT_NS::format(
                            "IpcContext: 连接主进程失败，result=0x{:08X}",
                            result);
                        DAS_LOG_ERROR(msg.c_str());
                        return result;
                    }

                    // Phase 2: 创建 Host 资源
                    result = CreateHostResources();
                    if (result != DAS_S_OK)
                    {
                        std::string msg = DAS_FMT_NS::format(
                            "IpcContext: 创建 Host 资源失败，result=0x{:08X}",
                            result);
                        DAS_LOG_ERROR(msg.c_str());
                        main_transport_->Shutdown();
                        main_transport_.reset();
                        return result;
                    }

                    is_initialized_ = true;
                    std::string msg = DAS_FMT_NS::format(
                        "IpcContext: Host 模式初始化完成，session_id={}",
                        session_id_);
                    DAS_LOG_INFO(msg.c_str());
                    return DAS_S_OK;
                }

                /**
                 * @brief Phase 1: 连接主进程获取 session_id
                 * - 连接主进程监听队列 das_ipc_{main_pid}_0_m2h/h2m
                 * - 发送 HELLO(host_pid)
                 * - 等待 WELCOME，获取 session_id
                 */
                DasResult ConnectToMainProcess()
                {
                    // 主进程监听队列名称
                    // M2H: Host 发送给主进程
                    // H2M: 主进程发送给 Host
                    std::string main_m2h =
                        IpcTransport::MakeQueueName(main_pid_, 0, true);
                    std::string main_h2m =
                        IpcTransport::MakeQueueName(main_pid_, 0, false);

                    std::string msg = DAS_FMT_NS::format(
                        "IpcContext: 连接主进程，M2H={}, H2M={}",
                        main_m2h,
                        main_h2m);
                    DAS_LOG_INFO(msg.c_str());

                    // 创建 Transport 连接到主进程
                    // Connect 参数: (receive_queue, send_queue)
                    // - receive_queue: M2H (Main->Host)
                    // - send_queue: H2M (Host->Main)
                    main_transport_ = std::make_unique<IpcTransport>();
                    DasResult result = main_transport_->Connect(
                        main_m2h, // 接收队列 (M2H)
                        main_h2m  // 发送队列 (H2M)
                    );
                    if (result != DAS_S_OK)
                    {
                        std::string err_msg = DAS_FMT_NS::format(
                            "IpcContext: 连接主进程消息队列失败，result=0x{:08X}",
                            result);
                        DAS_LOG_ERROR(err_msg.c_str());
                        main_transport_.reset();
                        return result;
                    }

                    // 发送 HELLO 请求
                    HelloRequestV1 hello_req;
                    InitHelloRequest(hello_req, host_pid_, "HostProcess");

                    IPCMessageHeader header{};
                    header.magic = IPCMessageHeader::MAGIC;
                    header.version = IPCMessageHeader::CURRENT_VERSION;
                    header.message_type =
                        static_cast<uint8_t>(MessageType::REQUEST);
                    header.header_flags = 0;
                    header.call_id = 1;
                    header.interface_id = static_cast<uint32_t>(
                        HandshakeInterfaceId::HANDSHAKE_IFACE_HELLO);
                    header.method_id = 0;
                    header.flags = 0;
                    header.error_code = 0;
                    header.session_id = 0;
                    header.generation = 0;
                    header.local_id = 0;
                    header.body_size = sizeof(HelloRequestV1);

                    result = main_transport_->Send(
                        header,
                        reinterpret_cast<const uint8_t*>(&hello_req),
                        sizeof(hello_req));
                    if (result != DAS_S_OK)
                    {
                        std::string err_msg = DAS_FMT_NS::format(
                            "IpcContext: 发送 HELLO 失败，result=0x{:08X}",
                            result);
                        DAS_LOG_ERROR(err_msg.c_str());
                        main_transport_->Shutdown();
                        main_transport_.reset();
                        return result;
                    }

                    msg = DAS_FMT_NS::format(
                        "IpcContext: 已发送 HELLO，等待 WELCOME...");
                    DAS_LOG_INFO(msg.c_str());

                    // 等待 WELCOME 响应
                    IPCMessageHeader     resp_header;
                    std::vector<uint8_t> resp_body;
                    result = main_transport_->Receive(
                        resp_header,
                        resp_body,
                        DEFAULT_CONNECTION_TIMEOUT_MS);
                    if (result != DAS_S_OK)
                    {
                        std::string err_msg = DAS_FMT_NS::format(
                            "IpcContext: 接收 WELCOME 超时或失败，result=0x{:08X}",
                            result);
                        DAS_LOG_ERROR(err_msg.c_str());
                        main_transport_->Shutdown();
                        main_transport_.reset();
                        return result;
                    }

                    // 验证响应
                    if (resp_header.interface_id
                        != static_cast<uint32_t>(
                            HandshakeInterfaceId::HANDSHAKE_IFACE_WELCOME))
                    {
                        std::string err_msg = DAS_FMT_NS::format(
                            "IpcContext: 收到非 WELCOME 响应，interface_id={}",
                            resp_header.interface_id);
                        DAS_LOG_ERROR(err_msg.c_str());
                        main_transport_->Shutdown();
                        main_transport_.reset();
                        return DAS_E_IPC_PROTOCOL_ERROR;
                    }

                    if (resp_body.size() < sizeof(WelcomeResponseV1))
                    {
                        std::string err_msg = DAS_FMT_NS::format(
                            "IpcContext: WELCOME 响应体太小，size={}",
                            resp_body.size());
                        DAS_LOG_ERROR(err_msg.c_str());
                        main_transport_->Shutdown();
                        main_transport_.reset();
                        return DAS_E_IPC_INVALID_MESSAGE;
                    }

                    WelcomeResponseV1* welcome =
                        reinterpret_cast<WelcomeResponseV1*>(resp_body.data());

                    if (welcome->status != WelcomeResponseV1::STATUS_SUCCESS
                        || welcome->session_id == 0)
                    {
                        std::string err_msg = DAS_FMT_NS::format(
                            "IpcContext: WELCOME 响应失败，status={}, session_id={}",
                            welcome->status,
                            welcome->session_id);
                        DAS_LOG_ERROR(err_msg.c_str());
                        main_transport_->Shutdown();
                        main_transport_.reset();
                        return DAS_E_IPC_HANDSHAKE_FAILED;
                    }

                    // 保存 session_id
                    session_id_ = welcome->session_id;

                    // 设置本地 session_id
                    auto& coordinator = SessionCoordinator::GetInstance();
                    coordinator.SetLocalSessionId(session_id_);

                    msg = DAS_FMT_NS::format(
                        "IpcContext: 收到 WELCOME，session_id={}",
                        session_id_);
                    DAS_LOG_INFO(msg.c_str());

                    return DAS_S_OK;
                }

                /**
                 * @brief Phase 2: 创建 Host 资源
                 * - 创建 Host 专用队列 das_ipc_{main_pid}_{host_pid}_m2h/h2m
                 * - 初始化 DistributedObjectManager
                 * - 初始化 SharedMemoryPool
                 * - 创建 RunLoop, CommandHandler, HandshakeHandler
                 * - 发送 READY
                 * - 等待 READYACK
                 */
                DasResult CreateHostResources()
                {
                    DasResult result = DAS_S_OK;

                    // 1. 创建并初始化 DistributedObjectManager
                    object_manager_ =
                        std::make_unique<DistributedObjectManager>();
                    result = object_manager_->Initialize(session_id_);
                    if (result != DAS_S_OK)
                    {
                        std::string msg = DAS_FMT_NS::format(
                            "IpcContext: DistributedObjectManager 初始化失败，result=0x{:08X}",
                            result);
                        DAS_LOG_ERROR(msg.c_str());
                        return result;
                    }

                    // 2. 创建 Host 专用队列（新格式）
                    std::string host_m2h =
                        MakeMessageQueueName(main_pid_, host_pid_, true);
                    std::string host_h2m =
                        MakeMessageQueueName(main_pid_, host_pid_, false);
                    std::string shm_name =
                        MakeSharedMemoryName(main_pid_, host_pid_);

                    std::string msg = DAS_FMT_NS::format(
                        "IpcContext: 创建 Host 资源，M2H={}, H2M={}, SHM={}",
                        host_m2h,
                        host_h2m,
                        shm_name);
                    DAS_LOG_INFO(msg.c_str());

                    auto transport = std::make_unique<IpcTransport>();
                    result = transport->Initialize(
                        host_h2m, // Host 接收队列 (H2M)
                        host_m2h, // Host 发送队列 (M2H)
                        DEFAULT_MAX_MESSAGE_SIZE,
                        DEFAULT_MAX_MESSAGES);
                    if (result != DAS_S_OK)
                    {
                        std::string err_msg = DAS_FMT_NS::format(
                            "IpcContext: Transport 初始化失败，result=0x{:08X}",
                            result);
                        DAS_LOG_ERROR(err_msg.c_str());
                        object_manager_->Shutdown();
                        object_manager_.reset();
                        return result;
                    }

                    // 3. 初始化 SharedMemoryPool
                    shared_memory_ = std::make_unique<SharedMemoryPool>();
                    result = shared_memory_->Initialize(
                        shm_name,
                        DEFAULT_SHARED_MEMORY_SIZE);
                    if (result != DAS_S_OK)
                    {
                        std::string err_msg = DAS_FMT_NS::format(
                            "IpcContext: SharedMemoryPool 初始化失败，result=0x{:08X}",
                            result);
                        DAS_LOG_ERROR(err_msg.c_str());
                        object_manager_->Shutdown();
                        object_manager_.reset();
                        return result;
                    }

                    // 4. 将 SharedMemoryPool 设置到 Transport
                    result =
                        transport->SetSharedMemoryPool(shared_memory_.get());
                    if (result != DAS_S_OK)
                    {
                        std::string err_msg = DAS_FMT_NS::format(
                            "IpcContext: 设置 SharedMemoryPool 到 Transport 失败，result=0x{:08X}",
                            result);
                        DAS_LOG_ERROR(err_msg.c_str());
                        shared_memory_->Shutdown();
                        shared_memory_.reset();
                        object_manager_->Shutdown();
                        object_manager_.reset();
                        return result;
                    }

                    // 5. 创建 IpcRunLoop
                    run_loop_ = std::make_unique<IpcRunLoop>();
                    run_loop_->SetTransport(std::move(transport));

                    // 6. 创建并初始化 IpcCommandHandler
                    command_handler_ = std::make_unique<IpcCommandHandler>();
                    command_handler_->SetSessionId(session_id_);

                    // 7. 创建并初始化 HandshakeHandler
                    handshake_handler_ = std::make_unique<HandshakeHandler>();
                    result = handshake_handler_->Initialize(session_id_);
                    if (result != DAS_S_OK)
                    {
                        std::string err_msg = DAS_FMT_NS::format(
                            "IpcContext: HandshakeHandler 初始化失败，result=0x{:08X}",
                            result);
                        DAS_LOG_ERROR(err_msg.c_str());
                        run_loop_->Shutdown();
                        run_loop_.reset();
                        object_manager_->Shutdown();
                        object_manager_.reset();
                        return result;
                    }

                    // 8. 设置 HandshakeHandler 的客户端连接回调
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
                    // 直接设置 running_ 标志为 false，io_thread_ 会在下次循环检查时退出
                    // 不能在 io_thread_ 上调用 Stop()（会 join 自己导致死锁）
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

                    // 10. 发送 READY 到主进程
                    ReadyRequestV1 ready_req;
                    InitReadyRequest(ready_req, session_id_);

                    IPCMessageHeader ready_header{};
                    ready_header.magic = IPCMessageHeader::MAGIC;
                    ready_header.version = IPCMessageHeader::CURRENT_VERSION;
                    ready_header.message_type =
                        static_cast<uint8_t>(MessageType::REQUEST);
                    ready_header.header_flags = 0;
                    ready_header.call_id = 2;
                    ready_header.interface_id = static_cast<uint32_t>(
                        HandshakeInterfaceId::HANDSHAKE_IFACE_READY);
                    ready_header.method_id = 0;
                    ready_header.flags = 0;
                    ready_header.error_code = 0;
                    // 控制平面消息: ObjectId = {0, 0, 0}
                    ready_header.session_id = 0;
                    ready_header.generation = 0;
                    ready_header.local_id = 0;
                    ready_header.body_size = sizeof(ReadyRequestV1);

                    result = main_transport_->Send(
                        ready_header,
                        reinterpret_cast<const uint8_t*>(&ready_req),
                        sizeof(ready_req));
                    if (result != DAS_S_OK)
                    {
                        std::string err_msg = DAS_FMT_NS::format(
                            "IpcContext: 发送 READY 失败，result=0x{:08X}",
                            result);
                        DAS_LOG_ERROR(err_msg.c_str());
                        run_loop_->Shutdown();
                        run_loop_.reset();
                        handshake_handler_.reset();
                        shared_memory_->Shutdown();
                        shared_memory_.reset();
                        object_manager_->Shutdown();
                        object_manager_.reset();
                        return result;
                    }

                    msg = DAS_FMT_NS::format(
                        "IpcContext: 已发送 READY，等待 READYACK...");
                    DAS_LOG_INFO(msg.c_str());

                    // 11. 等待 READYACK
                    IPCMessageHeader     ack_header;
                    std::vector<uint8_t> ack_body;
                    result = main_transport_->Receive(
                        ack_header,
                        ack_body,
                        DEFAULT_CONNECTION_TIMEOUT_MS);
                    if (result != DAS_S_OK)
                    {
                        std::string err_msg = DAS_FMT_NS::format(
                            "IpcContext: 接收 READYACK 超时或失败，result=0x{:08X}",
                            result);
                        DAS_LOG_ERROR(err_msg.c_str());
                        run_loop_->Shutdown();
                        run_loop_.reset();
                        handshake_handler_.reset();
                        shared_memory_->Shutdown();
                        shared_memory_.reset();
                        object_manager_->Shutdown();
                        object_manager_.reset();
                        return result;
                    }

                    // 验证 READYACK
                    if (ack_header.interface_id
                        != static_cast<uint32_t>(
                            HandshakeInterfaceId::HANDSHAKE_IFACE_READY_ACK))
                    {
                        std::string err_msg = DAS_FMT_NS::format(
                            "IpcContext: 收到非 READYACK 响应，interface_id={}",
                            ack_header.interface_id);
                        DAS_LOG_ERROR(err_msg.c_str());
                        run_loop_->Shutdown();
                        run_loop_.reset();
                        handshake_handler_.reset();
                        shared_memory_->Shutdown();
                        shared_memory_.reset();
                        object_manager_->Shutdown();
                        object_manager_.reset();
                        return DAS_E_IPC_PROTOCOL_ERROR;
                    }

                    if (ack_body.size() < sizeof(ReadyAckV1))
                    {
                        std::string err_msg = DAS_FMT_NS::format(
                            "IpcContext: READYACK 响应体太小，size={}",
                            ack_body.size());
                        DAS_LOG_ERROR(err_msg.c_str());
                        run_loop_->Shutdown();
                        run_loop_.reset();
                        handshake_handler_.reset();
                        shared_memory_->Shutdown();
                        shared_memory_.reset();
                        object_manager_->Shutdown();
                        object_manager_.reset();
                        return DAS_E_IPC_INVALID_MESSAGE;
                    }

                    ReadyAckV1* ready_ack =
                        reinterpret_cast<ReadyAckV1*>(ack_body.data());

                    if (ready_ack->status != ReadyAckV1::STATUS_SUCCESS)
                    {
                        std::string err_msg = DAS_FMT_NS::format(
                            "IpcContext: READYACK 响应失败，status={}",
                            ready_ack->status);
                        DAS_LOG_ERROR(err_msg.c_str());
                        run_loop_->Shutdown();
                        run_loop_.reset();
                        handshake_handler_.reset();
                        shared_memory_->Shutdown();
                        shared_memory_.reset();
                        object_manager_->Shutdown();
                        object_manager_.reset();
                        return DAS_E_IPC_HANDSHAKE_FAILED;
                    }

                    msg = DAS_FMT_NS::format(
                        "IpcContext: 收到 READYACK，Host 资源创建完成");
                    DAS_LOG_INFO(msg.c_str());

                    // 关闭与主进程的临时连接（后续通过 Host 专用队列通信）
                    // 注意：这里不关闭
                    // main_transport_，因为可能还需要与主进程通信
                    // 如果后续不需要，可以在这里关闭

                    return DAS_S_OK;
                }

                DasResult Shutdown()
                {
                    if (!is_initialized_)
                    {
                        return DAS_S_OK;
                    }

                    DasResult result = DAS_S_OK;

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

                    // 关闭 DistributedObjectManager
                    if (object_manager_)
                    {
                        DasResult object_result = object_manager_->Shutdown();
                        if (object_result != DAS_S_OK)
                        {
                            result = object_result;
                        }
                        object_manager_.reset();
                    }

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
                    DasResult result = run_loop_->Run();
                    is_running_ = false;

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
                std::unique_ptr<IpcTransport>
                    main_transport_; // 与主进程通信的 Transport（握手阶段）
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

            // ====== C API 实现 ======

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
