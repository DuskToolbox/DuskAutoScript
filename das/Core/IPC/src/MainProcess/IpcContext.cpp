#include <boost/asio/post.hpp>
#include <cstring>
#include <das/Core/IPC/AsyncOperationImpl.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/IPC/SessionCoordinator.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/IDasAsyncCallback.h>
#include <das/Utils/StringUtils.h>

// Define alias for IpcRunLoop in the parent namespace
namespace Das::Core::IPC::MainProcess
{
    using IpcRunLoopType = Das::Core::IPC::IpcRunLoop;
}

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        namespace MainProcess
        {
            // ====== IpcContext 实现（RAII 风格，无 pimpl）======

            IpcContext::IpcContext()
            {
                // 构造即初始化
                DasResult result = DAS_S_OK;

                // 1. Create IpcRunLoop FIRST (provides io_context)
                auto runloop_result = IpcRunLoopType::Create();
                if (!runloop_result.has_value())
                {
                    DAS_CORE_LOG_ERROR("IpcRunLoop::Create() failed");
                    throw std::runtime_error("IpcRunLoop::Create() failed");
                }
                runloop_ = std::move(runloop_result.value());
                // Create() 已自动完成初始化，无需再调用 Initialize()

                // 2. Initialize SessionCoordinator（主进程 session_id = 1）
                auto& coordinator = SessionCoordinator::GetInstance();
                coordinator.SetLocalSessionId(1);

                // 3. Create DistributedObjectManager
                object_manager_ = std::make_unique<DistributedObjectManager>();

                // 4. Initialize ProxyFactory with runloop_
                auto& proxy_factory = ProxyFactory::GetInstance();
                result = proxy_factory.Initialize(
                    object_manager_.get(),
                    &RemoteObjectRegistry::GetInstance(),
                    runloop_.get());
                if (result != DAS_S_OK)
                {
                    DAS_CORE_LOG_ERROR(
                        "ProxyFactory initialization failed, result = 0x{:08X}",
                        result);
                    object_manager_.reset();
                    runloop_.reset();
                    throw std::runtime_error(
                        "ProxyFactory initialization failed");
                }

                is_initialized_ = true;
            }

            IpcContext::~IpcContext()
            {
                // 析构即清理
                if (is_initialized_)
                {
                    Uninitialize();
                }
            }

            void IpcContext::Uninitialize()
            {
                if (!is_initialized_)
                {
                    return;
                }

                // Clear ProxyFactory
                auto& proxy_factory = ProxyFactory::GetInstance();
                proxy_factory.ClearAllProxies();

                // Destroy in reverse order
                object_manager_.reset();

                // RAII：unique_ptr 析构自动调用 Uninitialize
                if (runloop_)
                {
                    runloop_->RequestStop();
                    runloop_.reset(); // 析构函数会自动调用 Uninitialize()
                }

                is_initialized_ = false;
            }

            DistributedObjectManager& IpcContext::GetObjectManager()
            {
                return *object_manager_;
            }

            ProxyFactory& IpcContext::GetProxyFactory()
            {
                return ProxyFactory::GetInstance();
            }

            RemoteObjectRegistry& IpcContext::GetRegistry()
            {
                return RemoteObjectRegistry::GetInstance();
            }

            boost::asio::io_context& IpcContext::GetIoContext()
            {
                if (!runloop_)
                {
                    throw std::runtime_error("IpcContext not initialized");
                }
                return runloop_->GetIoContext();
            }

            DasResult IpcContext::CreateHostLauncher(
                IHostLauncher** pp_out_launcher)
            {
                if (!pp_out_launcher)
                {
                    return DAS_E_INVALID_ARGUMENT;
                }

                try
                {
                    *pp_out_launcher = new HostLauncher(GetIoContext());
                    return DAS_S_OK;
                }
                catch (const std::exception& e)
                {
                    DAS_CORE_LOG_ERROR(
                        "CreateHostLauncher: {}",
                        ToString(e.what()));
                    return DAS_E_IPC_NOT_INITIALIZED;
                }
            }

            void IpcContext::PostCallback(IDasAsyncCallback* callback)
            {
                if (!callback || !runloop_)
                {
                    return;
                }

                // DasPtr 在 post 之前获取所有权（AddRef），保证生命周期安全
                DasPtr<IDasAsyncCallback> ptr(callback);
                boost::asio::post(
                    runloop_->GetIoContext(),
                    [ptr = std::move(ptr)]() { ptr->Do(); });
            }

            DasResult IpcContext::LoadPluginAsync(
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

                if (!runloop_)
                {
                    DAS_CORE_LOG_ERROR(
                        "LoadPluginAsync: IpcRunLoop not initialized");
                    return DAS_E_IPC_NOT_INITIALIZED;
                }

                // Get Transport via ConnectionManager
                auto* conn_mgr = runloop_->GetConnectionManager();
                if (!conn_mgr)
                {
                    DAS_CORE_LOG_ERROR(
                        "LoadPluginAsync: No ConnectionManager available");
                    return DAS_E_IPC_NOT_INITIALIZED;
                }

                auto* transport = conn_mgr->GetTransport(session_id);
                if (!transport)
                {
                    DAS_CORE_LOG_ERROR(
                        "LoadPluginAsync: No Transport found for session_id = {}",
                        session_id);
                    return DAS_E_IPC_OBJECT_NOT_FOUND;
                }

                // 2. 构建 LOAD_PLUGIN 请求
                std::string plugin_path(u8_plugin_path);
                uint16_t path_len = static_cast<uint16_t>(plugin_path.size());

                // 载荷格式: uint16_t path_len + char[] path (不含 null
                // terminator)
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

                // 4. 发送异步请求（使用指定 transport）
                auto sender = runloop_->SendMessageAsync(
                    transport,
                    header,
                    payload.data(),
                    payload.size(),
                    timeout);

                // 5. 包装为 IDasAsyncLoadPluginOperation
                auto op =
                    MakeAsyncOperation<IDasAsyncLoadPluginOperation, ObjectId>(
                        std::move(sender));

                *pp_out_operation = op.Get();
                if (*pp_out_operation)
                {
                    (*pp_out_operation)->AddRef();
                }
                return DAS_S_OK;
            }

            DasResult IpcContext::Run()
            {
                if (!runloop_)
                {
                    DAS_CORE_LOG_ERROR("Run: IpcRunLoop not initialized");
                    return DAS_E_IPC_NOT_INITIALIZED;
                }
                return runloop_->Run();
            }

            void IpcContext::RequestStop()
            {
                if (runloop_)
                {
                    runloop_->RequestStop();
                }
            }

            DasResult IpcContext::RegisterHostLauncher(
                DasPtr<IHostLauncher> launcher)
            {
                if (!runloop_)
                {
                    DAS_CORE_LOG_ERROR(
                        "RegisterHostLauncher: IpcRunLoop not initialized");
                    return DAS_E_IPC_NOT_INITIALIZED;
                }

                auto* conn_mgr = runloop_->GetConnectionManager();
                if (!conn_mgr)
                {
                    DAS_CORE_LOG_ERROR(
                        "RegisterHostLauncher: ConnectionManager not initialized");
                    return DAS_E_IPC_NOT_INITIALIZED;
                }

                if (!launcher)
                {
                    DAS_CORE_LOG_ERROR(
                        "RegisterHostLauncher: launcher is null");
                    return DAS_E_INVALID_ARGUMENT;
                }

                uint16_t session_id = launcher->GetSessionId();

                // 转移 DasPtr 所有权到 ConnectionManager
                return conn_mgr->RegisterHostLauncher(
                    session_id,
                    std::move(launcher));
            }

            std::vector<uint16_t> IpcContext::GetConnectedSessions()
            {
                if (!runloop_)
                {
                    return {};
                }
                auto* conn_mgr = runloop_->GetConnectionManager();
                if (!conn_mgr)
                {
                    return {};
                }
                return conn_mgr->GetConnectedSessions();
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
