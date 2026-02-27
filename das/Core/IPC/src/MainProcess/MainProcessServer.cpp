#include "das/Core/IPC/MainProcess/MainProcessServer.h"

#include <chrono>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/IpcTransport.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/SessionCoordinator.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>

#ifdef _WIN32
#include <windows.h>
// 取消 Windows API 宏，避免与代码冲突
#ifdef DispatchMessage
#undef DispatchMessage
#endif
#else
#include <unistd.h>
#endif

#ifndef DAS_E_NOT_IMPLEMENTED
#define DAS_E_NOT_IMPLEMENTED DAS_E_NO_IMPLEMENTATION
#endif

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        namespace MainProcess
        {
            MainProcessServer& MainProcessServer::GetInstance()
            {
                static MainProcessServer instance;
                return instance;
            }

            MainProcessServer::MainProcessServer() = default;

            MainProcessServer::~MainProcessServer()
            {
                if (is_initialized_.load())
                {
                    Shutdown();
                }
            }

            DasResult MainProcessServer::Initialize()
            {
                if (is_initialized_.load())
                {
                    return DAS_S_OK;
                }

                // 获取主进程 PID
#ifdef _WIN32
                main_pid_ = GetCurrentProcessId();
#else
                main_pid_ = static_cast<uint32_t>(getpid());
#endif

                // 主进程 session_id = 1
                SessionCoordinator::GetInstance().SetLocalSessionId(1);

                is_initialized_.store(true);
                return DAS_S_OK;
            }

            DasResult MainProcessServer::Shutdown()
            {
                if (!is_initialized_.load())
                {
                    return DAS_S_OK;
                }

                Stop();
                StopListening();

                {
                    std::lock_guard<std::mutex> lock(sessions_mutex_);
                    sessions_.clear();
                }

                {
                    std::lock_guard<std::mutex> lock(host_pid_mutex_);
                    host_pid_to_session_.clear();
                }

                RemoteObjectRegistry::GetInstance().Clear();

                is_initialized_.store(false);
                return DAS_S_OK;
            }

            DasResult MainProcessServer::Start()
            {
                if (!is_initialized_.load())
                {
                    return DAS_E_IPC_INVALID_STATE;
                }

                if (is_running_.load())
                {
                    return DAS_S_OK;
                }

                is_running_.store(true);
                return DAS_S_OK;
            }

            DasResult MainProcessServer::Stop()
            {
                if (!is_running_.load())
                {
                    return DAS_S_OK;
                }

                is_running_.store(false);
                return DAS_S_OK;
            }

            bool MainProcessServer::IsRunning() const
            {
                return is_running_.load();
            }

            DasResult MainProcessServer::StartListening()
            {
                if (!is_initialized_.load())
                {
                    return DAS_E_IPC_INVALID_STATE;
                }

                if (is_listening_.load())
                {
                    return DAS_S_OK;
                }

                if (main_pid_ == 0)
                {
                    std::string msg = DAS_FMT_NS::format(
                        "StartListening failed: main_pid_ is 0");
                    DAS_LOG_ERROR(msg.c_str());
                    return DAS_E_IPC_INVALID_STATE;
                }

                // 创建监听队列名: das_ipc_{main_pid}_0_m2h/h2m
                std::string m2h_queue = IpcTransport::MakeQueueName(
                    main_pid_, 0, true);
                std::string h2m_queue = IpcTransport::MakeQueueName(
                    main_pid_, 0, false);

                // 创建监听传输
                listen_transport_ = std::make_unique<IpcTransport>();
                DasResult result = listen_transport_->Initialize(
                    m2h_queue,
                    h2m_queue,
                    4096,  // max_message_size
                    100);  // max_messages

                if (DAS::IsFailed(result))
                {
                    std::string msg = DAS_FMT_NS::format(
                        "StartListening failed to initialize transport: {}",
                        static_cast<int32_t>(result));
                    DAS_LOG_ERROR(msg.c_str());
                    listen_transport_.reset();
                    return result;
                }

                is_listening_.store(true);

                // 启动监听线程
                listen_thread_ = std::thread(&MainProcessServer::ListenLoop, this);

                {
                    std::string msg = DAS_FMT_NS::format(
                        "MainProcessServer started listening on queues: {} / {}",
                        m2h_queue,
                        h2m_queue);
                    DAS_LOG_INFO(msg.c_str());
                }

                return DAS_S_OK;
            }

            DasResult MainProcessServer::StopListening()
            {
                if (!is_listening_.load())
                {
                    return DAS_S_OK;
                }

                is_listening_.store(false);

                // 等待监听线程结束
                if (listen_thread_.joinable())
                {
                    listen_thread_.join();
                }

                // 关闭传输
                if (listen_transport_)
                {
                    listen_transport_->Shutdown();
                    listen_transport_.reset();
                }

                DAS_LOG_INFO("MainProcessServer stopped listening");
                return DAS_S_OK;
            }

            bool MainProcessServer::IsListening() const
            {
                return is_listening_.load();
            }

            uint32_t MainProcessServer::GetMainPid() const
            {
                return main_pid_;
            }

            DasResult MainProcessServer::OnHostConnected(uint16_t session_id)
            {
                if (!is_initialized_.load())
                {
                    return DAS_E_IPC_INVALID_STATE;
                }

                if (!ValidateSessionId(session_id))
                {
                    return DAS_E_INVALID_ARGUMENT;
                }

                std::lock_guard<std::mutex> lock(sessions_mutex_);

                auto it = sessions_.find(session_id);
                if (it != sessions_.end())
                {
                    if (!it->second.is_connected)
                    {
                        it->second.is_connected = true;
                        it->second.last_active_ms = GetCurrentTimeMs();
                    }
                    else
                    {
                        return DAS_E_DUPLICATE_ELEMENT;
                    }
                }
                else
                {
                    HostSessionInfo info{};
                    info.session_id = session_id;
                    info.is_connected = true;
                    info.connect_time_ms = GetCurrentTimeMs();
                    info.last_active_ms = info.connect_time_ms;
                    sessions_[session_id] = info;
                }

                if (on_session_connected_)
                {
                    on_session_connected_(session_id);
                }

                return DAS_S_OK;
            }

            DasResult MainProcessServer::OnHostDisconnected(uint16_t session_id)
            {
                if (!is_initialized_.load())
                {
                    return DAS_E_IPC_INVALID_STATE;
                }

                std::lock_guard<std::mutex> lock(sessions_mutex_);

                auto it = sessions_.find(session_id);
                if (it == sessions_.end())
                {
                    return DAS_E_IPC_OBJECT_NOT_FOUND;
                }

                it->second.is_connected = false;

                {
                    std::vector<RemoteObjectInfo> objects;
                    RemoteObjectRegistry::GetInstance().ListObjectsBySession(
                        session_id,
                        objects);

                    for (const auto& obj : objects)
                    {
                        RemoteObjectRegistry::GetInstance().UnregisterObject(
                            obj.object_id);

                        if (on_object_unregistered_)
                        {
                            on_object_unregistered_(obj);
                        }
                    }
                }

                if (on_session_disconnected_)
                {
                    on_session_disconnected_(session_id);
                }

                return DAS_S_OK;
            }

            bool MainProcessServer::IsSessionConnected(
                uint16_t session_id) const
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);

                auto it = sessions_.find(session_id);
                if (it == sessions_.end())
                {
                    return false;
                }

                return it->second.is_connected;
            }

            std::vector<uint16_t> MainProcessServer::GetConnectedSessions()
                const
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);

                std::vector<uint16_t> connected;
                for (const auto& pair : sessions_)
                {
                    if (pair.second.is_connected)
                    {
                        connected.push_back(pair.first);
                    }
                }

                return connected;
            }

            DasResult MainProcessServer::GetSessionInfo(
                uint16_t         session_id,
                HostSessionInfo& out_info) const
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);

                auto it = sessions_.find(session_id);
                if (it == sessions_.end())
                {
                    return DAS_E_IPC_OBJECT_NOT_FOUND;
                }

                out_info = it->second;
                return DAS_S_OK;
            }

            DasResult MainProcessServer::OnRemoteObjectRegistered(
                const ObjectId&    object_id,
                const DasGuid&     iid,
                uint16_t           session_id,
                const std::string& name,
                uint16_t           version)
            {
                if (!is_initialized_.load())
                {
                    return DAS_E_IPC_INVALID_STATE;
                }

                if (!IsSessionConnected(session_id))
                {
                    return DAS_E_IPC_CONNECTION_LOST;
                }

                auto&     registry = RemoteObjectRegistry::GetInstance();
                DasResult result = registry.RegisterObject(
                    object_id,
                    iid,
                    session_id,
                    name,
                    version);

                if (DAS::IsFailed(result))
                {
                    return result;
                }

                {
                    std::lock_guard<std::mutex> lock(sessions_mutex_);
                    auto                        it = sessions_.find(session_id);
                    if (it != sessions_.end())
                    {
                        it->second.last_active_ms = GetCurrentTimeMs();
                    }
                }

                if (on_object_registered_)
                {
                    RemoteObjectInfo info{};
                    info.iid = iid;
                    info.object_id = object_id;
                    info.session_id = session_id;
                    info.name = name;
                    info.version = version;
                    on_object_registered_(info);
                }

                return DAS_S_OK;
            }

            DasResult MainProcessServer::OnRemoteObjectUnregistered(
                const ObjectId& object_id)
            {
                if (!is_initialized_.load())
                {
                    return DAS_E_IPC_INVALID_STATE;
                }

                RemoteObjectInfo info{};
                DasResult        result =
                    RemoteObjectRegistry::GetInstance().GetObjectInfo(
                        object_id,
                        info);

                if (DAS::IsFailed(result))
                {
                    return result;
                }

                result = RemoteObjectRegistry::GetInstance().UnregisterObject(
                    object_id);

                if (DAS::IsFailed(result))
                {
                    return result;
                }

                if (on_object_unregistered_)
                {
                    on_object_unregistered_(info);
                }

                return DAS_S_OK;
            }

            DasResult MainProcessServer::GetRemoteObjects(
                std::vector<RemoteObjectInfo>& out_objects) const
            {
                RemoteObjectRegistry::GetInstance().ListAllObjects(out_objects);
                return DAS_S_OK;
            }

            DasResult MainProcessServer::GetRemoteObjectInfo(
                const ObjectId&   object_id,
                RemoteObjectInfo& out_info) const
            {
                return RemoteObjectRegistry::GetInstance().GetObjectInfo(
                    object_id,
                    out_info);
            }

            DasResult MainProcessServer::LookupRemoteObjectByName(
                const std::string& name,
                RemoteObjectInfo&  out_info) const
            {
                return RemoteObjectRegistry::GetInstance().LookupByName(
                    name,
                    out_info);
            }

            DasResult MainProcessServer::LookupRemoteObjectByInterface(
                const DasGuid&    iid,
                RemoteObjectInfo& out_info) const
            {
                uint32_t interface_id =
                    RemoteObjectRegistry::ComputeInterfaceId(iid);
                return RemoteObjectRegistry::GetInstance().LookupByInterface(
                    interface_id,
                    out_info);
            }

            DasResult MainProcessServer::SendLoadPlugin(
                const std::string& plugin_path,
                RemoteObjectInfo&  out_object_info)
            {
                if (!is_initialized_.load() || !is_running_.load())
                {
                    return DAS_E_IPC_INVALID_STATE;
                }

                // 检查是否有连接的 Host 进程
                auto connected_sessions = GetConnectedSessions();
                if (connected_sessions.empty())
                {
                    return DAS_E_IPC_NO_CONNECTIONS;
                }

                // TODO: 实现完整的 IPC 传输逻辑
                // 当前需要:
                // 1. IpcTransport 实例
                // 2. 序列化/反序列化工具函数
                // 3. stdexec 异步支持
                (void)plugin_path;
                (void)out_object_info;
                return DAS_E_NOT_IMPLEMENTED;
            }

            auto MainProcessServer::SendLoadPluginAsync(
                const std::string& plugin_path)
            {
                // TODO: 实现基于 stdexec 的异步加载
                // 当前返回未实现错误
                RemoteObjectInfo info{};
                info.name = plugin_path;
                return std::make_tuple(ObjectId{}, info);
            }

            DasResult MainProcessServer::DispatchMessage(
                const IPCMessageHeader& header,
                const uint8_t*          body,
                size_t                  body_size,
                std::vector<uint8_t>&   response_body)
            {
                if (!is_initialized_.load() || !is_running_.load())
                {
                    return DAS_E_IPC_INVALID_STATE;
                }

                DasResult result = ValidateTargetObject(header);
                if (DAS::IsFailed(result))
                {
                    return result;
                }

                // 检查是否需要转发到其他 Host 进程
                if (ShouldForwardMessage(header))
                {
                    return ForwardMessageToHost(
                        header,
                        body,
                        body_size,
                        response_body);
                }

                if (dispatch_handler_)
                {
                    return dispatch_handler_(
                        header,
                        body,
                        body_size,
                        response_body);
                }

                response_body.clear();
                return DAS_E_NOT_IMPLEMENTED;
            }

            void MainProcessServer::SetMessageDispatchHandler(
                MessageDispatchHandler handler)
            {
                dispatch_handler_ = std::move(handler);
            }

            void MainProcessServer::SetOnSessionConnectedCallback(
                SessionEventCallback callback)
            {
                on_session_connected_ = std::move(callback);
            }

            void MainProcessServer::SetOnSessionDisconnectedCallback(
                SessionEventCallback callback)
            {
                on_session_disconnected_ = std::move(callback);
            }

            void MainProcessServer::SetOnObjectRegisteredCallback(
                ObjectEventCallback callback)
            {
                on_object_registered_ = std::move(callback);
            }

            void MainProcessServer::SetOnObjectUnregisteredCallback(
                ObjectEventCallback callback)
            {
                on_object_unregistered_ = std::move(callback);
            }

            uint64_t MainProcessServer::GetCurrentTimeMs()
            {
                auto now = std::chrono::system_clock::now();
                auto duration = now.time_since_epoch();
                return static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        duration)
                        .count());
            }

            bool MainProcessServer::ValidateSessionId(uint16_t session_id) const
            {
                // session_id = 0 和 0xFFFF 是保留的
                return SessionCoordinator::IsValidSessionId(session_id);
            }

            DasResult MainProcessServer::ValidateTargetObject(
                const IPCMessageHeader& header) const
            {
                ObjectId obj_id = {
                    .session_id = header.session_id,
                    .generation = header.generation,
                    .local_id = header.local_id};

                if (IsNullObjectId(obj_id))
                {
                    return DAS_E_IPC_INVALID_OBJECT_ID;
                }

                if (!RemoteObjectRegistry::GetInstance().ObjectExists(obj_id))
                {
                    return DAS_E_IPC_OBJECT_NOT_FOUND;
                }

                if (!IsSessionConnected(obj_id.session_id))
                {
                    return DAS_E_IPC_CONNECTION_LOST;
                }

                return DAS_S_OK;
            }
            bool MainProcessServer::ShouldForwardMessage(
                const IPCMessageHeader& header) const
            {
                // 主进程的 session_id 是 1
                // 如果目标对象的 session_id 不是本地（主进程），则需要转发
                uint16_t local_session_id =
                    SessionCoordinator::GetInstance().GetLocalSessionId();
                return header.session_id != local_session_id;
            }

            DasResult MainProcessServer::ForwardMessageToHost(
                const IPCMessageHeader& header,
                const uint8_t*          body,
                size_t                  body_size,
                std::vector<uint8_t>&   response_body)
            {
                // 通过 ConnectionManager 获取传输层
                IpcTransport* transport =
                    ::Das::Core::IPC::ConnectionManager::GetInstance()
                        .GetTransport(header.session_id);
                if (!transport)
                {
                    std::string msg = DAS_FMT_NS::format(
                        "Failed to get transport for session_id: {}",
                        header.session_id);
                    DAS_LOG_ERROR(msg.c_str());
                    return DAS_E_IPC_NO_CONNECTIONS;
                }

                // 发送消息到目标 Host
                DasResult result = transport->Send(header, body, body_size);
                if (DAS::IsFailed(result))
                {
                    std::string msg = DAS_FMT_NS::format(
                        "Failed to forward message to session_id: {}, error: {}",
                        header.session_id,
                        static_cast<int32_t>(result));
                    DAS_LOG_ERROR(msg.c_str());
                    return result;
                }

                // TODO: 等待响应 - 当前简化实现，只发送不接收响应
                // 完整实现需要：
                // 1. 使用 IpcRunLoop 的 pending_calls_ 等待响应
                // 2. 设置超时机制
                // 3. 解析响应并填充 response_body
                response_body.clear();
                return DAS_S_OK;
            }

            void MainProcessServer::ListenLoop()
            {
                std::string msg = DAS_FMT_NS::format(
                    "ListenLoop started, main_pid={}",
                    main_pid_);
                DAS_LOG_INFO(msg.c_str());

                while (is_listening_.load())
                {
                    if (!listen_transport_)
                    {
                        break;
                    }

                    // 接收消息
                    IPCMessageHeader     header{};
                    std::vector<uint8_t> body;
                    DasResult result = listen_transport_->Receive(
                        header,
                        body,
                        1000);  // 1 second timeout

                    if (DAS::IsFailed(result))
                    {
                        // 超时或错误，继续循环
                        if (result != DAS_E_IPC_TIMEOUT)
                        {
                            std::string err_msg = DAS_FMT_NS::format(
                                "ListenLoop receive error: {}",
                                static_cast<int32_t>(result));
                            DAS_LOG_ERROR(err_msg.c_str());
                        }
                        continue;
                    }

                    // 处理握手消息
                    if (header.interface_id ==
                        static_cast<uint32_t>(
                            HandshakeInterfaceId::HANDSHAKE_IFACE_HELLO))
                    {
                        if (body.size() >= sizeof(HelloRequestV1))
                        {
                            HelloRequestV1 hello_req{};
                            std::memcpy(
                                &hello_req,
                                body.data(),
                                sizeof(HelloRequestV1));

                            WelcomeResponseV1 welcome_resp{};
                            DasResult         handle_result =
                                HandleHostHello(hello_req, welcome_resp);

                            if (DAS::IsFailed(handle_result))
                            {
                                std::string err_msg = DAS_FMT_NS::format(
                                    "HandleHostHello failed: {}",
                                    static_cast<int32_t>(handle_result));
                                DAS_LOG_ERROR(err_msg.c_str());
                            }

                            // 发送 WELCOME 响应
                            std::vector<uint8_t> resp_body(
                                sizeof(WelcomeResponseV1));
                            std::memcpy(
                                resp_body.data(),
                                &welcome_resp,
                                sizeof(WelcomeResponseV1));

                            IPCMessageHeader resp_header{};
                            resp_header.magic = IPCMessageHeader::MAGIC;
                            resp_header.version = IPCMessageHeader::CURRENT_VERSION;
                            resp_header.message_type = static_cast<uint8_t>(MessageType::RESPONSE);
                            resp_header.header_flags = 0;
                            resp_header.call_id = header.call_id;
                            resp_header.interface_id = static_cast<uint32_t>(
                                HandshakeInterfaceId::HANDSHAKE_IFACE_WELCOME);
                            resp_header.method_id = 0;
                            resp_header.flags = 0;
                            resp_header.error_code = 0;
                            resp_header.body_size = static_cast<uint32_t>(resp_body.size());
                            // 控制平面消息: ObjectId = {0, 0, 0}
                            resp_header.session_id = 0;
                            resp_header.generation = 0;
                            resp_header.local_id = 0;
                            listen_transport_->Send(
                                resp_header,
                                resp_body.data(),
                                resp_body.size());
                        }
                    }
                    else if (
                        header.interface_id ==
                        static_cast<uint32_t>(
                            HandshakeInterfaceId::HANDSHAKE_IFACE_READY))
                    {
                        if (body.size() >= sizeof(ReadyRequestV1))
                        {
                            ReadyRequestV1 ready_req{};
                            std::memcpy(
                                &ready_req,
                                body.data(),
                                sizeof(ReadyRequestV1));

                            ReadyAckV1 ready_ack{};
                            DasResult  handle_result =
                                HandleHostReady(ready_req, ready_ack);

                            if (DAS::IsFailed(handle_result))
                            {
                                std::string err_msg = DAS_FMT_NS::format(
                                    "HandleHostReady failed: {}",
                                    static_cast<int32_t>(handle_result));
                                DAS_LOG_ERROR(err_msg.c_str());
                            }

                            // 发送 READYACK 响应
                            std::vector<uint8_t> resp_body(sizeof(ReadyAckV1));
                            std::memcpy(
                                resp_body.data(),
                                &ready_ack,
                                sizeof(ReadyAckV1));

                            IPCMessageHeader resp_header{};
                            resp_header.magic = IPCMessageHeader::MAGIC;
                            resp_header.version = IPCMessageHeader::CURRENT_VERSION;
                            resp_header.message_type = static_cast<uint8_t>(MessageType::RESPONSE);
                            resp_header.header_flags = 0;
                            resp_header.call_id = header.call_id;
                            resp_header.interface_id = static_cast<uint32_t>(
                                HandshakeInterfaceId::HANDSHAKE_IFACE_READY_ACK);
                            resp_header.method_id = 0;
                            resp_header.flags = 0;
                            resp_header.error_code = 0;
                            resp_header.body_size = static_cast<uint32_t>(resp_body.size());
                            // 控制平面消息: ObjectId = {0, 0, 0}
                            resp_header.session_id = 0;
                            resp_header.generation = 0;
                            resp_header.local_id = 0;
                            listen_transport_->Send(
                                resp_header,
                                resp_body.data(),
                                resp_body.size());
                        }
                    }
                }

                DAS_LOG_INFO("ListenLoop ended");
            }

            DasResult MainProcessServer::HandleHostHello(
                const HelloRequestV1&  req,
                WelcomeResponseV1&     resp)
            {
                // 检查协议版本
                if (req.protocol_version != HelloRequestV1::CURRENT_PROTOCOL_VERSION)
                {
                    InitWelcomeResponse(
                        resp,
                        0,
                        WelcomeResponseV1::STATUS_VERSION_MISMATCH);
                    std::string msg = DAS_FMT_NS::format(
                        "HandleHostHello: version mismatch, expected={}, got={}",
                        HelloRequestV1::CURRENT_PROTOCOL_VERSION,
                        req.protocol_version);
                    DAS_LOG_ERROR(msg.c_str());
                    return DAS_E_IPC_VERSION_MISMATCH;
                }

                // 分配 session_id
                uint16_t session_id =
                    SessionCoordinator::GetInstance().AllocateSessionId();
                if (session_id == 0)
                {
                    InitWelcomeResponse(
                        resp,
                        0,
                        WelcomeResponseV1::STATUS_TOO_MANY_CLIENTS);
                    DAS_LOG_ERROR("HandleHostHello: failed to allocate session_id");
                    return DAS_E_IPC_TOO_MANY_CLIENTS;
                }

                // 记录 host_pid 到 session_id 的映射
                {
                    std::lock_guard<std::mutex> lock(host_pid_mutex_);
                    host_pid_to_session_[req.pid] = session_id;
                }

                // 初始化响应
                InitWelcomeResponse(resp, session_id, WelcomeResponseV1::STATUS_SUCCESS);

                {
                    std::string msg = DAS_FMT_NS::format(
                        "HandleHostHello: allocated session_id={} for host_pid={}, plugin={}",
                        session_id,
                        req.pid,
                        req.plugin_name);
                    DAS_LOG_INFO(msg.c_str());
                }

                return DAS_S_OK;
            }

            DasResult MainProcessServer::HandleHostReady(
                const ReadyRequestV1& req,
                ReadyAckV1&           ack)
            {
                // 验证 session_id
                if (!ValidateSessionId(req.session_id))
                {
                    InitReadyAck(ack, ReadyAckV1::STATUS_INVALID_SESSION);
                    std::string msg = DAS_FMT_NS::format(
                        "HandleHostReady: invalid session_id={}",
                        req.session_id);
                    DAS_LOG_ERROR(msg.c_str());
                    return DAS_E_INVALID_ARGUMENT;
                }

                // 检查 session_id 是否已分配
                if (!SessionCoordinator::GetInstance().IsSessionIdAllocated(
                        req.session_id))
                {
                    InitReadyAck(ack, ReadyAckV1::STATUS_SESSION_NOT_READY);
                    std::string msg = DAS_FMT_NS::format(
                        "HandleHostReady: session_id={} not allocated",
                        req.session_id);
                    DAS_LOG_ERROR(msg.c_str());
                    return DAS_E_IPC_INVALID_STATE;
                }

                // 注册 Host 连接
                DasResult result = OnHostConnected(req.session_id);
                if (DAS::IsFailed(result))
                {
                    InitReadyAck(ack, ReadyAckV1::STATUS_SESSION_NOT_READY);
                    std::string msg = DAS_FMT_NS::format(
                        "HandleHostReady: OnHostConnected failed for session_id={}, error={}",
                        req.session_id,
                        static_cast<int32_t>(result));
                    DAS_LOG_ERROR(msg.c_str());
                    return result;
                }

                // 初始化响应
                InitReadyAck(ack, ReadyAckV1::STATUS_SUCCESS);

                {
                    std::string msg = DAS_FMT_NS::format(
                        "HandleHostReady: host ready, session_id={}",
                        req.session_id);
                    DAS_LOG_INFO(msg.c_str());
                }

                return DAS_S_OK;
            }

        } // namespace MainProcess
    }
}
DAS_NS_END
