#include <das/Core/IPC/MainProcess/InboundHostHandshakeHandler.h>

#include <algorithm>
#include <cstring>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/Logger/Logger.h>
#include <vector>

DAS_CORE_IPC_NS_BEGIN
namespace MainProcess
{
    namespace
    {
        constexpr uint16_t kMainProcessSessionId = 1;

        template <typename T>
        void CopyBody(const std::vector<uint8_t>& body, T& out)
        {
            std::memcpy(&out, body.data(), sizeof(T));
        }

        [[nodiscard]]
        bool IsExpectedHandshakeRequest(
            const ValidatedIPCMessageHeader& header,
            HandshakeInterfaceId             expected)
        {
            return header.IsControlPlane()
                && header.GetMessageType() == MessageType::REQUEST
                && header.GetInterfaceId() == static_cast<uint32_t>(expected);
        }
    } // namespace

    InboundHostHandshakeHandler::InboundHostHandshakeHandler(
        AnyTransport& transport,
        uint16_t      session_id)
        : transport_(transport), session_id_(session_id)
    {
    }

    boost::asio::awaitable<DasResult> InboundHostHandshakeHandler::Run()
    {
        ValidatedIPCMessageHeader hello_header;
        HelloRequestV1            hello{};
        DasResult                 result =
            co_await ReceiveHello(hello_header, hello);
        if (DAS::IsFailed(result))
        {
            co_return result;
        }

        const uint32_t welcome_status = ValidateHello(hello);
        result = co_await SendWelcome(hello_header, welcome_status);
        if (DAS::IsFailed(result))
        {
            co_return result;
        }
        if (welcome_status != WelcomeResponseV1::STATUS_SUCCESS)
        {
            co_return DAS_E_IPC_HANDSHAKE_FAILED;
        }

        ValidatedIPCMessageHeader ready_header;
        ReadyRequestV1            ready{};
        result = co_await ReceiveReady(ready_header, ready);
        if (DAS::IsFailed(result))
        {
            co_return result;
        }

        const uint32_t ready_status =
            ready.session_id == session_id_
                ? ReadyAckV1::STATUS_SUCCESS
                : ReadyAckV1::STATUS_INVALID_SESSION;
        result = co_await SendReadyAck(ready_header, ready_status);
        if (DAS::IsFailed(result))
        {
            co_return result;
        }

        if (ready_status != ReadyAckV1::STATUS_SUCCESS)
        {
            DAS_CORE_LOG_ERROR(
                "InboundHostHandshakeHandler: READY session mismatch, expected={}, actual={}",
                session_id_,
                ready.session_id);
            co_return DAS_E_IPC_HANDSHAKE_FAILED;
        }

        DAS_CORE_LOG_INFO(
            "InboundHostHandshakeHandler: handshake complete, session_id={}, pid={}, name={}",
            session_id_,
            hello.pid,
            hello.plugin_name);
        co_return DAS_S_OK;
    }

    boost::asio::awaitable<DasResult>
    InboundHostHandshakeHandler::ReceiveHello(
        ValidatedIPCMessageHeader& header,
        HelloRequestV1&            hello)
    {
        auto received = co_await transport_.ReceiveCoroutine();
        if (received.index() == 0)
        {
            co_return std::get<0>(received);
        }

        auto&& [received_header, body] = std::get<1>(received);
        if (!IsExpectedHandshakeRequest(
                received_header,
                HandshakeInterfaceId::HANDSHAKE_IFACE_HELLO))
        {
            DAS_CORE_LOG_ERROR(
                "InboundHostHandshakeHandler: expected HELLO, got interface_id={}, type={}, flags={}",
                received_header.GetInterfaceId(),
                static_cast<int>(received_header.GetMessageType()),
                static_cast<int>(received_header.GetHeaderFlags()));
            co_return DAS_E_IPC_UNEXPECTED_MESSAGE;
        }

        if (body.size() < sizeof(HelloRequestV1))
        {
            DAS_CORE_LOG_ERROR(
                "InboundHostHandshakeHandler: HELLO body too small: {} < {}",
                body.size(),
                sizeof(HelloRequestV1));
            co_return DAS_E_IPC_INVALID_MESSAGE_BODY;
        }

        header = received_header;
        CopyBody(body, hello);
        co_return DAS_S_OK;
    }

    boost::asio::awaitable<DasResult>
    InboundHostHandshakeHandler::ReceiveReady(
        ValidatedIPCMessageHeader& header,
        ReadyRequestV1&            ready)
    {
        auto received = co_await transport_.ReceiveCoroutine();
        if (received.index() == 0)
        {
            co_return std::get<0>(received);
        }

        auto&& [received_header, body] = std::get<1>(received);
        if (!IsExpectedHandshakeRequest(
                received_header,
                HandshakeInterfaceId::HANDSHAKE_IFACE_READY))
        {
            DAS_CORE_LOG_ERROR(
                "InboundHostHandshakeHandler: expected READY, got interface_id={}, type={}, flags={}",
                received_header.GetInterfaceId(),
                static_cast<int>(received_header.GetMessageType()),
                static_cast<int>(received_header.GetHeaderFlags()));
            co_return DAS_E_IPC_UNEXPECTED_MESSAGE;
        }

        if (body.size() < sizeof(ReadyRequestV1))
        {
            DAS_CORE_LOG_ERROR(
                "InboundHostHandshakeHandler: READY body too small: {} < {}",
                body.size(),
                sizeof(ReadyRequestV1));
            co_return DAS_E_IPC_INVALID_MESSAGE_BODY;
        }

        header = received_header;
        CopyBody(body, ready);
        co_return DAS_S_OK;
    }

    boost::asio::awaitable<DasResult>
    InboundHostHandshakeHandler::SendWelcome(
        const ValidatedIPCMessageHeader& request_header,
        uint32_t                         status)
    {
        WelcomeResponseV1 welcome{};
        InitWelcomeResponse(
            welcome,
            status == WelcomeResponseV1::STATUS_SUCCESS ? session_id_ : 0,
            status);

        auto header =
            IPCMessageHeaderBuilder()
                .SetMessageType(MessageType::RESPONSE)
                .SetControlPlaneCommand(
                    HandshakeInterfaceId::HANDSHAKE_IFACE_WELCOME)
                .SetBodySize(sizeof(welcome))
                .SetCallId(request_header.GetCallId())
                .SetSourceSessionId(kMainProcessSessionId)
                .SetTargetSessionId(request_header.GetSourceSessionId())
                .SetErrorCode(
                    status == WelcomeResponseV1::STATUS_SUCCESS
                        ? 0
                        : DAS_E_IPC_HANDSHAKE_FAILED)
                .Build();

        co_return co_await transport_.SendCoroutine(
            header,
            reinterpret_cast<const uint8_t*>(&welcome),
            sizeof(welcome));
    }

    boost::asio::awaitable<DasResult>
    InboundHostHandshakeHandler::SendReadyAck(
        const ValidatedIPCMessageHeader& request_header,
        uint32_t                         status)
    {
        ReadyAckV1 ack{};
        InitReadyAck(ack, status);

        auto header =
            IPCMessageHeaderBuilder()
                .SetMessageType(MessageType::RESPONSE)
                .SetControlPlaneCommand(
                    HandshakeInterfaceId::HANDSHAKE_IFACE_READY_ACK)
                .SetBodySize(sizeof(ack))
                .SetCallId(request_header.GetCallId())
                .SetSourceSessionId(kMainProcessSessionId)
                .SetTargetSessionId(request_header.GetSourceSessionId())
                .SetErrorCode(
                    status == ReadyAckV1::STATUS_SUCCESS
                        ? 0
                        : DAS_E_IPC_HANDSHAKE_FAILED)
                .Build();

        co_return co_await transport_.SendCoroutine(
            header,
            reinterpret_cast<const uint8_t*>(&ack),
            sizeof(ack));
    }

    uint32_t InboundHostHandshakeHandler::ValidateHello(
        const HelloRequestV1& hello) const
    {
        if (hello.protocol_version != HelloRequestV1::CURRENT_PROTOCOL_VERSION)
        {
            DAS_CORE_LOG_ERROR(
                "InboundHostHandshakeHandler: protocol version mismatch: {} != {}",
                hello.protocol_version,
                HelloRequestV1::CURRENT_PROTOCOL_VERSION);
            return WelcomeResponseV1::STATUS_VERSION_MISMATCH;
        }

        const auto* name_begin = hello.plugin_name;
        const auto* name_end =
            hello.plugin_name + HelloRequestV1::PLUGIN_NAME_SIZE;
        const auto* null_pos = std::find(name_begin, name_end, '\0');
        if (null_pos == name_end || null_pos == name_begin)
        {
            DAS_CORE_LOG_ERROR(
                "InboundHostHandshakeHandler: invalid peer name");
            return WelcomeResponseV1::STATUS_INVALID_NAME;
        }

        return WelcomeResponseV1::STATUS_SUCCESS;
    }
} // namespace MainProcess
DAS_CORE_IPC_NS_END
