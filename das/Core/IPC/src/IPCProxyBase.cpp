#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IPCProxyBase.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/Logger/Logger.h>
DAS_CORE_IPC_NS_BEGIN
DasResult IPCProxyBase::SendRequest(
    uint16_t              method_id,
    const uint8_t*        body,
    size_t                body_size,
    std::vector<uint8_t>& response_body)
{
    if (!run_loop_)
    {
        DAS_CORE_LOG_ERROR("run_loop_ is null");
        return DAS_E_FAIL;
    }

    // 分配调用 ID
    uint64_t call_id = AllocateCallId();

    // 构建消息头（使用 Builder）
    auto validated_header =
        BuildMessageHeader(method_id, call_id, MessageType::REQUEST, body_size);

    // 调用 IpcRunLoop 发送请求
    return run_loop_
        ->SendRequest(validated_header, body, body_size, response_body);
}

DasResult IPCProxyBase::SendRequestNoResponse(
    uint16_t       method_id,
    const uint8_t* body,
    size_t         body_size)
{
    if (!run_loop_)
    {
        DAS_CORE_LOG_ERROR("run_loop_ is null");
        return DAS_E_FAIL;
    }

    // 分配调用 ID
    uint64_t call_id = AllocateCallId();

    // 构建消息头（使用 Builder）
    auto validated_header =
        BuildMessageHeader(method_id, call_id, MessageType::EVENT, body_size);

    // 调用 IpcRunLoop 发送事件
    return run_loop_->SendEvent(validated_header, body, body_size);
}

DAS_CORE_IPC_NS_END
