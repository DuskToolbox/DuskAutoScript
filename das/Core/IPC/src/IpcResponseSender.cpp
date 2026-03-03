#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/IpcRunLoop.h>

DAS_CORE_IPC_NS_BEGIN

IpcResponseSender::IpcResponseSender(IpcRunLoop& run_loop) : run_loop_(run_loop)
{
}

DasResult IpcResponseSender::SendResponse(
    const ValidatedIPCMessageHeader& validated_header,
    const std::vector<uint8_t>&      body)
{
    // 直接转发已验证的 header
    return run_loop_.SendResponse(validated_header, body.data(), body.size());
}
DAS_CORE_IPC_NS_END
