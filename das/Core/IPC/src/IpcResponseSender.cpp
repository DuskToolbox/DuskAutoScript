#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/IpcRunLoop.h>

#include <das/Core/Logger/Logger.h>
#include <das/Utils/fmt.h>

#ifdef _WIN32
#include <das/Core/IPC/Win32AsyncIpcTransport.h>
#else
#include <das/Core/IPC/UnixAsyncIpcTransport.h>
#endif

DAS_CORE_IPC_NS_BEGIN

IpcResponseSender::IpcResponseSender(IpcRunLoop& run_loop)
    : run_loop_(&run_loop), transport_(nullptr)
{
}

#ifdef _WIN32
IpcResponseSender::IpcResponseSender(Win32AsyncIpcTransport& transport)
    : run_loop_(nullptr), transport_(&transport)
{
}
#else
IpcResponseSender::IpcResponseSender(UnixAsyncIpcTransport& transport)
    : run_loop_(nullptr), transport_(&transport)
{
}
#endif

boost::asio::awaitable<DasResult> IpcResponseSender::SendResponse(
    const ValidatedIPCMessageHeader& validated_header,
    const std::vector<uint8_t>&      body)
{
    DAS_CORE_LOG_INFO("IpcResponseSender::SendResponse: transport_={}, run_loop_={}",
                      (void*)transport_, (void*)run_loop_);

    // 优先使用 transport（新模式）
    if (transport_)
    {
        DAS_CORE_LOG_INFO("IpcResponseSender::SendResponse: using transport->SendCoroutine, body_size={}", body.size());
        auto result = co_await transport_->SendCoroutine(validated_header, body.data(), body.size());
        DAS_CORE_LOG_INFO("IpcResponseSender::SendResponse: SendCoroutine returned 0x{:08X}", result);
        co_return result;
    }

    // 旧模式：通过 IpcRunLoop 发送（已废弃，可能失败）
    if (run_loop_)
    {
        DAS_CORE_LOG_WARN("IpcResponseSender::SendResponse: using deprecated run_loop_ path");
        co_return co_await run_loop_->SendResponseCoroutine(
            validated_header, body.data(), body.size());
    }

    DAS_CORE_LOG_ERROR("IpcResponseSender::SendResponse: no transport or run_loop available!");
    co_return DAS_E_IPC_NOT_INITIALIZED;
}

DAS_CORE_IPC_NS_END
