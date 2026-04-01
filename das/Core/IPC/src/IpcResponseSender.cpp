#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/DefaultAsyncIpcTransport.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/IpcRunLoop.h>

#include <das/Core/Logger/Logger.h>
#include <das/Utils/fmt.h>

DAS_CORE_IPC_NS_BEGIN

IpcResponseSender::IpcResponseSender(DefaultAsyncIpcTransport& transport)
    : transport_(&transport)
{
}

IpcResponseSender::IpcResponseSender(IpcRunLoop& run_loop)
    : run_loop_(&run_loop)
{
}

IpcResponseSender::IpcResponseSender(
    DefaultAsyncIpcTransport& transport,
    IpcRunLoop&               run_loop)
    : transport_(&transport), run_loop_(&run_loop)
{
}

DasResult IpcResponseSender::SendResponse(
    const ValidatedIPCMessageHeader& validated_header,
    const std::vector<uint8_t>&      body)
{
    // Dual mode: both transport and run_loop available
    if (transport_ && run_loop_)
    {
        DAS_CORE_LOG_DEBUG(
                    "SendResponse: routing through send queue, body_size = {}",
                    body.size());
        std::vector<uint8_t> body_copy(body);
        return run_loop_->PostSendWithTransport(
            transport_,
            validated_header,
            std::move(body_copy));
    }

    // Business thread mode: route through PostSend (connection_manager lookup)
    if (run_loop_)
    {
        DAS_CORE_LOG_DEBUG(
                    "SendResponse: routing through PostSend, body_size = {}",
                    body.size());
        std::vector<uint8_t> body_copy(body);
        return run_loop_->PostSend(validated_header, std::move(body_copy));
    }

    DAS_CORE_LOG_ERROR("SendResponse: no valid run_loop");
    return DAS_E_IPC_NOT_INITIALIZED;
}

boost::asio::awaitable<DasResult> IpcResponseSender::SendResponseAsync(
    const ValidatedIPCMessageHeader& validated_header,
    const std::vector<uint8_t>&      body)
{
    // Dual mode: route through send queue to serialize async_write
    if (transport_ && run_loop_)
    {
        DAS_CORE_LOG_DEBUG(
                    "SendResponseAsync: routing through send queue, body_size = {}",
                    body.size());
        std::vector<uint8_t> body_copy(body);
        DasResult result = run_loop_->PostSendWithTransport(
            transport_,
            validated_header,
            std::move(body_copy));
        co_return result;
    }

    // IO thread mode: directly co_await transport send (legacy fallback)
    if (transport_)
    {
        DAS_CORE_LOG_DEBUG(
                    "SendResponseAsync: direct transport send, body_size = {}",
                    body.size());
        co_return co_await transport_->SendCoroutine(
            validated_header,
            body.data(),
            body.size());
    }

    DAS_CORE_LOG_ERROR(
        "SendResponseAsync: no valid transport");
    co_return DAS_E_IPC_NOT_INITIALIZED;
}

DAS_CORE_IPC_NS_END
