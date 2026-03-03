#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/HandshakeClient.h>

#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>

#include <boost/process/v2/pid.hpp>

DAS_CORE_IPC_NS_BEGIN

HandshakeClient::HandshakeClient(IpcRunLoop& run_loop) : run_loop_(run_loop) {}

AwaitResponseSender HandshakeClient::SendHelloAsync(
    const std::string&        client_name,
    std::chrono::milliseconds timeout)
{
    // 准备 HelloRequestV1
    HelloRequestV1 hello{};
    uint32_t my_pid = static_cast<uint32_t>(boost::process::v2::current_pid());
    InitHelloRequest(hello, my_pid, client_name.c_str());

    // 构建请求头
    auto validated_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::REQUEST)
            .SetControlPlaneCommand(HandshakeInterfaceId::HANDSHAKE_IFACE_HELLO)
            .SetBodySize(sizeof(hello))
            .Build();

    // 返回异步 sender（不阻塞）
    return run_loop_.SendMessageAsync(
        validated_header,
        reinterpret_cast<const uint8_t*>(&hello),
        sizeof(hello),
        timeout);
}

AwaitResponseSender HandshakeClient::SendReadyAsync(
    uint16_t                  session_id,
    std::chrono::milliseconds timeout)
{
    // 准备 ReadyRequestV1
    ReadyRequestV1 ready{};
    InitReadyRequest(ready, session_id);

    // 构建请求头
    auto validated_header =
        IPCMessageHeaderBuilder()
            .SetMessageType(MessageType::REQUEST)
            .SetControlPlaneCommand(HandshakeInterfaceId::HANDSHAKE_IFACE_READY)
            .SetBodySize(sizeof(ready))
            .Build();

    return run_loop_.SendMessageAsync(
        validated_header,
        reinterpret_cast<const uint8_t*>(&ready),
        sizeof(ready),
        timeout);
}

DAS_CORE_IPC_NS_END
