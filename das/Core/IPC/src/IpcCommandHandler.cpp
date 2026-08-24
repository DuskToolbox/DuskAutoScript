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

IpcCommandHandler::IpcCommandHandler(
    RemoteObjectRegistry&     registry,
    DistributedObjectManager& object_manager)
    : session_id_(0), registry_(registry), object_manager_(object_manager)
{
}

void IpcCommandHandler::SetSessionId(uint16_t session_id)
{
    session_id_ = session_id;
}

uint16_t IpcCommandHandler::GetSessionId() const { return session_id_; }

DasResult IpcCommandHandler::HandleMessage(
    const ValidatedIPCMessageHeader& header,
    const std::vector<uint8_t>&      body,
    IpcResponseSender&               sender,
    StubContext&                     ctx)
{
    auto& object_manager = ctx.object_manager;
    (void)object_manager; // 控制平面处理器不使用 object_manager
    IpcCommandResponse             response;
    const std::span<const uint8_t> payload(body);

    // EVENT 类型不需要响应，直接在 HandleMessage 中处理
    const IpcCommandType cmd_type = ExtractCommandType(header);

    // 处理 RELEASE_SHM_BLOCK EVENT（fire-and-forget）
    if (cmd_type == IpcCommandType::ReleaseShmBlock
        && header.GetMessageType() == MessageType::Event)
    {
        if (payload.size() >= sizeof(ReleaseShmBlockPayload))
        {
            ReleaseShmBlockPayload shm_payload;
            std::memcpy(&shm_payload, payload.data(), sizeof(shm_payload));

            // Access shm_pool through run_loop -> connection_manager
            auto& run_loop = ctx.run_loop;
            if (run_loop.connection_manager_)
            {
                auto&          conn_mgr = run_loop.GetConnectionManager();
                ConnectionInfo conn_info;
                if (DAS::IsOk(conn_mgr.GetConnection(
                        shm_payload.source_session_id,
                        conn_info))
                    && conn_info.shm_pool != nullptr)
                {
                    DasResult dealloc_result =
                        conn_info.shm_pool->Deallocate(shm_payload.shm_handle);
                    if (DAS::IsFailed(dealloc_result))
                    {
                        DAS_CORE_LOG_WARN(
                            "[IpcCommandHandler] RELEASE_SHM_BLOCK: "
                            "Deallocate failed for handle={}, session={}, "
                            "result={}",
                            shm_payload.shm_handle,
                            shm_payload.source_session_id,
                            dealloc_result);
                    }
                    else
                    {
                        DAS_CORE_LOG_INFO(
                            "[IpcCommandHandler] RELEASE_SHM_BLOCK: handle={}, "
                            "session={}",
                            shm_payload.shm_handle,
                            shm_payload.source_session_id);
                    }
                }
            }
        }
        return DAS_S_OK;
    }

    const DasResult result = HandleCommand(header, payload, response);

    // 构建响应 header（保留请求的 header_flags，确保响应路由正确）
    auto validated_response_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::Response)
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
    const IpcCommandType cmd_type = ExtractCommandType(header);

    auto it = custom_handlers_.find(cmd_type);
    if (it != custom_handlers_.end())
    {
        return it->second(header, payload, response);
    }

    switch (cmd_type)
    {
    case IpcCommandType::LookupObject:
        return OnLookupObject(header, payload, response);

    case IpcCommandType::LookupByName:
        return OnLookupByName(header, payload, response);

    case IpcCommandType::LookupByInterface:
        return OnLookupByInterface(header, payload, response);

    case IpcCommandType::ListObjects:
        return OnListObjects(header, payload, response);

    case IpcCommandType::ListSessionObjects:
        return OnListSessionObjects(header, payload, response);

    case IpcCommandType::ClearSession:
        return OnClearSession(header, payload, response);

    case IpcCommandType::Ping:
        return OnPing(header, payload, response);

    case IpcCommandType::GetObjectCount:
        return OnGetObjectCount(header, payload, response);

    case IpcCommandType::LoadPlugin:
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
    const std::string log_msg = DAS_FMT_NS::format(
        "[IpcCommandHandler] RegisterHandler: command_type={}",
        static_cast<int>(command_type));
    DAS_CORE_LOG_INFO(log_msg.c_str());
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

    const RemoteObjectRegistry& registry = registry_;
    RemoteObjectInfo            info;
    const DasResult result = registry.GetObjectInfo(object_id, info);

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

    const RemoteObjectRegistry& registry = registry_;
    RemoteObjectInfo            info;
    const DasResult             result = registry.LookupByName(name, info);

    response.error_code = result;

    if (result == DAS_S_OK)
    {
        // 发出的句柄须配对一个 DOM ref，配平远端 proxy 销毁时的
        // REMOTE_RELEASE——否则服务注册 pin(ref=1) 被首个 proxy 释放吃掉 →
        // ObjectId 回收(gen+1) → 后续 resolve 拿陈旧句柄 STALE_OBJECT_HANDLE
        // （对齐 [out] 出参序列化时 RegisterLocalObject 的 +1）。对象已
        // 回收/未注册 → 不发陈旧句柄，直接失败。
        const DasResult ref_result = object_manager_.WithLocalObject(
            info.object_id,
            [](LocalObjectRef& ref)
            {
                ref.AddRef();
                return DAS_S_OK;
            });
        if (DAS::IsFailed(ref_result))
        {
            response.error_code = ref_result;
            response.response_data.clear();
            return response.error_code;
        }

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

    const RemoteObjectRegistry& registry = registry_;
    const uint32_t interface_id = RemoteObjectRegistry::ComputeInterfaceId(iid);
    RemoteObjectInfo info;
    const DasResult  result = registry.LookupByInterface(interface_id, info);

    response.error_code = result;

    if (result == DAS_S_OK)
    {
        // 同 OnLookupByName：发出句柄配一个 DOM ref，配平远端 REMOTE_RELEASE。
        const DasResult ref_result = object_manager_.WithLocalObject(
            info.object_id,
            [](LocalObjectRef& ref)
            {
                ref.AddRef();
                return DAS_S_OK;
            });
        if (DAS::IsFailed(ref_result))
        {
            response.error_code = ref_result;
            response.response_data.clear();
            return response.error_code;
        }

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

    std::vector<RemoteObjectInfo> objects;
    registry_.ListAllObjects(objects);
    const DasResult result = DAS_S_OK;

    response.error_code = result;

    if (result == DAS_S_OK)
    {
        response.response_data.clear();
        const uint32_t count = static_cast<uint32_t>(objects.size());
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

    std::vector<RemoteObjectInfo> objects;
    registry_.ListObjectsBySession(session_id, objects);
    const DasResult result = DAS_S_OK;

    response.error_code = result;

    if (result == DAS_S_OK)
    {
        response.response_data.clear();
        const uint32_t count = static_cast<uint32_t>(objects.size());
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

    RemoteObjectRegistry& registry = registry_;
    registry.UnregisterAllFromSession(session_id);
    const DasResult result = DAS_S_OK;

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

    const RemoteObjectRegistry& registry = registry_;
    const uint64_t count = static_cast<uint64_t>(registry.GetObjectCount());

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

DAS_CORE_IPC_NS_END
