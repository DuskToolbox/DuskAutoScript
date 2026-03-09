#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/IpcRunLoop.h>

DAS_CORE_IPC_NS_BEGIN

IpcResponseSender::IpcResponseSender(IpcRunLoop& run_loop) : run_loop_(run_loop)
{
}

boost::asio::awaitable<DasResult> IpcResponseSender::SendResponse(
    const ValidatedIPCMessageHeader& validated_header,
    const std::vector<uint8_t>&      body)
{
    // 使用协程版本发送响应，避免死锁
    co_return co_await run_loop_.SendResponseCoroutine(
        validated_header, body.data(), body.size());
}

DAS_CORE_IPC_NS_END
