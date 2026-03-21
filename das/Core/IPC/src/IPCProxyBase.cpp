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
    (void)method_id;

    // 1. Lock weak_ptr to check BusinessThread availability
    auto bt = business_thread_.lock();
    if (!bt)
    {
        DAS_CORE_LOG_ERROR(
            "IPCProxyBase::SendRequest: BusinessThread not available");
        return DAS_E_IPC_DISCONNECTED;
    }

    // 2. Generate call_id and build request header
    uint16_t call_id = NextCallId();

    ValidatedIPCMessageHeader header =
        BuildRequestHeader(call_id, MessageType::REQUEST, body_size);

    // 3. Send request via PostSend
    std::vector<uint8_t> body_vec(body, body + body_size);
    DasResult send_result = run_loop_.PostSend(header, std::move(body_vec));
    if (send_result != DAS_S_OK)
    {
        DAS_CORE_LOG_ERROR(
            "IPCProxyBase::SendRequest: PostSend failed, result={}",
            send_result);
        return send_result;
    }

    // 4. Wait for response using thread-appropriate strategy
    CallKey call_key{object_id_.session_id, call_id};

    if (bt->IsCurrentThread())
    {
        // Nested pump: called from within BusinessThread
        // Run() is paused, we pump inbound_queue_ directly
        return bt->PumpUntilResponse(call_key, out_response);
    }
    else
    {
        // External thread: register pending_call (on_complete=nullptr),
        // create AwaitResponseSender, sync_wait blocks until
        // CompletePendingCall fires the callback
        constexpr auto kTimeout = std::chrono::milliseconds{30000};
        run_loop_.RegisterPendingCall(call_key);

        AwaitResponseSender sender{&run_loop_, call_key, kTimeout};
        auto                result = stdexec::sync_wait(std::move(sender));
        if (!result)
        {
            DAS_CORE_LOG_ERROR(
                "IPCProxyBase::SendRequest: sync_wait failed for "
                "call_id={}",
                call_id);
            return DAS_E_IPC_REMOTE_ERROR;
        }

        // sync_wait returns optional<tuple<pair<DasResult, vector>>>
        auto&                inner = std::get<0>(*result);
        DasResult            ipc_result = inner.first;
        std::vector<uint8_t> response_body = std::move(inner.second);
        out_response = std::move(response_body);
        return ipc_result;
    }
}

DasResult IPCProxyBase::SendBusinessControlRequest(
    IpcCommandType        command,
    const uint8_t*        body,
    size_t                body_size,
    std::vector<uint8_t>& out_response)
{
    auto bt = business_thread_.lock();
    if (!bt)
    {
        DAS_CORE_LOG_ERROR(
            "IPCProxyBase::SendBusinessControlRequest: BusinessThread not "
            "available");
        return DAS_E_IPC_DISCONNECTED;
    }

    uint16_t call_id = NextCallId();

    ValidatedIPCMessageHeader header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::REQUEST)
            .SetBusinessControlCommand(command)
            .SetBodySize(static_cast<uint32_t>(body_size))
            .SetCallId(call_id)
            .SetSourceSessionId(GetSourceSessionId())
            .SetTargetSessionId(object_id_.session_id)
            .Build();

    std::vector<uint8_t> body_vec(body, body + body_size);
    DasResult send_result = run_loop_.PostSend(header, std::move(body_vec));
    if (send_result != DAS_S_OK)
    {
        DAS_CORE_LOG_ERROR(
            "IPCProxyBase::SendBusinessControlRequest: PostSend failed, "
            "result={}",
            send_result);
        return send_result;
    }

    CallKey call_key{object_id_.session_id, call_id};

    if (bt->IsCurrentThread())
    {
        return bt->PumpUntilResponse(call_key, out_response);
    }
    else
    {
        constexpr auto kTimeout = std::chrono::milliseconds{30000};
        run_loop_.RegisterPendingCall(call_key);

        AwaitResponseSender sender{&run_loop_, call_key, kTimeout};
        auto                result = stdexec::sync_wait(std::move(sender));
        if (!result)
        {
            DAS_CORE_LOG_ERROR(
                "IPCProxyBase::SendBusinessControlRequest: sync_wait failed "
                "for call_id={}",
                call_id);
            return DAS_E_IPC_REMOTE_ERROR;
        }

        auto&                inner = std::get<0>(*result);
        DasResult            ipc_result = inner.first;
        std::vector<uint8_t> response_body = std::move(inner.second);
        out_response = std::move(response_body);
        return ipc_result;
    }
}

DAS_CORE_IPC_NS_END
