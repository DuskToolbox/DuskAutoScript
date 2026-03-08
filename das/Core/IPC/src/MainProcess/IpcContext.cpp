#include <das/Core/IPC/AsyncOperationImpl.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/IPC/MainProcess/MainProcessServer.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/IPC/SessionCoordinator.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/IDasAsyncCallback.h>
#include <boost/asio/post.hpp>
#include <cstring>
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

                void PostCallback(IDasAsyncCallback* callback)
                {
                    if (!callback) return;

                    auto& server = MainProcessServer::GetInstance();
                    auto* run_loop = server.GetRunLoop();
                    if (!run_loop) return;

                    // DasPtr 在 post 之前获取所有权（AddRef），保证生命周期安全
                    DasPtr<IDasAsyncCallback> ptr(callback);
                    boost::asio::post(run_loop->GetIoContext(), [ptr = std::move(ptr)]() {
                        ptr->Do();
                    });
                }


                DasResult LoadPluginAsync(
                    IHostLauncher*                 host_launcher,
                    const char*                    u8_plugin_path,
                    IDasAsyncLoadPluginOperation** pp_out_operation,
                    std::chrono::milliseconds      timeout)
                {
                    if (!host_launcher || !u8_plugin_path || !pp_out_operation)
                    {
                        return DAS_E_INVALID_ARGUMENT;
                    }
                    *pp_out_operation = nullptr;

                    // 从 IHostLauncher 获取 session_id
                    uint16_t session_id = host_launcher->GetSessionId();
                    if (session_id == 0)
                    {
                        DAS_CORE_LOG_ERROR(
                            "LoadPluginAsync: Host not started (session_id = 0)");
                        return DAS_E_IPC_NOT_INITIALIZED;
                    }

                    // 1. 获取目标 session 的 IpcRunLoop
                    auto& conn_mgr = DAS::Core::IPC::ConnectionManager::GetInstance();
                    auto* run_loop = conn_mgr.GetRunLoop(session_id);
                    if (!run_loop)
                    {
                        DAS_CORE_LOG_ERROR(
                            "LoadPluginAsync: No RunLoop found for session_id = {}",
                            session_id);
                        return DAS_E_IPC_OBJECT_NOT_FOUND;
                    }

                    // 2. 构建 LOAD_PLUGIN 请求
                    std::string plugin_path(u8_plugin_path);
                    uint16_t    path_len = static_cast<uint16_t>(plugin_path.size());

                    // 载荷格式: uint16_t path_len + char[] path (不含 null terminator)
                    std::vector<uint8_t> payload(sizeof(uint16_t) + path_len);
                    std::memcpy(payload.data(), &path_len, sizeof(uint16_t));
                    std::memcpy(
                        payload.data() + sizeof(uint16_t),
                        plugin_path.data(),
                        path_len);

                    // 3. 构建消息头
                    auto header = MakeControlPlaneRequest(
                        IpcCommandType::LOAD_PLUGIN,
                        static_cast<uint32_t>(payload.size()),
                        session_id);

                    // 4. 发送异步请求
                    auto sender = run_loop->SendMessageAsync(
                        header,
                        payload.data(),
                        payload.size(),
                        timeout);

                    // 5. 包装为 IDasAsyncLoadPluginOperation
                    auto op = MakeAsyncOperation<
                        IDasAsyncLoadPluginOperation,
                        ObjectId>(std::move(sender));

                    *pp_out_operation = op.Get();
                    if (*pp_out_operation)
                    {
                        (*pp_out_operation)->AddRef();
                    }
                    return DAS_S_OK;
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

            void IpcContext::PostCallback(IDasAsyncCallback* callback)
            {
                impl_->PostCallback(callback);
            }

            DasResult IpcContext::LoadPluginAsync(
                IHostLauncher*                 host_launcher,
                const char*                    u8_plugin_path,
                IDasAsyncLoadPluginOperation** pp_out_operation,
                std::chrono::milliseconds      timeout)
            {
                return impl_->LoadPluginAsync(
                    host_launcher,
                    u8_plugin_path,
                    pp_out_operation,
                    timeout);
            }

            DasResult IpcContext::Run()
            {
                auto& server = MainProcessServer::GetInstance();
                auto* run_loop = server.GetRunLoop();
                if (!run_loop)
                {
                    DAS_CORE_LOG_ERROR("Run: IpcRunLoop not initialized");
                    return DAS_E_IPC_NOT_INITIALIZED;
                }
                return run_loop->Run();
            }

            void IpcContext::RequestStop()
            {
                auto& server = MainProcessServer::GetInstance();
                auto* run_loop = server.GetRunLoop();
                if (run_loop)
                {
                    run_loop->RequestStop();
                }
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
