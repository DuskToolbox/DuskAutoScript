#include <das/Core/IPC/ConnectionManager.h>
#include <das/Core/IPC/HandshakeSerialization.h>

#include <chrono>
#include <cstring>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>

#include <filesystem>
#include <unordered_map>

DAS_CORE_IPC_NS_BEGIN

IpcCommandHandler::IpcCommandHandler() : session_id_(0) {}

void IpcCommandHandler::SetSessionId(uint16_t session_id)
{
    session_id_ = session_id;
}

uint16_t  IpcCommandHandler::GetSessionId() const { return session_id_; }
DasResult IpcCommandHandler::HandleMessage(
    const ValidatedIPCMessageHeader& header,
    const std::vector<uint8_t>&      body,
    IpcResponseSender&               sender,
    StubContext&                     ctx)
{
    auto& object_manager = ctx.object_manager;
    (void)object_manager; // 控制平面处理器不使用 object_manager
    IpcCommandResponse       response;
    std::span<const uint8_t> payload(body);

    // 处理 REMOTE_RELEASE EVENT（fire-and-forget）
    // EVENT 类型不需要响应，直接在 HandleMessage 中处理
    IpcCommandType cmd_type = ExtractCommandType(header);
    if (cmd_type == IpcCommandType::REMOTE_RELEASE
        && header.GetMessageType() == MessageType::EVENT)
    {
        // 解析 ObjectId 并调用 object_manager->Release
        if (payload.size() >= sizeof(ObjectId))
        {
            size_t   offset = 0;
            ObjectId object_id;
            if (DeserializeValue(payload, offset, object_id))
            {
                object_manager.UnregisterObject(object_id);
                std::string log_msg = DAS_FMT_NS::format(
                    "[IpcCommandHandler] REMOTE_RELEASE: released object_id "
                    "session={}, local={}",
                    object_id.session_id,
                    object_id.local_id);
                DAS_CORE_LOG_INFO(log_msg.c_str());
            }
        }
        // EVENT 不需要响应，直接返回
        return DAS_S_OK;
    }

    // 处理 RELEASE_SHM_BLOCK EVENT（fire-and-forget）
    if (cmd_type == IpcCommandType::RELEASE_SHM_BLOCK
        && header.GetMessageType() == MessageType::EVENT)
    {
        if (payload.size() >= sizeof(ReleaseShmBlockPayload))
        {
            ReleaseShmBlockPayload shm_payload;
            std::memcpy(&shm_payload, payload.data(), sizeof(shm_payload));

            // Access shm_pool through run_loop -> connection_manager
            auto& run_loop = ctx.run_loop;
            auto* conn_mgr = run_loop.GetConnectionManager();
            if (conn_mgr)
            {
                ConnectionInfo conn_info;
                if (DAS::IsOk(conn_mgr->GetConnection(
                        shm_payload.source_session_id,
                        conn_info))
                    && conn_info.shm_pool != nullptr)
                {
                    conn_info.shm_pool->Deallocate(shm_payload.shm_handle);
                    DAS_CORE_LOG_INFO(
                        "[IpcCommandHandler] RELEASE_SHM_BLOCK: handle={}, "
                        "session={}",
                        shm_payload.shm_handle,
                        shm_payload.source_session_id);
                }
            }
        }
        return DAS_S_OK;
    }

    DasResult result = HandleCommand(header, payload, response);

    // 构建响应 header（保留请求的 header_flags，确保响应路由正确）
    auto validated_response_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::RESPONSE)
            .SetHeaderFlags(header.GetHeaderFlags())
            .SetInterfaceId(header.GetInterfaceId())
            .SetBodySize(static_cast<uint32_t>(response.response_data.size()))
            .SetCallId(header.GetCallId())
            .SetSourceSessionId(session_id_)
            .SetTargetSessionId(header.GetSourceSessionId())
            .SetErrorCode(static_cast<int32_t>(response.error_code))
            .Build();
    sender.SendResponse(validated_response_header, response.response_data);
    return result;
}

IpcCommandType IpcCommandHandler::ExtractCommandType(
    const ValidatedIPCMessageHeader& header)
{
    return static_cast<IpcCommandType>(header.GetInterfaceId());
}

DasResult IpcCommandHandler::HandleCommand(
    const ValidatedIPCMessageHeader& header,
    std::span<const uint8_t>         payload,
    IpcCommandResponse&              response)
{
    IpcCommandType cmd_type = ExtractCommandType(header);

    std::string _find_log = DAS_FMT_NS::format(
        "[IpcCommandHandler] HandleCommand: cmd_type={}, custom_handlers_.size()={}",
        static_cast<int>(cmd_type),
        custom_handlers_.size());
    DAS_CORE_LOG_INFO(_find_log.c_str());
    // 检查是否有自定义处理器
    auto it = custom_handlers_.find(cmd_type);
    if (it != custom_handlers_.end())
    {
        std::string _found_log = DAS_FMT_NS::format(
            "[IpcCommandHandler] Found custom handler for cmd_type={}",
            static_cast<int>(cmd_type));
        DAS_CORE_LOG_INFO(_found_log.c_str());
        return it->second(header, payload, response);
    }
    else
    {
        std::string _not_found_log = DAS_FMT_NS::format(
            "[IpcCommandHandler] No custom handler for cmd_type={}",
            static_cast<int>(cmd_type));
        DAS_CORE_LOG_INFO(_not_found_log.c_str());
    }

    switch (cmd_type)
    {
    case IpcCommandType::LOOKUP_OBJECT:
        return OnLookupObject(header, payload, response);

    case IpcCommandType::LOOKUP_BY_NAME:
        return OnLookupByName(header, payload, response);

    case IpcCommandType::LOOKUP_BY_INTERFACE:
        return OnLookupByInterface(header, payload, response);

    case IpcCommandType::LIST_OBJECTS:
        return OnListObjects(header, payload, response);

    case IpcCommandType::LIST_SESSION_OBJECTS:
        return OnListSessionObjects(header, payload, response);

    case IpcCommandType::CLEAR_SESSION:
        return OnClearSession(header, payload, response);

    case IpcCommandType::PING:
        return OnPing(header, payload, response);

    case IpcCommandType::GET_OBJECT_COUNT:
        return OnGetObjectCount(header, payload, response);

    case IpcCommandType::REMOTE_ADD_REF:
        return OnRemoteAddRef(header, payload, response);

    case IpcCommandType::REMOTE_RELEASE:
        return OnRemoteRelease(header, payload, response);

    case IpcCommandType::LOAD_PLUGIN:
        // LOAD_PLUGIN requires custom handler registration.
        // Must call RegisterHandler() before using this command.
        response.error_code = DAS_E_IPC_COMMAND_NOT_REGISTERED;
        return DAS_E_IPC_COMMAND_NOT_REGISTERED;

    default:
        response.error_code = DAS_E_IPC_INVALID_MESSAGE_TYPE;
        return DAS_E_IPC_INVALID_MESSAGE_TYPE;
    }
}

void IpcCommandHandler::RegisterHandler(
    IpcCommandType command_type,
    CommandHandler handler)
{
    std::string _log_msg = DAS_FMT_NS::format(
        "[IpcCommandHandler] RegisterHandler: command_type={}",
        static_cast<int>(command_type));
    DAS_CORE_LOG_INFO(_log_msg.c_str());
    custom_handlers_[command_type] = std::move(handler);
}

DasResult IpcCommandHandler::OnLookupObject(
    const ValidatedIPCMessageHeader& header,
    std::span<const uint8_t>         payload,
    IpcCommandResponse&              response)
{
    (void)header;

    if (payload.size() < sizeof(LookupObjectPayload))
    {
        response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
        return DAS_E_IPC_INVALID_MESSAGE_BODY;
    }

    size_t offset = 0;

    ObjectId object_id;
    if (!DeserializeValue(payload, offset, object_id))
    {
        response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
        return DAS_E_IPC_DESERIALIZATION_FAILED;
    }

    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    RemoteObjectInfo      info;
    DasResult             result = registry.GetObjectInfo(object_id, info);

    response.error_code = result;

    if (result == DAS_S_OK)
    {
        response.response_data.clear();
        SerializeValue(response.response_data, info.object_id);
        SerializeValue(response.response_data, info.iid);
        SerializeValue(response.response_data, info.session_id);
        SerializeValue(response.response_data, info.version);
        SerializeString(response.response_data, info.name);
    }
    else
    {
        response.response_data.clear();
    }

    return result;
}

DasResult IpcCommandHandler::OnLookupByName(
    const ValidatedIPCMessageHeader& header,
    std::span<const uint8_t>         payload,
    IpcCommandResponse&              response)
{
    (void)header;

    size_t offset = 0;

    std::string name;
    if (!DeserializeString(payload, offset, name))
    {
        response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
        return DAS_E_IPC_DESERIALIZATION_FAILED;
    }

    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    RemoteObjectInfo      info;
    DasResult             result = registry.LookupByName(name, info);

    response.error_code = result;

    if (result == DAS_S_OK)
    {
        response.response_data.clear();
        SerializeValue(response.response_data, info.object_id);
        SerializeValue(response.response_data, info.iid);
        SerializeValue(response.response_data, info.session_id);
        SerializeValue(response.response_data, info.version);
        SerializeString(response.response_data, info.name);
    }
    else
    {
        response.response_data.clear();
    }

    return result;
}

DasResult IpcCommandHandler::OnLookupByInterface(
    const ValidatedIPCMessageHeader& header,
    std::span<const uint8_t>         payload,
    IpcCommandResponse&              response)
{
    (void)header;

    if (payload.size() < sizeof(LookupByInterfacePayload))
    {
        response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
        return DAS_E_IPC_INVALID_MESSAGE_BODY;
    }

    size_t offset = 0;

    DasGuid iid;
    if (!DeserializeValue(payload, offset, iid))
    {
        response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
        return DAS_E_IPC_DESERIALIZATION_FAILED;
    }

    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    uint32_t interface_id = RemoteObjectRegistry::ComputeInterfaceId(iid);
    RemoteObjectInfo info;
    DasResult        result = registry.LookupByInterface(interface_id, info);

    response.error_code = result;

    if (result == DAS_S_OK)
    {
        response.response_data.clear();
        SerializeValue(response.response_data, info.object_id);
        SerializeValue(response.response_data, info.iid);
        SerializeValue(response.response_data, info.session_id);
        SerializeValue(response.response_data, info.version);
        SerializeString(response.response_data, info.name);
    }
    else
    {
        response.response_data.clear();
    }

    return result;
}

DasResult IpcCommandHandler::OnListObjects(
    const ValidatedIPCMessageHeader& header,
    std::span<const uint8_t>         payload,
    IpcCommandResponse&              response)
{
    (void)header;
    (void)payload;

    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    std::vector<RemoteObjectInfo> objects;
    registry.ListAllObjects(objects);
    DasResult result = DAS_S_OK;

    response.error_code = result;

    if (result == DAS_S_OK)
    {
        response.response_data.clear();
        uint32_t count = static_cast<uint32_t>(objects.size());
        SerializeValue(response.response_data, count);

        for (const auto& info : objects)
        {
            SerializeValue(response.response_data, info.object_id);
            SerializeValue(response.response_data, info.iid);
            SerializeValue(response.response_data, info.session_id);
            SerializeValue(response.response_data, info.version);
            SerializeString(response.response_data, info.name);
        }
    }
    else
    {
        response.response_data.clear();
    }

    return result;
}

DasResult IpcCommandHandler::OnListSessionObjects(
    const ValidatedIPCMessageHeader& header,
    std::span<const uint8_t>         payload,
    IpcCommandResponse&              response)
{
    (void)header;

    if (payload.size() < sizeof(ListSessionObjectsPayload))
    {
        response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
        return DAS_E_IPC_INVALID_MESSAGE_BODY;
    }

    size_t offset = 0;

    uint16_t session_id = 0;
    if (!DeserializeValue(payload, offset, session_id))
    {
        response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
        return DAS_E_IPC_DESERIALIZATION_FAILED;
    }

    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    std::vector<RemoteObjectInfo> objects;
    registry.ListObjectsBySession(session_id, objects);
    DasResult result = DAS_S_OK;

    response.error_code = result;

    if (result == DAS_S_OK)
    {
        response.response_data.clear();
        uint32_t count = static_cast<uint32_t>(objects.size());
        SerializeValue(response.response_data, count);

        for (const auto& info : objects)
        {
            SerializeValue(response.response_data, info.object_id);
            SerializeValue(response.response_data, info.iid);
            SerializeValue(response.response_data, info.session_id);
            SerializeValue(response.response_data, info.version);
            SerializeString(response.response_data, info.name);
        }
    }
    else
    {
        response.response_data.clear();
    }

    return result;
}

DasResult IpcCommandHandler::OnClearSession(
    const ValidatedIPCMessageHeader& header,
    std::span<const uint8_t>         payload,
    IpcCommandResponse&              response)
{
    (void)header;

    if (payload.size() < sizeof(ClearSessionPayload))
    {
        response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
        return DAS_E_IPC_INVALID_MESSAGE_BODY;
    }

    size_t offset = 0;

    uint16_t session_id = 0;
    if (!DeserializeValue(payload, offset, session_id))
    {
        response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
        return DAS_E_IPC_DESERIALIZATION_FAILED;
    }

    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    registry.UnregisterAllFromSession(session_id);
    DasResult result = DAS_S_OK;

    response.error_code = result;
    response.response_data.clear();

    return result;
}

DasResult IpcCommandHandler::OnPing(
    const ValidatedIPCMessageHeader& header,
    std::span<const uint8_t>         payload,
    IpcCommandResponse&              response)
{
    (void)header;
    (void)payload;

    auto timestamp = static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());

    response.error_code = DAS_S_OK;
    response.response_data.clear();
    SerializeValue(response.response_data, timestamp);

    return DAS_S_OK;
}

DasResult IpcCommandHandler::OnGetObjectCount(
    const ValidatedIPCMessageHeader& header,
    std::span<const uint8_t>         payload,
    IpcCommandResponse&              response)
{
    (void)header;
    (void)payload;

    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    uint64_t count = static_cast<uint64_t>(registry.GetObjectCount());

    response.error_code = DAS_S_OK;
    response.response_data.clear();
    SerializeValue(response.response_data, count);

    return DAS_S_OK;
}

DasResult IpcCommandHandler::OnLoadPlugin(
    const ValidatedIPCMessageHeader& header,
    std::span<const uint8_t>         payload,
    IpcCommandResponse&              response)
{
    (void)header;
    (void)payload;

    // This method should never be called directly.
    // LOAD_PLUGIN requires custom handler registration via RegisterHandler().
    // See das/Host/src/main.cpp for reference implementation.
    response.error_code = DAS_E_IPC_COMMAND_NOT_REGISTERED;
    return DAS_E_IPC_COMMAND_NOT_REGISTERED;
}

DasResult IpcCommandHandler::OnRemoteAddRef(
    const ValidatedIPCMessageHeader& header,
    std::span<const uint8_t>         payload,
    IpcCommandResponse&              response)
{
    (void)header;

    if (payload.size() < sizeof(RemoteAddRefPayload))
    {
        response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
        return DAS_E_IPC_INVALID_MESSAGE_BODY;
    }

    size_t offset = 0;

    ObjectId object_id;
    if (!DeserializeValue(payload, offset, object_id))
    {
        response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
        return DAS_E_IPC_DESERIALIZATION_FAILED;
    }

    // 获取 DistributedObjectManager 单例
    // 注意：这里需要通过某种方式获取 DistributedObjectManager
    // 在 Host 进程中，DistributedObjectManager 是单例
    // 由于 IpcCommandHandler 不知道当前进程是主进程还是 Host 进程，
    // 这里需要通过全局访问或者在初始化时设置回调

    // 暂时返回 NOT_IMPLEMENTED，实际实现需要在 Host 进程初始化时注册处理器
    response.error_code = DAS_E_IPC_COMMAND_NOT_REGISTERED;
    return DAS_E_IPC_COMMAND_NOT_REGISTERED;
}

DasResult IpcCommandHandler::OnRemoteRelease(
    const ValidatedIPCMessageHeader& header,
    std::span<const uint8_t>         payload,
    IpcCommandResponse&              response)
{
    (void)header;

    if (payload.size() < sizeof(RemoteReleasePayload))
    {
        response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
        return DAS_E_IPC_INVALID_MESSAGE_BODY;
    }

    size_t offset = 0;

    ObjectId object_id;
    if (!DeserializeValue(payload, offset, object_id))
    {
        response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
        return DAS_E_IPC_DESERIALIZATION_FAILED;
    }

    // REMOTE_RELEASE 是 fire-and-forget 的 EVENT，
    // 不需要发送响应（HandleMessage 中不会为 EVENT 调用 SendResponse）
    // 但为了完整性，我们还是返回成功
    response.error_code = DAS_S_OK;
    return DAS_S_OK;
}
DAS_CORE_IPC_NS_END
