#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/IpcRunLoop.h>

DAS_CORE_IPC_NS_BEGIN

IpcResponseSender::IpcResponseSender(IpcRunLoop& run_loop) : run_loop_(run_loop)
{
}

DasResult IpcResponseSender::SendResponse(
    const IPCMessageHeader&     request_header,
    const std::vector<uint8_t>& body)
{
    // 创建响应头（设置 message_type = RESPONSE）
    IPCMessageHeader response_header = request_header;
    response_header.message_type = static_cast<uint8_t>(MessageType::RESPONSE);
    return run_loop_.SendResponse(response_header, body.data(), body.size());
}
DAS_CORE_IPC_NS_END
