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

DasResult IpcResponseSender::SendResponse(
    const ValidatedIPCMessageHeader& validated_header,
    const std::vector<uint8_t>&      body)
{
    // 业务线程模式：通过 PostSend 投递到 IO 线程
    if (run_loop_)
    {
        DAS_CORE_LOG_INFO(
            "IpcResponseSender::SendResponse: body_size={}",
            body.size());
        std::vector<uint8_t> body_copy(body);
        return run_loop_->PostSend(validated_header, std::move(body_copy));
    }

    DAS_CORE_LOG_ERROR("IpcResponseSender::SendResponse: no valid run_loop");
    return DAS_E_IPC_NOT_INITIALIZED;
}

boost::asio::awaitable<DasResult> IpcResponseSender::SendResponseAsync(
    const ValidatedIPCMessageHeader& validated_header,
    const std::vector<uint8_t>&      body)
{
    // IO 线程模式：直接 co_await transport 发送
    if (transport_)
    {
        DAS_CORE_LOG_INFO(
            "IpcResponseSender::SendResponseAsync: body_size={}",
            body.size());
        co_return co_await transport_->SendCoroutine(
            validated_header,
            body.data(),
            body.size());
    }

    DAS_CORE_LOG_ERROR(
        "IpcResponseSender::SendResponseAsync: no valid transport");
    co_return DAS_E_IPC_NOT_INITIALIZED;
}

DAS_CORE_IPC_NS_END
