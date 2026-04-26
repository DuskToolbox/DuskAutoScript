#include <IpcProxyFactory.h>
#include <boost/asio/post.hpp>
#include <cassert>
#include <cstring>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/IPC/AsyncOperationImpl.h>
#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/DasProxyBase.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/HostLauncher.h>
#include <das/Core/IPC/InternalCallbackHandler.h>
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

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        namespace MainProcess
        {
            // ====== IpcContext 实现（RAII 风格，无 pimpl）======

            IpcContext::IpcContext(bool enable_heartbeat)
                : proxy_factory_(std::in_place), runloop_(
                                                     enable_heartbeat,
                                                     &inbound_queue_,
                                                     *proxy_factory_,
                                                     registry_)
            {
                // 1. 设置 session_id
                proxy_factory_->GetObjectManager().SetSessionId(1);
                runloop_.SetSessionId(1);

                // 2. Initialize reserved session IDs
                for (uint16_t reserved_id : reserved_session_ids_)
                {
                    allocated_ids_[reserved_id] = true;
                }

                // 3. Create BusinessThread（构造即启动线程）
                business_thread_ = std::make_shared<BusinessThread>(
                    inbound_queue_,
                    runloop_,
                    *this,
                    *proxy_factory_,
                    registry_);

                // 4. 创建并初始化 IpcCommandHandler
                command_handler_ = IpcCommandHandler::Create(registry_);
                command_handler_->SetSessionId(1);

                // 5. 创建并注册 InternalCallbackHandler
                internal_callback_handler_ = DasPtr<InternalCallbackHandler>(
                    new InternalCallbackHandler());
                runloop_.RegisterHandler(
                    HeaderFlags::BUSINESS_CONTROL,
                    static_cast<uint32_t>(
                        InternalBusinessCommand::ASYNC_CALLBACK),
                    internal_callback_handler_.Get());

                // 6. 注册 LOOKUP_BY_INTERFACE 到 IpcRunLoop
                runloop_.RegisterHandler(
                    HeaderFlags::BUSINESS_CONTROL,
                    static_cast<uint32_t>(IpcCommandType::LOOKUP_BY_INTERFACE),
                    command_handler_.Get());

                runloop_.RegisterHandler(
                    HeaderFlags::BUSINESS_CONTROL,
                    static_cast<uint32_t>(IpcCommandType::REMOTE_RELEASE),
                    command_handler_.Get());

                runloop_.RegisterHandler(
                    HeaderFlags::BUSINESS_CONTROL,
                    static_cast<uint32_t>(IpcCommandType::RELEASE_SHM_BLOCK),
                    command_handler_.Get());
            }

            IpcContext::~IpcContext()
            {
                // 析构即清理，无条件守卫

                // 生命周期安全：重置所有 HostLauncher 的回调，防止
                // ConnectionManager 持有的 DasPtr<HostLauncher>
                // 在 IpcContext 析构后仍触发悬空回调
                for (auto& [sid, launcher] : launchers_)
                {
                    if (launcher)
                    {
                        launcher->ClearCallbacks();
                    }
                }

                // Clear ProxyFactory
                if (proxy_factory_.has_value())
                {
                    proxy_factory_->ClearAllProxies();
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
                runloop_.RequestStop();

                // runloop_ 是值成员，析构时自动清理
                // proxy_factory_ 是 optional 值成员，自动析构
            }

            ProxyFactory& IpcContext::GetProxyFactory()
            {
                return *proxy_factory_;
            }

            boost::asio::io_context& IpcContext::GetIoContext()
            {
                return runloop_.GetIoContext();
            }

            DasResult IpcContext::CreateHostLauncher(
                IHostLauncher** pp_out_launcher)
            {
                if (!pp_out_launcher)
                {
                    return DAS_E_INVALID_ARGUMENT;
                }

                // 预分配 session_id，作为值传入 HostLauncher 构造函数
                uint16_t session_id = AllocateSessionId();
                if (session_id == 0)
                {
                    DAS_CORE_LOG_ERROR(
                        "Failed to allocate session_id for HostLauncher");
                    return DAS_E_IPC_SESSION_ALLOC_FAILED;
                }

                try
                {
                    // 注入 on_register 回调，IpcContext 自行调用
                    // InternalRegisterHostLauncher
                    auto launcher = DAS::DasPtr<HostLauncher>(new HostLauncher(
                        GetIoContext(),
                        session_id,
                        [this, session_id]()
                        { return InternalRegisterHostLauncher(session_id); }));

                    launchers_[session_id] = launcher;
                    *pp_out_launcher = launcher.Get();
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
                if (!callback)
                {
                    return;
                }

                // DasPtr 在 post 之前获取所有权（AddRef），保证生命周期安全
                DasPtr<IDasAsyncCallback> ptr(callback);
                boost::asio::post(
                    runloop_.GetIoContext(),
                    [ptr = std::move(ptr)]() { ptr->Do(); });
            }

            void IpcContext::PostToBusinessThread(IDasAsyncCallback* callback)
            {
                if (!callback)
                {
                    return;
                }

                // AddRef 保持生命周期（body 持有的引用）
                callback->AddRef();

                // body 编码 callback 原始指针
                std::vector<uint8_t> body(sizeof(IDasAsyncCallback*));
                std::memcpy(body.data(), &callback, sizeof(IDasAsyncCallback*));

                // 构造 header: BUSINESS_CONTROL +
                // InternalBusinessCommand::ASYNC_CALLBACK
                auto header =
                    IPCMessageHeaderBuilder()
                        .SetMessageType(MessageType::EVENT)
                        .SetHeaderFlags(HeaderFlags::BUSINESS_CONTROL)
                        .SetInterfaceId(
                            static_cast<uint32_t>(
                                InternalBusinessCommand::ASYNC_CALLBACK))
                        .SetBodySize(static_cast<uint32_t>(body.size()))
                        .Build();

                InboundMessage msg;
                msg.header = header;
                msg.body = std::move(body);

                DasResult push_result = inbound_queue_.Push(std::move(msg));
                if (DAS::IsFailed(push_result))
                {
                    // Push 失败，用 DasPtr::Attach 释放 callback 引用
                    auto released = DasPtr<IDasAsyncCallback>::Attach(callback);
                    (void)released;
                    DAS_CORE_LOG_ERROR(
                        "PostToBusinessThread: Push failed, result={}",
                        push_result);
                }
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

                // Get Transport via ConnectionManager
                if (!runloop_.connection_manager_)
                {
                    DAS_CORE_LOG_ERROR(
                        "LoadPluginAsync: No ConnectionManager available");
                    return DAS_E_IPC_NOT_INITIALIZED;
                }

                auto& conn_mgr = runloop_.GetConnectionManager();
                auto* transport = conn_mgr.GetTransport(session_id);
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
                auto sender = runloop_.SendMessageAsync(
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

                return runloop_.Run();
            }

            void IpcContext::RequestStop() { runloop_.RequestStop(); }

            DasResult IpcContext::InternalRegisterHostLauncher(
                uint16_t session_id)
            {
                if (!runloop_.connection_manager_)
                {
                    DAS_CORE_LOG_ERROR(
                        "InternalRegisterHostLauncher: ConnectionManager not initialized");
                    return DAS_E_IPC_NOT_INITIALIZED;
                }

                auto it = launchers_.find(session_id);
                if (it == launchers_.end() || !it->second)
                {
                    DAS_CORE_LOG_ERROR(
                        "InternalRegisterHostLauncher: no launcher for session_id={}",
                        session_id);
                    return DAS_E_INVALID_ARGUMENT;
                }

                return runloop_.RegisterHostLauncher(it->second);
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

                if (!business_thread_)
                {
                    DAS_CORE_LOG_ERROR(
                        "CreateRemoteProxy: BusinessThread not initialized");
                    return DAS_E_IPC_NOT_INITIALIZED;
                }

                /**
                 * BusinessThread-only guard: CreateRemoteProxy creates/caches
                 * proxy and registers remote object in DOM. This operation
                 * must run on BusinessThread because DOM/Registry/ProxyCache
                 * are single-threaded BT-owned state. Non-BT callers are
                 * programming errors — they must schedule proxy creation on
                 * BusinessThread before calling this method.
                 */
                if (!business_thread_->IsCurrentThread())
                {
                    DAS_CORE_LOG_ERROR(
                        "CreateRemoteProxy must be called on BusinessThread");
                    assert(
                        false
                        && "CreateRemoteProxy must run on BusinessThread");
                    return DAS_E_UNEXPECTED_THREAD_DETECTED;
                }

                // Convert DasGuid (UUID) to uint32_t FNV-1a hash using the same
                // algorithm as RemoteObjectRegistry::ComputeInterfaceId
                uint32_t interface_hash =
                    RemoteObjectRegistry::ComputeInterfaceId(iid);

                // Create proxy using unified GetOrCreateProxy (cache +
                // RegisterRemoteObject)
                DasPtr<IDasBase> proxy = proxy_factory_->GetOrCreateProxy(
                    runloop_,
                    business_thread_,
                    object_id,
                    interface_hash);

                if (!proxy)
                {
                    DAS_CORE_LOG_ERROR(
                        "CreateRemoteProxy: failed to create proxy "
                        "for object_id={{session:{}, gen:{}, local:{}}}, interface_hash=0x{:08X}",
                        object_id.session_id,
                        object_id.generation,
                        object_id.local_id,
                        interface_hash);
                    return DAS_E_NO_INTERFACE;
                }

                *pp_out = proxy.Get();
                (*pp_out)->AddRef();
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

                /**
                 * ResolveMainProcessInterface must run on BusinessThread
                 * because it accesses RemoteObjectRegistry and
                 * DistributedObjectManager (BT single-threaded state).
                 *
                 * BT fast path: direct execution (zero overhead).
                 * Non-BT path: schedule_on_business_thread + synchronous wait
                 * (caller expects synchronous return).
                 */
                if (business_thread_ && business_thread_->IsCurrentThread())
                {
                    return ResolveMainProcessInterfaceImpl(iid, pp_out_object);
                }

                auto result = wait(
                    stdexec::let_value(
                        stdexec::schedule(schedule_on_business_thread(*this)),
                        [this, iid]()
                        {
                            IDasBase* obj = nullptr;
                            auto      hr =
                                ResolveMainProcessInterfaceImpl(iid, &obj);
                            // DasPtr(obj) AddRefs; when the pair is
                            // destructed it Releases, leaving the caller's
                            // AddRef from LookupObject intact.
                            return stdexec::just(
                                std::make_pair(hr, DasPtr<IDasBase>(obj)));
                        }));

                if (!result.has_value())
                {
                    return DAS_E_IPC_TIMEOUT;
                }
                auto& [hr, ptr] = std::get<0>(*result);
                if (IsOk(hr))
                {
                    // ptr.Get() is valid until ptr destructs.
                    // AddRef for caller; ptr's destructor will Release its
                    // own reference.
                    if (ptr)
                    {
                        ptr->AddRef();
                    }
                    *pp_out_object = ptr.Get();
                }
                return hr;
            }

            DasResult IpcContext::ResolveMainProcessInterfaceImpl(
                const DasGuid& iid,
                IDasBase**     pp_out_object)
            {
                uint32_t interface_id =
                    RemoteObjectRegistry::ComputeInterfaceId(iid);
                RemoteObjectInfo info;
                DasResult        lookup_result =
                    registry_.LookupByInterface(interface_id, info);
                if (DAS::IsFailed(lookup_result))
                {
                    return DAS_E_IPC_OBJECT_NOT_FOUND;
                }

                IDasBase* local_obj = nullptr;
                DasResult result =
                    proxy_factory_->GetObjectManager().LookupObject(
                        info.object_id,
                        &local_obj);
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
                if (business_thread_ && business_thread_->IsCurrentThread())
                {
                    return RegisterServiceImpl(p_object, iid);
                }

                auto result = wait(
                    stdexec::let_value(
                        stdexec::schedule(schedule_on_business_thread(*this)),
                        [this, p_object, iid]()
                        {
                            return stdexec::just(
                                RegisterServiceImpl(p_object, iid));
                        }));

                if (!result.has_value())
                {
                    return DAS_E_IPC_TIMEOUT;
                }
                return std::get<0>(*result);
            }

            DasResult IpcContext::RegisterServiceImpl(
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
                    proxy_factory_->GetObjectManager().RegisterLocalObject(
                        p_object,
                        obj_id);
                if (DAS::IsFailed(result))
                {
                    return result;
                }

                // 3. Auto-generate name and register to RemoteObjectRegistry
                auto name = DAS::fmt::format("{}", iid);
                auto session_id = runloop_.GetSessionId();
                result =
                    registry_.RegisterObject(obj_id, iid, session_id, name);

                // 4. Partial failure rollback
                if (DAS::IsFailed(result))
                {
                    proxy_factory_->GetObjectManager().UnregisterObject(obj_id);
                    return result;
                }

                return DAS_S_OK;
            }

            DasResult IpcContext::UnregisterService(const DasGuid& iid)
            {
                if (business_thread_ && business_thread_->IsCurrentThread())
                {
                    return UnregisterServiceImpl(iid);
                }

                auto result = wait(
                    stdexec::let_value(
                        stdexec::schedule(schedule_on_business_thread(*this)),
                        [this, iid]()
                        { return stdexec::just(UnregisterServiceImpl(iid)); }));

                if (!result.has_value())
                {
                    return DAS_E_IPC_TIMEOUT;
                }
                return std::get<0>(*result);
            }

            DasResult IpcContext::UnregisterServiceImpl(const DasGuid& iid)
            {
                // 1. Lookup by IID
                auto interface_id =
                    RemoteObjectRegistry::ComputeInterfaceId(iid);

                RemoteObjectInfo info;
                auto result = registry_.LookupByInterface(interface_id, info);
                if (DAS::IsFailed(result))
                {
                    return DAS_E_IPC_OBJECT_NOT_FOUND;
                }

                // 2. Remove from Registry first
                result = registry_.UnregisterObject(info.object_id);
                if (DAS::IsFailed(result))
                {
                    return result;
                }

                // 3. Release from DistributedObjectManager
                proxy_factory_->GetObjectManager().UnregisterObject(
                    info.object_id);
                return DAS_S_OK;
            }

        } // namespace MainProcess
    } // namespace IPC
} // namespace Core
DAS_NS_END
