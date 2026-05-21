#include <das/Core/IPC/BusinessControlRequestRaw.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/Utils/StdExecution.h>
#include <stdexec/execution.hpp>
#include <tuple>

DAS_CORE_IPC_NS_BEGIN

DasResult SendBusinessControlRequestRaw(
    IpcRunLoop&                   run_loop,
    std::weak_ptr<BusinessThread> business_thread,
    uint16_t                      source_session_id,
    uint16_t                      target_session_id,
    IpcCommandType                command,
    const uint8_t*                body,
    size_t                        body_size,
    std::vector<uint8_t>&         out_response)
{
    auto bt = business_thread.lock();
    if (!bt)
    {
        DAS_CORE_LOG_ERROR(
            "SendBusinessControlRequestRaw: BusinessThread not available");
        return DAS_E_IPC_DISCONNECTED;
    }

    uint16_t call_id = run_loop.AllocateCallId();

    ValidatedIPCMessageHeader header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::REQUEST)
            .SetBusinessControlCommand(command)
            .SetBodySize(static_cast<uint32_t>(body_size))
            .SetCallId(call_id)
            .SetSourceSessionId(source_session_id)
            .SetTargetSessionId(target_session_id)
            .Build();

    // CallKey uses the REMOTE session_id as source_session_id
    CallKey call_key{target_session_id, call_id};

    if (bt->IsCurrentThread())
    {
        std::vector<uint8_t> body_vec(body, body + body_size);
        DasResult send_result = run_loop.PostSend(header, std::move(body_vec));
        if (send_result != DAS_S_OK)
        {
            DAS_CORE_LOG_ERROR(
                "SendBusinessControlRequestRaw: PostSend failed, result = {}",
                send_result);
            return send_result;
        }

        return bt->PumpUntilResponse(call_key, out_response);
    }
    else
    {
        // External thread: AwaitResponseSender 的 start() 会调用
        // PostSend(带回调)，在 IO 线程注册 pending call 后再发送
        constexpr auto kTimeout = std::chrono::milliseconds{30000};

        std::vector<uint8_t> body_vec(body, body + body_size);
        AwaitResponseSender sender{
            &run_loop, header, std::move(body_vec), call_key, kTimeout};
        auto result = stdexec::sync_wait(std::move(sender));
        if (!result)
        {
            DAS_CORE_LOG_ERROR(
                "SendBusinessControlRequestRaw: sync_wait failed for call_id "
                "= {}",
                call_id);
            return DAS_E_IPC_REMOTE_ERROR;
        }

        auto&                inner = std::get<0>(*result);
        DasResult            ipc_result = std::get<0>(inner);
        std::vector<uint8_t> response_body = std::move(std::get<1>(inner));
        out_response = std::move(response_body);
        return ipc_result;
    }
}

DAS_CORE_IPC_NS_END
