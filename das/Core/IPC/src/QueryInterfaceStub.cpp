#include <das/Core/IPC/QueryInterfaceStub.h>

#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/MethodMetadata.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/Utils/fmt.h>

#include <cstring>

DAS_CORE_IPC_NS_BEGIN

DasResult QueryInterfaceStub::HandleMessage(
    const ValidatedIPCMessageHeader& header,
    const std::vector<uint8_t>&      body,
    IpcResponseSender&               sender,
    StubContext&                     ctx)
{
    (void)header;

    // Body: ObjectId (8B) + DasGuid (16B) = 24B minimum
    constexpr size_t kMinBodySize = sizeof(ObjectId) + sizeof(DasGuid);
    if (body.size() < kMinBodySize)
    {
        DAS_CORE_LOG_ERROR(
            "[QUERY_INTERFACE] payload too small: {}",
            body.size());
        return DAS_E_IPC_INVALID_MESSAGE_BODY;
    }

    size_t offset = 0;

    // 1. Deserialize ObjectId
    ObjectId object_id;
    std::memcpy(&object_id, body.data() + offset, sizeof(object_id));
    offset += sizeof(object_id);

    // 2. Deserialize DasGuid
    DasGuid iid;
    std::memcpy(&iid, body.data() + offset, sizeof(iid));
    offset += sizeof(iid);

    // 3. Lookup the real object (LookupObject internally AddRef)
    DAS::DasPtr<IDasBase> raw_obj;
    DasResult             lookup_result =
        ctx.object_manager.LookupObject(object_id, raw_obj.Put());
    if (DAS::IsFailed(lookup_result))
    {
        DAS_CORE_LOG_ERROR(
            "[QUERY_INTERFACE] LookupObject failed: session = {}, local = {}, "
            "result = {}",
            object_id.session_id,
            object_id.local_id,
            lookup_result);

        // Send failure response
        auto response_header =
            IPCMessageHeaderBuilder()
                .SetMessageType(MessageType::RESPONSE)
                .SetBodySize(0)
                .SetCallId(header.GetCallId())
                .SetSourceSessionId(header.GetTargetSessionId())
                .SetTargetSessionId(header.GetSourceSessionId())
                .SetErrorCode(static_cast<int32_t>(lookup_result))
                .Build();
        sender.SendResponse(response_header, {});
        return lookup_result;
    }

    // 4. Call QueryInterface on the real object
    DAS::DasPtr<IDasBase> new_obj;
    DasResult qi_result = raw_obj->QueryInterface(iid, new_obj.PutVoid());
    if (DAS::IsFailed(qi_result))
    {
        DAS_CORE_LOG_INFO(
            "[QUERY_INTERFACE] QueryInterface returned: {}",
            qi_result);

        // Build failure response: int32(result) + uint32(0) + uint64(0)
        std::vector<uint8_t> response_body;
        int32_t              fail_result = static_cast<int32_t>(qi_result);
        response_body.insert(
            response_body.end(),
            reinterpret_cast<const uint8_t*>(&fail_result),
            reinterpret_cast<const uint8_t*>(&fail_result)
                + sizeof(fail_result));
        uint32_t zero32 = 0;
        response_body.insert(
            response_body.end(),
            reinterpret_cast<const uint8_t*>(&zero32),
            reinterpret_cast<const uint8_t*>(&zero32) + sizeof(zero32));
        uint64_t zero64 = 0;
        response_body.insert(
            response_body.end(),
            reinterpret_cast<const uint8_t*>(&zero64),
            reinterpret_cast<const uint8_t*>(&zero64) + sizeof(zero64));

        auto response_header =
            IPCMessageHeaderBuilder()
                .SetMessageType(MessageType::RESPONSE)
                .SetBodySize(static_cast<uint32_t>(response_body.size()))
                .SetCallId(header.GetCallId())
                .SetSourceSessionId(header.GetTargetSessionId())
                .SetTargetSessionId(header.GetSourceSessionId())
                .SetErrorCode(static_cast<int32_t>(qi_result))
                .Build();
        sender.SendResponse(response_header, std::move(response_body));
        return qi_result;
    }

    // 5. Register the new interface pointer as a local object
    ObjectId  new_obj_id;
    DasResult reg_result =
        ctx.object_manager.RegisterLocalObject(new_obj.Get(), new_obj_id);
    if (DAS::IsFailed(reg_result))
    {
        auto response_header =
            IPCMessageHeaderBuilder()
                .SetMessageType(MessageType::RESPONSE)
                .SetBodySize(0)
                .SetCallId(header.GetCallId())
                .SetSourceSessionId(header.GetTargetSessionId())
                .SetTargetSessionId(header.GetSourceSessionId())
                .SetErrorCode(static_cast<int32_t>(reg_result))
                .Build();
        sender.SendResponse(response_header, {});
        return reg_result;
    }

    // 6. Build success response: int32(result) + uint32(interface_id) +
    // uint64(encoded_object_id)
    uint32_t interface_id = ComputeInterfaceId(iid);
    uint64_t encoded_id = EncodeObjectId(new_obj_id);

    std::vector<uint8_t> response_body;
    int32_t              ok_result = static_cast<int32_t>(DAS_S_OK);
    response_body.insert(
        response_body.end(),
        reinterpret_cast<const uint8_t*>(&ok_result),
        reinterpret_cast<const uint8_t*>(&ok_result) + sizeof(ok_result));
    response_body.insert(
        response_body.end(),
        reinterpret_cast<const uint8_t*>(&interface_id),
        reinterpret_cast<const uint8_t*>(&interface_id) + sizeof(interface_id));
    response_body.insert(
        response_body.end(),
        reinterpret_cast<const uint8_t*>(&encoded_id),
        reinterpret_cast<const uint8_t*>(&encoded_id) + sizeof(encoded_id));

    auto response_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::RESPONSE)
            .SetBodySize(static_cast<uint32_t>(response_body.size()))
            .SetCallId(header.GetCallId())
            .SetSourceSessionId(header.GetTargetSessionId())
            .SetTargetSessionId(header.GetSourceSessionId())
            .SetErrorCode(0)
            .Build();
    sender.SendResponse(response_header, std::move(response_body));

    DAS_CORE_LOG_INFO(
        "[QUERY_INTERFACE] success: iid_hash = 0x{:08X}, "
        "new_obj_id = {{ session: {}, gen: {}, local: {} }}",
        interface_id,
        new_obj_id.session_id,
        new_obj_id.generation,
        new_obj_id.local_id);

    return DAS_S_OK;
}

DAS_CORE_IPC_NS_END
