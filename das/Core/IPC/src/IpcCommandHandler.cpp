#include "das/Core/IPC/IpcCommandHandler.h"
#include <chrono>
#include <cstring>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/ObjectManager.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>

#include <filesystem>
#include <unordered_map>

DAS_CORE_IPC_NS_BEGIN
namespace
{
    // 序列化辅助函数
    template <typename T>
    void SerializeValue(std::vector<uint8_t>& buffer, const T& value)
    {
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&value);
        buffer.insert(buffer.end(), ptr, ptr + sizeof(T));
    }

    template <typename T>
    bool
    DeserializeValue(std::span<const uint8_t> buffer, size_t& offset, T& value)
    {
        if (offset + sizeof(T) > buffer.size())
        {
            return false;
        }
        std::memcpy(&value, buffer.data() + offset, sizeof(T));
        offset += sizeof(T);
        return true;
    }

    void SerializeString(std::vector<uint8_t>& buffer, const std::string& str)
    {
        SerializeValue(buffer, static_cast<uint16_t>(str.size()));
        buffer.insert(
            buffer.end(),
            reinterpret_cast<const uint8_t*>(str.data()),
            reinterpret_cast<const uint8_t*>(str.data()) + str.size());
    }

    bool DeserializeString(
        std::span<const uint8_t> buffer,
        size_t&                  offset,
        std::string&             str,
        uint16_t                 max_len = 1024)
    {
        uint16_t len = 0;
        if (!DeserializeValue(buffer, offset, len))
        {
            return false;
        }
        if (len > max_len || offset + len > buffer.size())
        {
            return false;
        }
        str.assign(reinterpret_cast<const char*>(buffer.data() + offset), len);
        offset += len;
        return true;
    }

}

IpcCommandHandler::IpcCommandHandler() : session_id_(0) {}

void IpcCommandHandler::SetSessionId(uint16_t session_id)
{
    session_id_ = session_id;
}

uint16_t  IpcCommandHandler::GetSessionId() const { return session_id_; }
DasResult IpcCommandHandler::HandleMessage(
    const IPCMessageHeader&     header,
    const std::vector<uint8_t>& body,
    IpcResponseSender&          sender)
{
    IpcCommandResponse       response;
    std::span<const uint8_t> payload(body);

    DasResult result = HandleCommand(header, payload, response);

    // 构建响应 header
    IPCMessageHeader response_header = header;
    response_header.message_type = static_cast<uint8_t>(MessageType::RESPONSE);
    response_header.error_code = static_cast<int32_t>(response.error_code);
    response_header.body_size =
        static_cast<uint32_t>(response.response_data.size());

    sender.SendResponse(response_header, response.response_data);

    return result;
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

    case IpcCommandType::LOAD_PLUGIN:
        return OnLoadPlugin(header, payload, response);

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
    (void)response;

    // 1. 解析响应：encoded_object_id
    if (payload.size() < 8)
    {
        response.error_code = DAS_E_IPC_INVALID_MESSAGE_BODY;
        return DAS_E_IPC_INVALID_MESSAGE_BODY;
    }

    uint64_t encoded_id;
    std::memcpy(&encoded_id, payload.data(), 8);
    ObjectId object_id = DecodeObjectId(encoded_id);

    // 2. 注册到 RemoteObjectRegistry
    auto&     registry = RemoteObjectRegistry::GetInstance();
    DasResult result = registry.RegisterObject(
        object_id,
        DAS_IID_BASE,      // iid = IDasBase
        header.session_id, // 来自哪个 Host
        "loaded_plugin",   // name
        1                  // version
    );

    if (DAS::IsFailed(result))
    {
        response.error_code = result;
        return result;
    }

    // TODO: DistributedObjectManager 没有 GetInstance()
    // 单例方法，需要后续实现
    // // 3. 注册到 DistributedObjectManager（标记为远程对象）
    // result =
    // DistributedObjectManager::GetInstance().RegisterRemoteObject(object_id);
    // if (DAS::IsFailed(result))
    // {
    //     response.error_code = result;
    //     return result;
    // }

    // 4. 创建 Proxy<IDasBase>
    auto proxy = ProxyFactory::GetInstance().CreateProxy<IDasBase>(object_id);
    if (!proxy)
    {
        response.error_code = DAS_E_FAIL;
        return DAS_E_FAIL;
    }

    response.error_code = DAS_S_OK;
    return DAS_S_OK;
}
DAS_CORE_IPC_NS_END
