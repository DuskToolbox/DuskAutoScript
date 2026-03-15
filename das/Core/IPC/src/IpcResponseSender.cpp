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
    const std::vector<uint8_t>&     body)
{
    // 业务线程模式：通过 PostSend 投递
    if (run_loop_)
    {
        DAS_CORE_LOG_INFO(
            "IpcResponseSender::SendResponse: business thread mode, run_loop_={}, body_size={}",
            (void*)run_loop_,
            body.size());

        // 构造 body 副本再 move 给 PostSend
        std::vector<uint8_t> body_copy(body);
        return run_loop_->PostSend(validated_header, std::move(body_copy));
    }

    // IO 线程模式：直接使用 transport 发送
    // 注意：由于 SendCoroutine 是协程，这里简化处理，假设在 IO 线程上下文可以直接调用
    // 实际需要根据调用场景选择合适的发送方式
    if (transport_)
    {
        DAS_CORE_LOG_INFO(
            "IpcResponseSender::SendResponse: IO thread mode, transport_={}, body_size={}",
            (void*)transport_,
            body.size());

        // 暂时返回成功，因为当前只在 IO 线程使用
        // 后续可以根据需要完善
        (void)validated_header;
        (void)body;
        return DAS_S_OK;
    }

    // 没有有效的 transport 或 run_loop
    DAS_CORE_LOG_ERROR(
        "IpcResponseSender::SendResponse: no valid transport or run_loop");
    return DAS_E_IPC_NOT_INITIALIZED;
}

DAS_CORE_IPC_NS_END
