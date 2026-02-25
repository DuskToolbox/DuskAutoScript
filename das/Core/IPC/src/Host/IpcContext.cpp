#include <das/Core/IPC/Host/HandshakeHandler.h>
#include <das/Core/IPC/Host/HostConfig.h>
#include <das/Core/IPC/Host/IIpcContext.h>
#include <das/Core/IPC/Host/IpcContext.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/MessageHandlerRef.h>
#include <das/Core/IPC/MessageQueueTransport.h>
#include <das/Core/IPC/ObjectManager.h>
#include <das/Core/IPC/SessionCoordinator.h>
#include <das/Core/IPC/SharedMemoryPool.h>

#include <atomic>
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
                    DasResult result = DAS_S_OK;

                    // 1. 分配 session_id
                    auto& coordinator = SessionCoordinator::GetInstance();
                    session_id_ = coordinator.AllocateSessionId();
                    if (session_id_ == 0)
                    {
                        return DAS_E_FAIL;
                    }
                    coordinator.SetLocalSessionId(session_id_);

                    // 2. 创建并初始化 DistributedObjectManager
                    object_manager_ =
                        std::make_unique<DistributedObjectManager>();
                    result = object_manager_->Initialize(session_id_);
                    if (result != DAS_S_OK)
                    {
                        coordinator.ReleaseSessionId(session_id_);
                        object_manager_.reset();
                        return result;
                    }

                    // 3. 先创建并初始化 Transport（确保 Transport 在 RunLoop
                    // 之前就绪） 使用当前进程 PID
                    // 作为消息队列和共享内存的命名基础
                    uint32_t pid;
#ifdef _WIN32
                    pid = GetCurrentProcessId();
#else
                    pid = getpid();
#endif
                    std::string host_to_plugin_queue =
                        MakeMessageQueueName(pid, true);
                    std::string plugin_to_host_queue =
                        MakeMessageQueueName(pid, false);
                    std::string shm_name = MakeSharedMemoryName(pid);

                    auto transport = std::make_unique<IpcTransport>();
                    result = transport->Initialize(
                        host_to_plugin_queue,
                        plugin_to_host_queue,
                        DEFAULT_MAX_MESSAGE_SIZE,
                        DEFAULT_MAX_MESSAGES);
                    if (result != DAS_S_OK)
                    {
                        object_manager_->Shutdown();
                        object_manager_.reset();
                        coordinator.ReleaseSessionId(session_id_);
                        return result;
                    }

                    // 4. 初始化 SharedMemoryPool
                    shared_memory_ = std::make_unique<SharedMemoryPool>();
                    result = shared_memory_->Initialize(
                        shm_name,
                        DEFAULT_SHARED_MEMORY_SIZE);
                    if (result != DAS_S_OK)
                    {
                        object_manager_->Shutdown();
                        object_manager_.reset();
                        coordinator.ReleaseSessionId(session_id_);
                        return result;
                    }

                    // 5. 将 SharedMemoryPool 设置到 Transport
                    result =
                        transport->SetSharedMemoryPool(shared_memory_.get());
                    if (result != DAS_S_OK)
                    {
                        shared_memory_->Shutdown();
                        shared_memory_.reset();
                        object_manager_->Shutdown();
                        object_manager_.reset();
                        coordinator.ReleaseSessionId(session_id_);
                        return result;
                    }

                    // 6. 创建 IpcRunLoop（此时 Transport 已就绪）
                    run_loop_ = std::make_unique<IpcRunLoop>();
                    run_loop_->SetTransport(std::move(transport));
                    // 注意：不再调用 run_loop_->Initialize()，因为 Transport
                    // 已通过 SetTransport 设置

                    // 7. 创建并初始化 IpcCommandHandler
                    command_handler_ = std::make_unique<IpcCommandHandler>();
                    command_handler_->SetSessionId(session_id_);

                    // 8. 创建并初始化 HandshakeHandler
                    handshake_handler_ = std::make_unique<HandshakeHandler>();
                    result = handshake_handler_->Initialize(session_id_);
                    if (result != DAS_S_OK)
                    {
                        run_loop_->Shutdown();
                        run_loop_.reset();
                        object_manager_->Shutdown();
                        object_manager_.reset();
                        coordinator.ReleaseSessionId(session_id_);
                        return result;
                    }

                    // 9. 设置 HandshakeHandler 的客户端连接回调
                    handshake_handler_->SetOnClientConnected(
                        [this](const ConnectedClient& /*client*/)
                        {
                            // 客户端连接完成
                            is_connected_ = true;

                            // 触发握手完成回调
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
                        {
                            // 客户端断开连接
                            is_connected_ = false;
                        });

                    // 10. 注册消息处理器
                    run_loop_->RegisterHandler(
                        std::make_unique<MessageHandlerRef>(
                            handshake_handler_.get()));

                    run_loop_->RegisterHandler(
                        std::make_unique<MessageHandlerRef>(
                            command_handler_.get()));

                    is_initialized_ = true;
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

                DistributedObjectManager& GetObjectManager()
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
                        run_loop_->Stop();
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
            DistributedObjectManager& IpcContext::GetObjectManager()
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
