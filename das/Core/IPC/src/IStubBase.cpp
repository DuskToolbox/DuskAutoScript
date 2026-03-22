#include <cstring>
#include <das/Core/IPC/IStubBase.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>

DAS_CORE_IPC_NS_BEGIN

// V3 Body Header 大小: interface_id(4B) + method_id(2B) + reserved(2B) +
// ObjectId(8B) = 16 bytes
constexpr size_t V3_BODY_HEADER_SIZE = 16;

bool IStubBase::ParseV3BodyHeader(
    const std::vector<uint8_t>& body,
    uint32_t&                   out_interface_id,
    uint16_t&                   out_method_id,
    ObjectId&                   out_object_id)
{
    if (body.size() < V3_BODY_HEADER_SIZE)
    {
        DAS_CORE_LOG_ERROR(
            "IStubBase::ParseV3BodyHeader: body too small, size={}, expected={}",
            body.size(),
            V3_BODY_HEADER_SIZE);
        return false;
    }

    // 解析 interface_id (4 bytes, little-endian)
    std::memcpy(&out_interface_id, body.data(), sizeof(out_interface_id));

    // 解析 method_id (2 bytes, little-endian)
    std::memcpy(&out_method_id, body.data() + 4, sizeof(out_method_id));

    // 解析 ObjectId (8 bytes: session_id 2B + generation 2B + local_id 4B)
    uint16_t session_id = 0;
    uint16_t generation = 0;
    uint32_t local_id = 0;
    std::memcpy(&session_id, body.data() + 8, sizeof(session_id));
    std::memcpy(&generation, body.data() + 10, sizeof(generation));
    std::memcpy(&local_id, body.data() + 12, sizeof(local_id));

    out_object_id = ObjectId{session_id, generation, local_id};

    return true;
}

DasResult IStubBase::HandleMessage(
    const ValidatedIPCMessageHeader& header,
    const std::vector<uint8_t>&      body,
    IpcResponseSender&               sender,
    StubContext&                     ctx)
{
    // 解析 V3 Body Header
    uint32_t interface_id = 0;
    uint16_t method_id = 0;
    ObjectId target_object;

    if (!ParseV3BodyHeader(body, interface_id, method_id, target_object))
    {
        // 构建错误响应
        auto error_header =
            IPCMessageHeaderBuilder()
                .SetMessageType(MessageType::RESPONSE)
                .SetBodySize(0)
                .SetCallId(header.GetCallId())
                .SetSourceSessionId(header.GetTargetSessionId())
                .SetTargetSessionId(header.GetSourceSessionId())
                .SetErrorCode(
                    static_cast<int32_t>(DAS_E_IPC_INVALID_MESSAGE_BODY))
                .Build();
        sender.SendResponse(error_header, {});
        return DAS_E_IPC_INVALID_MESSAGE_BODY;
    }

    // 通过 ObjectManager 查找 impl 指针（LookupObject 内部 AddRef）
    DAS::DasPtr<IDasBase> impl_holder;
    DasResult             lookup_result =
        ctx.object_manager.LookupObject(target_object, impl_holder.Put());

    if (DAS::IsFailed(lookup_result))
    {
        DAS_CORE_LOG_WARN(
            "IStubBase::HandleMessage: LookupObject failed for object_id "
            "(session_id={}, generation={}, local_id={}), result={}",
            target_object.session_id,
            target_object.generation,
            target_object.local_id,
            lookup_result);

        // 构建错误响应
        auto error_header =
            IPCMessageHeaderBuilder()
                .SetMessageType(MessageType::RESPONSE)
                .SetBodySize(0)
                .SetCallId(header.GetCallId())
                .SetSourceSessionId(header.GetTargetSessionId())
                .SetTargetSessionId(header.GetSourceSessionId())
                .SetErrorCode(static_cast<int32_t>(lookup_result))
                .Build();
        sender.SendResponse(error_header, {});
        return lookup_result;
    }

    // 调用 DispatchMethod
    const uint8_t* params = body.data() + V3_BODY_HEADER_SIZE;
    size_t         params_size = body.size() - V3_BODY_HEADER_SIZE;

    std::vector<uint8_t> response_body;
    DasResult            dispatch_result = DispatchMethod(
        method_id,
        static_cast<void*>(impl_holder.Get()),
        params,
        params_size,
        ctx,
        response_body);

    // 构建响应并发送
    auto response_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::RESPONSE)
            .SetFlags(ctx.response_flags)
            .SetBodySize(static_cast<uint32_t>(response_body.size()))
            .SetCallId(header.GetCallId())
            .SetSourceSessionId(header.GetTargetSessionId())
            .SetTargetSessionId(header.GetSourceSessionId())
            .SetErrorCode(static_cast<int32_t>(dispatch_result))
            .Build();

    sender.SendResponse(response_header, response_body);
    return dispatch_result;
}

DAS_CORE_IPC_NS_END
