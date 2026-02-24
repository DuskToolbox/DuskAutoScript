#include <das/Core/IPC/Host/HandshakeHandler.h>
#include <das/Core/IPC/Host/IIpcContext.h>
#include <das/Core/IPC/Host/IpcContext.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ObjectManager.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/Core/IPC/SessionCoordinator.h>
#include <das/Core/IPC/Host/HostConfig.h>
#include <das/Core/IPC/MessageQueueTransport.h>

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
                explicit IpcContextImpl(IpcContext* owner, const IpcContextConfig& config)
                    : owner_(owner), config_(config), is_connected_(false), is_running_(false),
                      handshake_complete_handler_(nullptr),
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

                    // 3. 创建 IpcRunLoop
                    run_loop_ = std::make_unique<IpcRunLoop>();
                    result = run_loop_->Initialize();
                    if (result != DAS_S_OK)
                    {
                        object_manager_->Shutdown();
                        object_manager_.reset();
                        coordinator.ReleaseSessionId(session_id_);
                        return result;
                    }

                    // 4. 创建并初始化 IpcCommandHandler
                    command_handler_ = std::make_unique<IpcCommandHandler>();
                    command_handler_->SetSessionId(session_id_);

                    // 5. 创建并初始化 HandshakeHandler
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

                    // 6. 设置 HandshakeHandler 的客户端连接回调
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

                    // 7. 设置 IpcRunLoop 的请求处理器
                    run_loop_->SetRequestHandler(
                        [this](
                            const IPCMessageHeader& header,
                            const uint8_t*          body,
                            size_t                  body_size) -> DasResult
                        {
                            // 优先处理握手消息
                            std::vector<uint8_t> response;
                            DasResult            result =
                                handshake_handler_->HandleMessage(
                                    header,
                                    body,
                                    body_size,
                                    response);
                            if (result == DAS_S_OK && !response.empty())
                            {
                                run_loop_->SendResponse(
                                    header,
                                    response.data(),
                                    response.size());
                                return DAS_S_OK;
                            }

                            // 处理命令消息
                            IpcCommandResponse cmd_response;
                            result = command_handler_->HandleCommand(
                                header,
                                std::span<const uint8_t>(body, body_size),
                                cmd_response);
                            if (result == DAS_S_OK
                                && !cmd_response.response_data.empty())
                            {
                                run_loop_->SendResponse(
                                    header,
                                    cmd_response.response_data.data(),
                                    cmd_response.response_data.size());
                            }

                            return result;
                        });

                    // 8. 初始化 Transport（消息队列）
                    // 使用当前进程 PID 作为消息队列和共享内存的命名基础
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

                    IpcTransport* transport = run_loop_->GetTransport();
                    if (!transport)
                    {
                        handshake_handler_->Shutdown();
                        handshake_handler_.reset();
                        run_loop_->Shutdown();
                        run_loop_.reset();
                        object_manager_->Shutdown();
                        object_manager_.reset();
                        coordinator.ReleaseSessionId(session_id_);
                        return DAS_E_FAIL;
                    }

                    result = transport->Initialize(
                        host_to_plugin_queue,
                        plugin_to_host_queue,
                        DEFAULT_MAX_MESSAGE_SIZE,
                        DEFAULT_MAX_MESSAGES);
                    if (result != DAS_S_OK)
                    {
                        handshake_handler_->Shutdown();
                        handshake_handler_.reset();
                        run_loop_->Shutdown();
                        run_loop_.reset();
                        object_manager_->Shutdown();
                        object_manager_.reset();
                        coordinator.ReleaseSessionId(session_id_);
                        return result;
                    }

                    // 9. 初始化 SharedMemoryPool
                    shared_memory_ = std::make_unique<SharedMemoryPool>();
                    result = shared_memory_->Initialize(
                        shm_name,
                        DEFAULT_SHARED_MEMORY_SIZE);
                    if (result != DAS_S_OK)
                    {
                        handshake_handler_->Shutdown();
                        handshake_handler_.reset();
                        run_loop_->Shutdown();
                        run_loop_.reset();
                        object_manager_->Shutdown();
                        object_manager_.reset();
                        coordinator.ReleaseSessionId(session_id_);
                        return result;
                    }

                    // 10. 将 SharedMemoryPool 设置到 Transport
                    result = transport->SetSharedMemoryPool(shared_memory_.get());
                    if (result != DAS_S_OK)
                    {
                        shared_memory_->Shutdown();
                        shared_memory_.reset();
                        handshake_handler_->Shutdown();
                        handshake_handler_.reset();
                        run_loop_->Shutdown();
                        run_loop_.reset();
                        object_manager_->Shutdown();
                        object_manager_.reset();
                        coordinator.ReleaseSessionId(session_id_);
                        return result;
                    }

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
                    uint32_t      cmd_type,
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
                IpcContext* owner_ = nullptr;
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
                uint32_t      cmd_type,
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

            DAS_API IIpcContext* CreateIpcContext(const IpcContextConfig& config)
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
