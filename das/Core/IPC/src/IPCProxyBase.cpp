#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IPCProxyBase.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/Logger/Logger.h>
#include <stdexec/execution.hpp>

DAS_CORE_IPC_NS_BEGIN

// V3: IPCProxyBase 的 SendRequest 方法实现
// 通过 PostSend 发送请求，然后 PumpUntilResponse 等待响应

DasResult IPCProxyBase::SendRequest(
    uint16_t              method_id,
    const uint8_t*        body,
    size_t                body_size,
    std::vector<uint8_t>& out_response)
{
    (void)method_id; // method_id 用于日志记录，当前未使用

    // 1. Lock weak_ptr 检查 BusinessThread 是否可用
    auto bt = business_thread_.lock();
    if (!bt)
    {
        DAS_CORE_LOG_ERROR(
            "IPCProxyBase::SendRequest: BusinessThread not available");
        return DAS_E_IPC_DISCONNECTED;
    }

    // 2. 生成 call_id
    uint16_t call_id = NextCallId();

    // 3. 构建请求 header
    ValidatedIPCMessageHeader header =
        BuildRequestHeader(call_id, MessageType::REQUEST, body_size);

    // 4. 通过 PostSend 发送请求
    std::vector<uint8_t> body_vec(body, body + body_size);
    DasResult send_result = run_loop_.PostSend(header, std::move(body_vec));
    if (send_result != DAS_S_OK)
    {
        DAS_CORE_LOG_ERROR(
            "IPCProxyBase::SendRequest: PostSend failed, result={}",
            send_result);
        return send_result;
    }

    // 5. 等待响应
    CallKey call_key{GetSourceSessionId(), call_id};
    return bt->PumpUntilResponse(call_key, out_response);
}

DAS_CORE_IPC_NS_END
