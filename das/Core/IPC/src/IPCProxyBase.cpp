#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IPCProxyBase.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/Logger/Logger.h>
#include <stdexec/execution.hpp>

DAS_CORE_IPC_NS_BEGIN

DasResult IPCProxyBase::SendRequest(
    uint16_t              method_id,
    const uint8_t*        body,
    size_t                body_size,
    std::vector<uint8_t>& response_body)
{
    if (!run_loop_ || !transport_)
    {
        DAS_CORE_LOG_ERROR("run_loop_ or transport_ is null");
        return DAS_E_FAIL;
    }

    uint64_t call_id = AllocateCallId();

    auto validated_header =
        BuildMessageHeader(method_id, call_id, MessageType::REQUEST, body_size);

    // 使用 IpcRunLoop 的 SendMessageAsync 带 transport 版本
    auto sender = run_loop_->SendMessageAsync(
        transport_,
        validated_header,
        body,
        body_size,
        std::chrono::milliseconds{5000});

    auto result_opt = stdexec::sync_wait(std::move(sender));

    if (!result_opt.has_value())
    {
        DAS_CORE_LOG_ERROR("SendRequest timeout or cancelled");
        return DAS_E_IPC_TIMEOUT;
    }

    auto [result, response] = std::get<0>(*result_opt);

    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR("SendRequest failed with error = 0x{:08X}", result);
        return result;
    }

    response_body = std::move(response);
    return DAS_S_OK;
}

DasResult IPCProxyBase::SendRequestNoResponse(
    uint16_t       method_id,
    const uint8_t* body,
    size_t         body_size)
{
    if (!run_loop_ || !transport_)
    {
        DAS_CORE_LOG_ERROR("run_loop_ or transport_ is null");
        return DAS_E_FAIL;
    }

    uint64_t call_id = AllocateCallId();

    auto validated_header =
        BuildMessageHeader(method_id, call_id, MessageType::EVENT, body_size);

    // 使用 IpcRunLoop 的 SendMessageAsync 带 transport 版本发送事件
    // 注意：EVENT 类型的消息不需要等待响应
    // 这里使用同步发送，因为 IPCProxy 是同步 API
    auto sender = run_loop_->SendMessageAsync(
        transport_,
        validated_header,
        body,
        body_size,
        std::chrono::milliseconds{5000});

    // 对于 EVENT，我们等待发送完成但不关心响应
    auto result_opt = stdexec::sync_wait(std::move(sender));

    if (!result_opt.has_value())
    {
        // 超时或取消，但事件发送可能是 fire-and-forget
        DAS_CORE_LOG_WARN("SendRequestNoResponse timeout or cancelled");
        return DAS_S_OK;
    }

    auto [result, response] = std::get<0>(*result_opt);
    (void)response;

    if (DAS::IsFailed(result))
    {
        DAS_CORE_LOG_WARN("SendRequestNoResponse failed with error = 0x{:08X}", result);
    }

    return DAS_S_OK;
}

DAS_CORE_IPC_NS_END
