#include <IpcProxyFactory.h>
#include <boost/asio/post.hpp>
#include <cstring>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/IPC/AsyncOperationImpl.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <das/Core/IPC/DasProxyBase.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/LoadPluginAsyncOperationImpl.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/IDasAsyncCallback.h>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>

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

            IpcContext::IpcContext(bool enable_heartbeat)
            {
                // 构造即初始化
                DasResult result = DAS_S_OK;

                // 1. Create IpcRunLoop FIRST (provides io_context)
                auto runloop_result = IpcRunLoopType::Create(enable_heartbeat);
                if (!runloop_result.has_value())
                {
                    DAS_CORE_LOG_ERROR("IpcRunLoop::Create() failed");
                    throw std::runtime_error("IpcRunLoop::Create() failed");
                }
                runloop_ = std::move(runloop_result.value());
                // Create() 已自动完成初始化，无需再调用 Initialize()

                // 2. Set inbound_queue to IpcRunLoop
                runloop_->SetInboundQueue(&inbound_queue_);

                // 3. 主进程 session_id = 1
                runloop_->SetSessionId(1);

                // 4. Initialize reserved session IDs
                for (uint16_t reserved_id : reserved_session_ids_)
                {
                    allocated_ids_[reserved_id] = true;
                }

                // 5. DistributedObjectManager 绑定 IpcRunLoop
                object_manager_.SetRunLoop(runloop_.get());

                // 6. Create BusinessThread
                business_thread_ = std::make_shared<BusinessThread>(
                    inbound_queue_,
                    *runloop_,
                    *this);

                // 7. Initialize ProxyFactory with runloop_
                auto& proxy_factory = ProxyFactory::GetInstance();
                result = proxy_factory.Initialize(
                    &object_manager_,
                    &RemoteObjectRegistry::GetInstance(),
                    runloop_.get());
                if (result != DAS_S_OK)
                {
                    DAS_CORE_LOG_ERROR(
                        "ProxyFactory initialization failed, result = {}",
                        result);
                    runloop_.reset();
                    throw std::runtime_error(
                        "ProxyFactory initialization failed");
                }

                // 8. 创建并初始化 IpcCommandHandler
                command_handler_ = IpcCommandHandler::Create();
                command_handler_->SetSessionId(1);

                // 9. 注册 LOOKUP_BY_INTERFACE 到 IpcRunLoop
                runloop_->RegisterHandler(
                    HeaderFlags::BUSINESS_CONTROL,
                    static_cast<uint32_t>(IpcCommandType::LOOKUP_BY_INTERFACE),
                    command_handler_.Get());

                // REMOTE_RELEASE (fire-and-forget EVENT)
                runloop_->RegisterHandler(
                    HeaderFlags::BUSINESS_CONTROL,
                    static_cast<uint32_t>(IpcCommandType::REMOTE_RELEASE),
                    command_handler_.Get());

                // RELEASE_SHM_BLOCK (fire-and-forget EVENT)
                runloop_->RegisterHandler(
                    HeaderFlags::BUSINESS_CONTROL,
                    static_cast<uint32_t>(IpcCommandType::RELEASE_SHM_BLOCK),
                    command_handler_.Get());

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

                // 关闭顺序：inbound_queue -> business_thread -> io_context
                // 1. 关闭入站队列，让业务线程的 Pop() 返回 nullopt
                inbound_queue_.Uninitialize();

                // 2. 停止业务线程（会 join）
                if (business_thread_)
                {
                    business_thread_->Stop();
                    business_thread_.reset();
                }

                // 3. 停止 IO 线程
                if (runloop_)
                {
                    runloop_->RequestStop();
                    runloop_.reset();
                }

                // object_manager_ is value member, automatically destructed
                is_initialized_ = false;
            }

            DistributedObjectManager& IpcContext::GetObjectManager()
            {
                return object_manager_;
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
                    auto* launcher = new HostLauncher(GetIoContext());
                    launcher->SetIpcContext(this);

                    // Store DasPtr internally for auto-registration after Start
                    launcher_ = DAS::DasPtr<HostLauncher>(launcher);

                    *pp_out_launcher = launcher;
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
                auto header = MakeBusinessControlRequest(
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
                auto op = MakeLoadPluginAsyncOperation(std::move(sender), this);

                *pp_out_operation = op.Get();
                if (*pp_out_operation)
                {
                    (*pp_out_operation)->AddRef();
                }
                return DAS_S_OK;
            }

            DasResult IpcContext::Run()
            {
                ScopedCurrentIpcContext scope(this);

                if (!runloop_)
                {
                    DAS_CORE_LOG_ERROR("Run: IpcRunLoop not initialized");
                    return DAS_E_IPC_NOT_INITIALIZED;
                }

                // 启动业务线程
                if (business_thread_)
                {
                    business_thread_->Start(object_manager_);
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

            DasResult IpcContext::InternalRegisterHostLauncher()
            {
                if (!runloop_)
                {
                    DAS_CORE_LOG_ERROR(
                        "InternalRegisterHostLauncher: IpcRunLoop not initialized");
                    return DAS_E_IPC_NOT_INITIALIZED;
                }

                auto* conn_mgr = runloop_->GetConnectionManager();
                if (!conn_mgr)
                {
                    DAS_CORE_LOG_ERROR(
                        "InternalRegisterHostLauncher: ConnectionManager not initialized");
                    return DAS_E_IPC_NOT_INITIALIZED;
                }

                if (!launcher_)
                {
                    DAS_CORE_LOG_ERROR(
                        "InternalRegisterHostLauncher: no launcher stored");
                    return DAS_E_INVALID_ARGUMENT;
                }

                // Use internally stored DasPtr for registration
                return runloop_->RegisterHostLauncher(launcher_);
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

            DAS_API IIpcContext* CreateIpcContext(bool enable_heartbeat)
            {
                try
                {
                    // 构造即初始化（RAII）
                    auto* context{new IpcContext(enable_heartbeat)};
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

            const uint16_t IpcContext::reserved_session_ids_[3] = {
                0,
                1,
                0xFFFF};

            uint16_t IpcContext::FindAvailableSessionId()
            {
                uint16_t start_id = next_session_id_.load();
                uint16_t current_id = start_id;

                do
                {
                    if (!allocated_ids_[current_id])
                    {
                        bool is_reserved = false;
                        for (uint16_t reserved_id : reserved_session_ids_)
                        {
                            if (current_id == reserved_id)
                            {
                                is_reserved = true;
                                break;
                            }
                        }

                        if (!is_reserved)
                        {
                            next_session_id_.store(
                                (current_id + 1) % (65536 - 1));
                            if (next_session_id_.load() <= 1)
                            {
                                next_session_id_.store(2);
                            }
                            return current_id;
                        }
                    }

                    current_id++;
                    // uint16_t 溢出后变成 0，重置到 2（避免使用保留的
                    // session_id 0 和 1）
                    if (current_id == 0)
                    {
                        current_id = 2;
                    }

                    if (current_id == start_id)
                    {
                        break;
                    }
                } while (true);

                return 0;
            }

            void IpcContext::MarkSessionIdAsAllocated(uint16_t session_id)
            {
                allocated_ids_[session_id] = true;
            }

            void IpcContext::MarkSessionIdAsFree(uint16_t session_id)
            {
                allocated_ids_[session_id] = false;
            }

            uint16_t IpcContext::AllocateSessionId()
            {
                std::lock_guard<std::mutex> lock(allocated_ids_mutex_);

                uint16_t available_id = FindAvailableSessionId();
                if (available_id == 0)
                {
                    DAS_CORE_LOG_ERROR("No available session_id");
                    return 0;
                }

                MarkSessionIdAsAllocated(available_id);
                return available_id;
            }

            void IpcContext::ReleaseSessionId(uint16_t session_id)
            {
                std::lock_guard<std::mutex> lock(allocated_ids_mutex_);
                MarkSessionIdAsFree(session_id);
            }

            DasResult IpcContext::CreateRemoteProxy(
                ObjectId       object_id,
                const DasGuid& iid,
                IDasBase**     pp_out)
            {
                if (!pp_out)
                {
                    return DAS_E_INVALID_ARGUMENT;
                }
                *pp_out = nullptr;

                if (!runloop_ || !business_thread_)
                {
                    DAS_CORE_LOG_ERROR(
                        "CreateRemoteProxy: IpcRunLoop or BusinessThread not initialized");
                    return DAS_E_IPC_NOT_INITIALIZED;
                }

                // Convert DasGuid (UUID) to uint32_t FNV-1a hash using the same
                // algorithm as RemoteObjectRegistry::ComputeInterfaceId
                uint32_t interface_hash =
                    RemoteObjectRegistry::ComputeInterfaceId(iid);

                // Create proxy directly based on interface hash using factory
                IDasBase* proxy = DasIpcProxy::CreateProxyByInterfaceId(
                    interface_hash,
                    object_id,
                    *runloop_,
                    business_thread_,
                    object_manager_);

                if (!proxy)
                {
                    DAS_CORE_LOG_ERROR(
                        "CreateRemoteProxy: failed to create proxy "
                        "for object_id={{session:{}, gen:{}, local:{}}}, interface_hash={}",
                        object_id.session_id,
                        object_id.generation,
                        object_id.local_id,
                        interface_hash);
                    return DAS_E_NO_INTERFACE;
                }

                // Register remote object so DistributedObjectManager can track
                // it
                object_manager_.RegisterRemoteObject(object_id);

                *pp_out = proxy;
                return DAS_S_OK;
            }

            DasResult IpcContext::ResolveMainProcessInterface(
                const DasGuid& iid,
                IDasBase**     pp_out_object)
            {
                if (!pp_out_object)
                {
                    return DAS_E_INVALID_ARGUMENT;
                }
                *pp_out_object = nullptr;

                uint32_t interface_id =
                    RemoteObjectRegistry::ComputeInterfaceId(iid);
                RemoteObjectInfo info;
                DasResult        lookup_result =
                    RemoteObjectRegistry::GetInstance().LookupByInterface(
                        interface_id,
                        info);
                if (DAS::IsFailed(lookup_result))
                {
                    return DAS_E_IPC_OBJECT_NOT_FOUND;
                }

                IDasBase* local_obj = nullptr;
                DasResult result =
                    object_manager_.LookupObject(info.object_id, &local_obj);
                if (DAS::IsFailed(result) || !local_obj)
                {
                    return DAS_E_IPC_OBJECT_NOT_FOUND;
                }

                *pp_out_object = local_obj;
                return DAS_S_OK;
            }

            DasResult IpcContext::RegisterService(
                IDasBase*      p_object,
                const DasGuid& iid)
            {
                // 1. Parameter validation
                if (!p_object)
                {
                    return DAS_E_INVALID_POINTER;
                }

                // 2. Register to DistributedObjectManager (AddRef)
                ObjectId obj_id;
                auto     result =
                    object_manager_.RegisterLocalObject(p_object, obj_id);
                if (DAS::IsFailed(result))
                {
                    return result;
                }

                // 3. Auto-generate name and register to RemoteObjectRegistry
                //    RemoteObjectRegistry::RegisterObject requires non-empty
                //    name
                auto name = DAS::fmt::format("{}", iid);
                auto session_id = runloop_->GetSessionId();
                result = RemoteObjectRegistry::GetInstance()
                             .RegisterObject(obj_id, iid, session_id, name);

                // 4. Partial failure rollback
                if (DAS::IsFailed(result))
                {
                    object_manager_.UnregisterObject(obj_id);
                    return result;
                }

                return DAS_S_OK;
            }

            DasResult IpcContext::UnregisterService(const DasGuid& iid)
            {
                // 1. Lookup by IID
                auto interface_id =
                    RemoteObjectRegistry::ComputeInterfaceId(iid);

                RemoteObjectInfo info;
                auto             result =
                    RemoteObjectRegistry::GetInstance().LookupByInterface(
                        interface_id,
                        info);
                if (DAS::IsFailed(result))
                {
                    return DAS_E_IPC_OBJECT_NOT_FOUND;
                }

                // 2. Remove from Registry first
                result = RemoteObjectRegistry::GetInstance().UnregisterObject(
                    info.object_id);
                if (DAS::IsFailed(result))
                {
                    return result;
                }

                // 3. Release from DistributedObjectManager
                object_manager_.UnregisterObject(info.object_id);
                return DAS_S_OK;
            }

        } // namespace MainProcess
    } // namespace IPC
} // namespace Core
DAS_NS_END
