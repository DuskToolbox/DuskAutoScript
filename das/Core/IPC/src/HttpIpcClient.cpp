#include <das/Core/IPC/HttpIpcClient.h>

#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/HttpIpcTransport.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>

#include <boost/asio/use_awaitable.hpp>

DAS_CORE_IPC_NS_BEGIN

struct HttpIpcClient::Impl
{
    std::unique_ptr<HttpIpcTransport> transport;
    uint16_t                          session_id = 0;
};

HttpIpcClient::HttpIpcClient(
    std::unique_ptr<HttpIpcTransport> transport,
    uint16_t                          session_id)
    : impl_(std::make_unique<Impl>())
{
    impl_->transport = std::move(transport);
    impl_->session_id = session_id;
}

HttpIpcClient::~HttpIpcClient() = default;

boost::asio::awaitable<std::variant<DasResult, std::unique_ptr<HttpIpcClient>>>
HttpIpcClient::Connect(
    boost::asio::io_context& io_context,
    const std::string&       host,
    const std::string&       port,
    uint32_t                 my_pid)
{
    // ── Phase 1: TCP connect + WebSocket upgrade ──
    boost::asio::ip::tcp::resolver resolver(io_context);
    auto                           endpoints =
        co_await resolver.async_resolve(host, port, boost::asio::use_awaitable);

    boost::asio::ip::tcp::socket socket(io_context);
    co_await boost::asio::async_connect(
        socket,
        endpoints,
        boost::asio::use_awaitable);

    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws(
        std::move(socket));
    co_await ws.async_handshake(
        host + ":" + port,
        "/ipc/v1/transport",
        boost::asio::use_awaitable);

    auto transport = std::make_unique<HttpIpcTransport>(std::move(ws));
    if (!transport->IsConnected())
    {
        DAS_CORE_LOG_ERROR("WebSocket connect failed to {}:{}", host, port);
        co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    // ── Phase 2: IPC Handshake over the WebSocket transport ──

    // Step 1: Send Hello
    {
        HelloRequestV1 hello{};
        InitHelloRequest(hello, my_pid, "HttpIpcClient");

        uint16_t call_id = 1;
        auto     validated_header =
            IPCMessageHeaderBuilder()
                .SetMessageType(MessageType::REQUEST)
                .SetControlPlaneCommand(
                    HandshakeInterfaceId::HANDSHAKE_IFACE_HELLO)
                .SetBodySize(sizeof(hello))
                .SetCallId(call_id)
                .Build();

        DasResult result = co_await transport->SendCoroutine(
            validated_header,
            reinterpret_cast<const uint8_t*>(&hello),
            sizeof(hello));

        if (result != DAS_S_OK)
        {
            DAS_CORE_LOG_ERROR("Failed to send Hello: error = {}", result);
            co_return result;
        }

        DAS_CORE_LOG_INFO("Sent Hello: pid = {}", my_pid);
    }

    // Step 2: Receive Welcome
    uint16_t session_id = 0;
    {
        auto result_variant = co_await transport->ReceiveCoroutine();

        if (result_variant.index() == 0)
        {
            DasResult error_code = std::get<0>(result_variant);
            DAS_CORE_LOG_ERROR(
                "Failed to receive Welcome: error = {}",
                error_code);
            co_return error_code;
        }

        auto&& [header, body] = std::get<1>(result_variant);
        const IPCMessageHeader& raw_header = header.Raw();

        if (raw_header.interface_id
            != static_cast<uint32_t>(
                HandshakeInterfaceId::HANDSHAKE_IFACE_WELCOME))
        {
            DAS_CORE_LOG_ERROR(
                "Unexpected interface_id: {}",
                raw_header.interface_id);
            co_return static_cast<DasResult>(DAS_E_IPC_UNEXPECTED_MESSAGE);
        }

        if (body.size() < sizeof(WelcomeResponseV1))
        {
            DAS_CORE_LOG_ERROR("Welcome response body too small");
            co_return static_cast<DasResult>(DAS_E_IPC_INVALID_MESSAGE_BODY);
        }

        WelcomeResponseV1 welcome =
            *reinterpret_cast<const WelcomeResponseV1*>(body.data());

        if (welcome.status != WelcomeResponseV1::STATUS_SUCCESS)
        {
            DAS_CORE_LOG_ERROR("Welcome status error: {}", welcome.status);
            co_return static_cast<DasResult>(DAS_E_IPC_HANDSHAKE_FAILED);
        }

        session_id = welcome.session_id;

        if (session_id == 0)
        {
            DAS_CORE_LOG_ERROR("Received invalid session_id (0)");
            co_return static_cast<DasResult>(DAS_E_IPC_HANDSHAKE_FAILED);
        }

        DAS_CORE_LOG_INFO(
            "Received Welcome: session_id = {}, status = {}",
            welcome.session_id,
            welcome.status);
    }

    // Step 3: Send Ready
    {
        ReadyRequestV1 ready{};
        InitReadyRequest(ready, session_id);

        uint16_t call_id = 2;
        auto     validated_header =
            IPCMessageHeaderBuilder()
                .SetMessageType(MessageType::REQUEST)
                .SetControlPlaneCommand(
                    HandshakeInterfaceId::HANDSHAKE_IFACE_READY)
                .SetBodySize(sizeof(ready))
                .SetCallId(call_id)
                .Build();

        DasResult result = co_await transport->SendCoroutine(
            validated_header,
            reinterpret_cast<const uint8_t*>(&ready),
            sizeof(ready));

        if (result != DAS_S_OK)
        {
            DAS_CORE_LOG_ERROR("Failed to send Ready: error = {}", result);
            co_return result;
        }

        DAS_CORE_LOG_INFO("Sent Ready: session_id = {}", session_id);
    }

    // Step 4: Receive ReadyAck
    {
        auto result_variant = co_await transport->ReceiveCoroutine();

        if (result_variant.index() == 0)
        {
            DasResult error_code = std::get<0>(result_variant);
            DAS_CORE_LOG_ERROR(
                "Failed to receive ReadyAck: error = {}",
                error_code);
            co_return error_code;
        }

        auto&& [header, body] = std::get<1>(result_variant);
        const IPCMessageHeader& raw_header = header.Raw();

        if (raw_header.interface_id
            != static_cast<uint32_t>(
                HandshakeInterfaceId::HANDSHAKE_IFACE_READY_ACK))
        {
            DAS_CORE_LOG_ERROR(
                "Unexpected interface_id: {}",
                raw_header.interface_id);
            co_return static_cast<DasResult>(DAS_E_IPC_UNEXPECTED_MESSAGE);
        }

        if (body.size() < sizeof(ReadyAckV1))
        {
            DAS_CORE_LOG_ERROR("ReadyAck response body too small");
            co_return static_cast<DasResult>(DAS_E_IPC_INVALID_MESSAGE_BODY);
        }

        ReadyAckV1 ack = *reinterpret_cast<const ReadyAckV1*>(body.data());

        if (ack.status != ReadyAckV1::STATUS_SUCCESS)
        {
            DAS_CORE_LOG_ERROR("ReadyAck status error: {}", ack.status);
            co_return static_cast<DasResult>(DAS_E_IPC_HANDSHAKE_FAILED);
        }
    }

    DAS_CORE_LOG_INFO("Full handshake completed: session_id = {}", session_id);

    // Create fully initialized client
    co_return std::unique_ptr<HttpIpcClient>(
        new HttpIpcClient(std::move(transport), session_id));
}

HttpIpcTransport* HttpIpcClient::GetTransport() const
{
    return impl_->transport.get();
}

std::unique_ptr<HttpIpcTransport> HttpIpcClient::ReleaseTransport()
{
    return std::move(impl_->transport);
}

uint16_t HttpIpcClient::GetSessionId() const { return impl_->session_id; }

DAS_CORE_IPC_NS_END
