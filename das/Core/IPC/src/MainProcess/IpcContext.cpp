#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/IPC/MainProcess/MainProcessServer.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/IPC/SessionCoordinator.h>
#include <das/Core/Logger/Logger.h>
DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        namespace MainProcess
        {
            /**
             * @brief IpcContext 的实现类
             */
            class IpcContextImpl
            {
            public:
                IpcContextImpl() = default;
                ~IpcContextImpl() = default;

                DasResult Initialize()
                {
                    DasResult result = DAS_S_OK;

                    // 1. 初始化 SessionCoordinator（主进程 session_id = 1）
                    auto& coordinator = SessionCoordinator::GetInstance();
                    coordinator.SetLocalSessionId(1);

                    // 2. 创建 DistributedObjectManager
                    object_manager_ =
                        std::make_unique<DistributedObjectManager>();

                    // 3. 初始化 ProxyFactory
                    auto& proxy_factory = ProxyFactory::GetInstance();
                    result = proxy_factory.Initialize(
                        object_manager_.get(),
                        &RemoteObjectRegistry::GetInstance(),
                        nullptr); // run_loop 留空，由 MainProcessServer 设置
                    if (result != DAS_S_OK)
                    {
                        DAS_CORE_LOG_ERROR(
                            "ProxyFactory initialization failed, result = 0x{:08X}",
                            result);
                        object_manager_.reset();
                        return result;
                    }

                    // 4. 初始化 MainProcessServer
                    auto& server = MainProcessServer::GetInstance();
                    result = server.Initialize();
                    if (result != DAS_S_OK)
                    {
                        DAS_CORE_LOG_ERROR(
                            "MainProcessServer initialization failed, result = 0x{:08X}",
                            result);
                        object_manager_.reset();
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

                    // 停止 MainProcessServer
                    auto&     server = MainProcessServer::GetInstance();
                    DasResult shutdown_result = server.Shutdown();
                    if (shutdown_result != DAS_S_OK)
                    {
                        result = shutdown_result;
                    }

                    // 清理 ProxyFactory
                    auto& proxy_factory = ProxyFactory::GetInstance();
                    proxy_factory.ClearAllProxies();

                    // 销毁 DistributedObjectManager
                    object_manager_.reset();

                    is_initialized_ = false;
                    return result;
                }

                MainProcessServer& GetServer()
                {
                    return MainProcessServer::GetInstance();
                }

                DistributedObjectManager& GetObjectManager()
                {
                    return *object_manager_;
                }

                ProxyFactory& GetProxyFactory()
                {
                    return ProxyFactory::GetInstance();
                }

                RemoteObjectRegistry& GetRegistry()
                {
                    return RemoteObjectRegistry::GetInstance();
                }

                void PostRequest(
                    void (*callback)(void* user_data),
                    void* user_data)
                {
                    auto& server = MainProcessServer::GetInstance();
                    auto* run_loop = server.GetRunLoop();
                    if (run_loop && callback)
                    {
                        run_loop->PostRequest([callback, user_data]()
                                              { callback(user_data); });
                    }
                }

                void PumpMessage()
                {
                    auto& server = MainProcessServer::GetInstance();
                    auto* run_loop = server.GetRunLoop();
                    if (run_loop)
                    {
                        uint32_t timeout_ms = run_loop->GetNearestDeadlineMs();
                        if (timeout_ms == 0)
                            timeout_ms = 100; // 无 pending call 时给合理默认值

                        run_loop->ReceiveAndDispatch(
                            std::chrono::milliseconds(timeout_ms));
                        run_loop->ProcessPostedCallbacks();
                        run_loop->TickPendingSenders();
                    }
                }

            private:
                std::unique_ptr<DistributedObjectManager> object_manager_;
                bool is_initialized_ = false;
            };

            // ====== IpcContext 实现（RAII 风格）======

            IpcContext::IpcContext() : impl_(std::make_unique<IpcContextImpl>())
            {
                // 构造即初始化
                impl_->Initialize();
            }

            IpcContext::~IpcContext()
            {
                // 析构即清理
                if (impl_)
                {
                    impl_->Shutdown();
                }
            }

            MainProcessServer& IpcContext::GetServer()
            {
                return impl_->GetServer();
            }

            DistributedObjectManager& IpcContext::GetObjectManager()
            {
                return impl_->GetObjectManager();
            }

            ProxyFactory& IpcContext::GetProxyFactory()
            {
                return impl_->GetProxyFactory();
            }

            RemoteObjectRegistry& IpcContext::GetRegistry()
            {
                return impl_->GetRegistry();
            }

            DasResult IpcContext::CreateHostLauncher(
                IHostLauncher** pp_out_launcher)
            {
                if (!pp_out_launcher)
                {
                    return DAS_E_INVALID_ARGUMENT;
                }

                auto& server = MainProcessServer::GetInstance();
                auto* run_loop = server.GetRunLoop();
                if (!run_loop)
                {
                    DAS_CORE_LOG_ERROR("CreateHostLauncher: IpcRunLoop not initialized");
                    return DAS_E_IPC_NOT_INITIALIZED;
                }

                *pp_out_launcher = new HostLauncher(run_loop->GetIoContext());
                return DAS_S_OK;
            }

            // ====== C API 实现 ======

            DAS_API IIpcContext* CreateIpcContext()
            {
                try
                {
                    // 构造即初始化（RAII）
                    auto* context{new IpcContext()};
                    return context;
                }
                catch (...)
                {
                    return nullptr;
                }
            }

            DAS_API void DestroyIpcContext(IIpcContext* ctx)
            {
                // 析构即清理（RAII）
                delete ctx;
            }

            // ====== IpcContextDeleter 实现 ======

            void IpcContextDeleter::operator()(IIpcContext* ctx) const
            {
                DestroyIpcContext(ctx);
            }

        } // namespace MainProcess
    } // namespace IPC
} // namespace Core
DAS_NS_END
