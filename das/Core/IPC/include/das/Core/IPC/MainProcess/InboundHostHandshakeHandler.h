#ifndef DAS_CORE_IPC_MAIN_PROCESS_INBOUND_HOST_HANDSHAKE_HANDLER_H
#define DAS_CORE_IPC_MAIN_PROCESS_INBOUND_HOST_HANDSHAKE_HANDLER_H

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <das/Core/IPC/AnyTransport.h>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/Handshake.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>

DAS_CORE_IPC_NS_BEGIN
namespace MainProcess
{
    class InboundHostHandshakeHandler
    {
    public:
        InboundHostHandshakeHandler(
            AnyTransport& transport,
            uint16_t      session_id);

        boost::asio::awaitable<DasResult> Run();

    private:
        boost::asio::awaitable<DasResult> ReceiveHello(
            ValidatedIPCMessageHeader& header,
            HelloRequestV1&            hello);

        boost::asio::awaitable<DasResult> ReceiveReady(
            ValidatedIPCMessageHeader& header,
            ReadyRequestV1&            ready);

        boost::asio::awaitable<DasResult> SendWelcome(
            const ValidatedIPCMessageHeader& request_header,
            uint32_t                         status);

        boost::asio::awaitable<DasResult> SendReadyAck(
            const ValidatedIPCMessageHeader& request_header,
            uint32_t                         status);

        [[nodiscard]]
        uint32_t ValidateHello(const HelloRequestV1& hello) const;

        AnyTransport& transport_;
        uint16_t      session_id_;
    };
} // namespace MainProcess
DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_MAIN_PROCESS_INBOUND_HOST_HANDSHAKE_HANDLER_H
