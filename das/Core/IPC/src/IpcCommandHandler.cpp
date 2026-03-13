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

uint16_t IpcCommandHandler::GetSessionId() const { return session_id_; }
boost::asio::awaitable<DasResult> IpcCommandHandler::HandleMessage(
    const IPCMessageHeader&     header,
    const std::vector<uint8_t>& body,
    IpcResponseSender&          sender)
{
    IpcCommandResponse       response;
    std::span<const uint8_t> payload(body);

    DasResult result = HandleCommand(header, payload, response);

    // 构建响应 header（使用 Builder）
    auto validated_response_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::RESPONSE)
            .SetControlPlaneCommand(
                static_cast<IpcCommandType>(header.interface_id))
            .SetBodySize(static_cast<uint32_t>(response.response_data.size()))
            .SetCallId(header.call_id)
            .SetErrorCode(static_cast<int32_t>(response.error_code))
            .Build();
    co_await sender.SendResponse(
        validated_response_header,
        response.response_data);
    co_return result;
}

IpcCommandType IpcCommandHandler::ExtractCommandType(
    const IPCMessageHeader& header)
{
    return static_cast<IpcCommandType>(header.interface_id);
}

DasResult IpcCommandHandler::HandleCommand(
    const IPCMessageHeader&  header,
    std::span<const uint8_t> payload,
    IpcCommandResponse&      response)
{
    IpcCommandType cmd_type = ExtractCommandType(header);

    std::string _find_log = DAS_FMT_NS::format(
        "[IpcCommandHandler] HandleCommand: cmd_type={}, custom_handlers_.size()={}",
        static_cast<int>(cmd_type),
        custom_handlers_.size());
    DAS_LOG_INFO(_find_log.c_str());
    // 检查是否有自定义处理器
    auto it = custom_handlers_.find(cmd_type);
    if (it != custom_handlers_.end())
    {
        std::string _found_log = DAS_FMT_NS::format(
            "[IpcCommandHandler] Found custom handler for cmd_type={}",
            static_cast<int>(cmd_type));
        DAS_LOG_INFO(_found_log.c_str());
        return it->second(header, payload, response);
    }
    else
    {
        std::string _not_found_log = DAS_FMT_NS::format(
            "[IpcCommandHandler] No custom handler for cmd_type={}",
            static_cast<int>(cmd_type));
        DAS_LOG_INFO(_not_found_log.c_str());
    }

    switch (cmd_type)
    {
    case IpcCommandType::REGISTER_OBJECT:
        return OnRegisterObject(header, payload, response);

    case IpcCommandType::UNREGISTER_OBJECT:
        return OnUnregisterObject(header, payload, response);

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
    DAS_LOG_INFO(_log_msg.c_str());
    custom_handlers_[command_type] = std::move(handler);
}

DasResult IpcCommandHandler::OnRegisterObject(
    const IPCMessageHeader&  header,
    std::span<const uint8_t> payload,
    IpcCommandResponse&      response)
{
    (void)header;

    if (payload.size() < sizeof(RegisterObjectPayload) - sizeof(uint16_t))
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

    DasGuid iid;
    if (!DeserializeValue(payload, offset, iid))
    {
        response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
        return DAS_E_IPC_DESERIALIZATION_FAILED;
    }

    uint16_t session_id = 0;
    if (!DeserializeValue(payload, offset, session_id))
    {
        response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
        return DAS_E_IPC_DESERIALIZATION_FAILED;
    }

    uint16_t version = 0;
    if (!DeserializeValue(payload, offset, version))
    {
        response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
        return DAS_E_IPC_DESERIALIZATION_FAILED;
    }

    std::string name;
    if (!DeserializeString(payload, offset, name))
    {
        response.error_code = DAS_E_IPC_DESERIALIZATION_FAILED;
        return DAS_E_IPC_DESERIALIZATION_FAILED;
    }

    RemoteObjectRegistry& registry = RemoteObjectRegistry::GetInstance();
    DasResult             result =
        registry.RegisterObject(object_id, iid, session_id, name, version);

    response.error_code = result;
    response.response_data.clear();

    return result;
}

DasResult IpcCommandHandler::OnUnregisterObject(
    const IPCMessageHeader&  header,
    std::span<const uint8_t> payload,
    IpcCommandResponse&      response)
{
    (void)header;

    if (payload.size() < sizeof(UnregisterObjectPayload))
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
    DasResult             result = registry.UnregisterObject(object_id);

    response.error_code = result;
    response.response_data.clear();

    return result;
}

DasResult IpcCommandHandler::OnLookupObject(
    const IPCMessageHeader&  header,
    std::span<const uint8_t> payload,
    IpcCommandResponse&      response)
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
    const IPCMessageHeader&  header,
    std::span<const uint8_t> payload,
    IpcCommandResponse&      response)
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
    const IPCMessageHeader&  header,
    std::span<const uint8_t> payload,
    IpcCommandResponse&      response)
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
    const IPCMessageHeader&  header,
    std::span<const uint8_t> payload,
    IpcCommandResponse&      response)
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
    const IPCMessageHeader&  header,
    std::span<const uint8_t> payload,
    IpcCommandResponse&      response)
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
    const IPCMessageHeader&  header,
    std::span<const uint8_t> payload,
    IpcCommandResponse&      response)
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
    const IPCMessageHeader&  header,
    std::span<const uint8_t> payload,
    IpcCommandResponse&      response)
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
    const IPCMessageHeader&  header,
    std::span<const uint8_t> payload,
    IpcCommandResponse&      response)
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
    const IPCMessageHeader&  header,
    std::span<const uint8_t> payload,
    IpcCommandResponse&      response)
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
    const IPCMessageHeader&  header,
    std::span<const uint8_t> payload,
    IpcCommandResponse&      response)
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

    // 获取 DistributedObjectManager 单例并调用 HandleRemoteAddRef
    // 注意：这里需要通过某种方式获取 DistributedObjectManager
    // 在 Host 进程中，DistributedObjectManager 是单例
    // 由于 IpcCommandHandler 不知道当前进程是主进程还是 Host 进程，
    // 这里需要通过全局访问或者在初始化时设置回调

    // 暂时返回 NOT_IMPLEMENTED，实际实现需要在 Host 进程初始化时注册处理器
    response.error_code = DAS_E_IPC_COMMAND_NOT_REGISTERED;
    return DAS_E_IPC_COMMAND_NOT_REGISTERED;
}

DasResult IpcCommandHandler::OnRemoteRelease(
    const IPCMessageHeader&  header,
    std::span<const uint8_t> payload,
    IpcCommandResponse&      response)
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

    // 与 OnRemoteAddRef 相同，需要通过全局访问获取 DistributedObjectManager
    response.error_code = DAS_E_IPC_COMMAND_NOT_REGISTERED;
    return DAS_E_IPC_COMMAND_NOT_REGISTERED;
}
DAS_CORE_IPC_NS_END
