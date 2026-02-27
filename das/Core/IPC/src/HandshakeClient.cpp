#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/HandshakeClient.h>

#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>

#include <boost/process/v2/pid.hpp>

DAS_CORE_IPC_NS_BEGIN

HandshakeClient::HandshakeClient(IpcRunLoop& run_loop) : run_loop_(run_loop) {}

DasResult HandshakeClient::SendHelloAndWaitWelcome(
    const std::string&        client_name,
    uint16_t&                 out_session_id,
    std::chrono::milliseconds timeout)
{
    // 准备 HelloRequestV1
    HelloRequestV1 hello{};
    uint32_t my_pid = static_cast<uint32_t>(boost::process::v2::current_pid());
    InitHelloRequest(hello, my_pid, client_name.c_str());

    // 构建请求头
    IPCMessageHeader header{};
    header.magic = IPCMessageHeader::MAGIC;
    header.version = IPCMessageHeader::CURRENT_VERSION;
    header.message_type = static_cast<uint8_t>(MessageType::REQUEST);
    header.header_flags = 0;
    header.call_id = 0; // 由 SendMessage 内部分配
    header.interface_id =
        static_cast<uint32_t>(HandshakeInterfaceId::HANDSHAKE_IFACE_HELLO);
    header.method_id = 0;
    header.flags = 0;
    header.error_code = 0;
    header.body_size = sizeof(hello);
    header.session_id = 0;
    header.generation = 0;
    header.local_id = 0;

    // 使用 SendMessage 发送并等待响应
    std::vector<uint8_t> response_body;
    DasResult            result = run_loop_.SendMessage(
        header,
        reinterpret_cast<const uint8_t*>(&hello),
        sizeof(hello),
        response_body,
        timeout);

    if (DAS::IsFailed(result))
    {
        std::string msg = DAS_FMT_NS::format(
            "SendHelloAndWaitWelcome failed: error={}",
            result);
        DAS_LOG_ERROR(msg.c_str());
        return result;
    }

    // 解析 WelcomeResponseV1
    if (response_body.size() < sizeof(WelcomeResponseV1))
    {
        DAS_LOG_ERROR("Welcome response body too small");
        return DAS_E_IPC_INVALID_MESSAGE_BODY;
    }

    const WelcomeResponseV1* welcome =
        reinterpret_cast<const WelcomeResponseV1*>(response_body.data());

    if (welcome->status != WelcomeResponseV1::STATUS_SUCCESS)
    {
        std::string msg =
            DAS_FMT_NS::format("Welcome status error: {}", welcome->status);
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_HANDSHAKE_FAILED;
    }

    if (welcome->session_id == 0)
    {
        DAS_LOG_ERROR("Received invalid session_id (0)");
        return DAS_E_IPC_HANDSHAKE_FAILED;
    }

    out_session_id = welcome->session_id;

    std::string info_msg = DAS_FMT_NS::format(
        "SendHelloAndWaitWelcome succeeded: session_id={}",
        out_session_id);
    DAS_LOG_INFO(info_msg.c_str());

    return DAS_S_OK;
}

DasResult HandshakeClient::SendReadyAndWaitAck(
    uint16_t                  session_id,
    std::chrono::milliseconds timeout)
{
    // 准备 ReadyRequestV1
    ReadyRequestV1 ready{};
    InitReadyRequest(ready, session_id);

    // 构建请求头
    IPCMessageHeader header{};
    header.magic = IPCMessageHeader::MAGIC;
    header.version = IPCMessageHeader::CURRENT_VERSION;
    header.message_type = static_cast<uint8_t>(MessageType::REQUEST);
    header.header_flags = 0;
    header.call_id = 0; // 由 SendMessage 内部分配
    header.interface_id =
        static_cast<uint32_t>(HandshakeInterfaceId::HANDSHAKE_IFACE_READY);
    header.method_id = 0;
    header.flags = 0;
    header.error_code = 0;
    header.body_size = sizeof(ready);
    // 控制平面消息: ObjectId = {0, 0, 0}
    header.session_id = 0;
    header.generation = 0;
    header.local_id = 0;
    // 使用 SendMessage 发送并等待响应
    std::vector<uint8_t> response_body;
    DasResult            result = run_loop_.SendMessage(
        header,
        reinterpret_cast<const uint8_t*>(&ready),
        sizeof(ready),
        response_body,
        timeout);

    if (DAS::IsFailed(result))
    {
        std::string msg =
            DAS_FMT_NS::format("SendReadyAndWaitAck failed: error={}", result);
        DAS_LOG_ERROR(msg.c_str());
        return result;
    }

    // 解析 ReadyAckV1
    if (response_body.size() < sizeof(ReadyAckV1))
    {
        DAS_LOG_ERROR("ReadyAck response body too small");
        return DAS_E_IPC_INVALID_MESSAGE_BODY;
    }

    const ReadyAckV1* ack =
        reinterpret_cast<const ReadyAckV1*>(response_body.data());

    if (ack->status != ReadyAckV1::STATUS_SUCCESS)
    {
        std::string msg =
            DAS_FMT_NS::format("ReadyAck status error: {}", ack->status);
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_HANDSHAKE_FAILED;
    }

    std::string info_msg = DAS_FMT_NS::format(
        "SendReadyAndWaitAck succeeded: session_id={}",
        session_id);
    DAS_LOG_INFO(info_msg.c_str());

    return DAS_S_OK;
}

DasResult HandshakeClient::PerformHandshake(
    const std::string&        client_name,
    uint16_t&                 out_session_id,
    std::chrono::milliseconds timeout)
{
    // Step 1: 发送 HELLO，等待 WELCOME
    DasResult result =
        SendHelloAndWaitWelcome(client_name, out_session_id, timeout);
    if (DAS::IsFailed(result))
    {
        std::string msg = DAS_FMT_NS::format(
            "PerformHandshake: SendHelloAndWaitWelcome failed: {}",
            result);
        DAS_LOG_ERROR(msg.c_str());
        return result;
    }

    // Step 2: 发送 READY，等待 READY_ACK
    result = SendReadyAndWaitAck(out_session_id, timeout);
    if (DAS::IsFailed(result))
    {
        std::string msg = DAS_FMT_NS::format(
            "PerformHandshake: SendReadyAndWaitAck failed: {}",
            result);
        DAS_LOG_ERROR(msg.c_str());
        return result;
    }

    std::string info_msg = DAS_FMT_NS::format(
        "PerformHandshake completed: client={}, session_id={}",
        client_name,
        out_session_id);
    DAS_LOG_INFO(info_msg.c_str());

    return DAS_S_OK;
}
DAS_CORE_IPC_NS_END
