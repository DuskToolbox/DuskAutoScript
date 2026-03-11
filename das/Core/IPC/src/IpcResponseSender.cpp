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

boost::asio::awaitable<DasResult> IpcResponseSender::SendResponse(
    const ValidatedIPCMessageHeader& validated_header,
    const std::vector<uint8_t>&      body)
{
    DAS_CORE_LOG_INFO(
        "IpcResponseSender::SendResponse: transport_={}, body_size={}",
        (void*)transport_,
        body.size());

    // 使用 transport 直接发送
    auto result = co_await transport_->SendCoroutine(
        validated_header,
        body.data(),
        body.size());
    DAS_CORE_LOG_INFO(
        "IpcResponseSender::SendResponse: SendCoroutine returned 0x{:08X}",
        result);
    co_return result;
}

DAS_CORE_IPC_NS_END
