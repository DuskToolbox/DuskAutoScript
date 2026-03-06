// 在包含头文件之前取消 Windows 的 DispatchMessage 宏，// 因为 Windows API
// 定义了 DispatchMessage 宏，与类方法名冲突
#ifdef _WIN32
#ifdef DispatchMessage
#undef DispatchMessage
#endif
#endif

#include "das/Core/IPC/MainProcess/MainProcessServer.h"

#include <das/Core/IPC/AsyncOperationImpl.h>
#include <das/Core/IPC/DasAsyncSender.h>

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
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/SessionCoordinator.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
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

                // 主进程 session_id = 1（使用专用方法）
                SessionCoordinator::GetInstance().SetAsMainProcess();

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

                {
                    std::lock_guard<std::mutex> lock(sessions_mutex_);
                    sessions_.clear();
                }
                RemoteObjectRegistry::GetInstance().Clear();

                is_initialized_.store(false);
                return DAS_S_OK;
            }

            DasResult MainProcessServer::Start()
            {
                if (!is_initialized_.load())
                {
                    DAS_CORE_LOG_ERROR("Server not initialized");
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

            uint32_t MainProcessServer::GetMainPid() const { return main_pid_; }

            DasResult MainProcessServer::OnHostConnected(uint16_t session_id)
            {
                if (!is_initialized_.load())
                {
                    DAS_CORE_LOG_ERROR("Server not initialized");
                    return DAS_E_IPC_INVALID_STATE;
                }

                if (!ValidateSessionId(session_id))
                {
                    DAS_CORE_LOG_ERROR("Invalid session_id = {}", session_id);
                    return DAS_E_INVALID_ARGUMENT;
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
                        DAS_CORE_LOG_ERROR(
                            "Session already connected (session_id = {})",
                            session_id);
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
                    DAS_CORE_LOG_ERROR("Server not initialized");
                    return DAS_E_IPC_INVALID_STATE;
                }

                std::lock_guard<std::mutex> lock(sessions_mutex_);

                auto it = sessions_.find(session_id);
                if (it == sessions_.end())
                {
                    DAS_CORE_LOG_ERROR(
                        "Session not found (session_id = {})",
                        session_id);
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
                // 主进程 session 总是"连接"的（本地对象不需要远程连接）
                // 直接解引用，如果未初始化会崩溃
                uint16_t local_session_id =
                    *SessionCoordinator::GetInstance().GetLocalSessionId();
                if (session_id == local_session_id)
                {
                    return true;
                }

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
                    DAS_CORE_LOG_ERROR(
                        "Session not found (session_id = {})",
                        session_id);
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
                    DAS_CORE_LOG_ERROR("Server not initialized");
                    return DAS_E_IPC_INVALID_STATE;
                }

                if (!IsSessionConnected(session_id))
                {
                    DAS_CORE_LOG_ERROR(
                        "Session not connected (session_id = {})",
                        session_id);
                    return DAS_E_IPC_CONNECTION_LOST;
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
                    DAS_CORE_LOG_ERROR("Server not initialized");
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

            DasResult MainProcessServer::DispatchMessage(
                const ValidatedIPCMessageHeader& validated_header,
                const uint8_t*                   body,
                size_t                           body_size,
                std::vector<uint8_t>&            response_body)
            {
                const IPCMessageHeader& header = validated_header.Raw();

                if (!is_initialized_.load() || !is_running_.load())
                {
                    DAS_CORE_LOG_ERROR("Server not initialized or not running");
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
                    DAS_CORE_LOG_ERROR("Null object_id");
                    return DAS_E_IPC_INVALID_OBJECT_ID;
                }

                if (!RemoteObjectRegistry::GetInstance().ObjectExists(obj_id))
                {
                    DAS_CORE_LOG_ERROR("Object not found");
                    return DAS_E_IPC_OBJECT_NOT_FOUND;
                }

                if (!IsSessionConnected(obj_id.session_id))
                {
                    DAS_CORE_LOG_ERROR(
                        "Session not connected (session_id = {})",
                        obj_id.session_id);
                    return DAS_E_IPC_CONNECTION_LOST;
                }

                return DAS_S_OK;
            }
            bool MainProcessServer::ShouldForwardMessage(
                const IPCMessageHeader& header) const
            {
                // 主进程的 session_id 是 1
                // 如果目标对象的 session_id 不是本地（主进程），则需要转发
                // 直接解引用，如果未初始化会崩溃
                uint16_t local_session_id =
                    *SessionCoordinator::GetInstance().GetLocalSessionId();
                return header.session_id != local_session_id;
            }

            DasResult MainProcessServer::ForwardMessageToHost(
                const IPCMessageHeader& header,
                const uint8_t*          body,
                size_t                  body_size,
                std::vector<uint8_t>&   response_body)
            {
                (void)header;
                (void)body;
                (void)body_size;
                (void)response_body;
                DAS_CORE_LOG_ERROR("ForwardMessageToHost not implemented");
                return DAS_E_NOT_IMPLEMENTED;
            }

            AwaitResponseSender MainProcessServer::ForwardMessageToHostAsync(
                const IPCMessageHeader& header,
                const uint8_t*          body,
                size_t                  body_size)
            {
                (void)header;
                (void)body;
                (void)body_size;
                DAS_CORE_LOG_ERROR("ForwardMessageToHostAsync not implemented");
                return AwaitResponseSender{
                    nullptr,
                    static_cast<uint64_t>(DAS_E_NOT_IMPLEMENTED),
                    std::chrono::milliseconds{0}};
            }

            DasResult MainProcessServer::SendLoadPlugin(
                const std::string&        plugin_path,
                ObjectId&                 out_object_id,
                uint16_t                  target_session_id,
                std::chrono::milliseconds timeout)
            {
                (void)plugin_path;
                (void)out_object_id;
                (void)target_session_id;
                (void)timeout;
                DAS_CORE_LOG_ERROR("SendLoadPlugin not implemented");
                return DAS_E_NOT_IMPLEMENTED;
            }

            DasResult MainProcessServer::SendLoadPluginAsync(
                const std::string&             plugin_path,
                uint16_t                       target_session_id,
                IDasAsyncLoadPluginOperation** pp_out_operation,
                std::chrono::milliseconds      timeout)
            {
                (void)plugin_path;
                (void)target_session_id;
                (void)pp_out_operation;
                (void)timeout;
                DAS_CORE_LOG_ERROR("SendLoadPluginAsync not implemented");
                return DAS_E_NOT_IMPLEMENTED;
            }

        } // namespace MainProcess
    }
}
DAS_NS_END
