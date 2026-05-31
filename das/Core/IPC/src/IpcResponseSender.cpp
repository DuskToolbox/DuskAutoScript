#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IHostConnection.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/IpcRunLoop.h>

#include <das/Core/Logger/Logger.h>
#include <das/Utils/fmt.h>
#include <utility>

DAS_CORE_IPC_NS_BEGIN

IpcResponseSender::IpcResponseSender(IpcRunLoop& run_loop)
    : IpcResponseSender(SessionRoute{&run_loop})
{
}

IpcResponseSender::IpcResponseSender(SessionRoute route)
    : route_kind_(RouteKind::Session), run_loop_(route.run_loop)
{
}

IpcResponseSender::IpcResponseSender(HostConnectionRoute route)
    : route_kind_(RouteKind::HostConnection), run_loop_(route.run_loop),
      connection_(std::move(route.connection))
{
}

DasResult IpcResponseSender::SendViaRoute(
    const ValidatedIPCMessageHeader& validated_header,
    std::vector<uint8_t>&&           body)
{
    if (!run_loop_)
    {
        DAS_CORE_LOG_ERROR("Response route is not initialized");
        return DAS_E_IPC_NOT_INITIALIZED;
    }

    switch (route_kind_)
    {
    case RouteKind::Session:
    {
        DAS_CORE_LOG_DEBUG(
            "Routing response through session route, body_size = {}",
            body.size());
        return run_loop_->PostSend(validated_header, std::move(body));
    }
    case RouteKind::HostConnection:
    {
        if (!connection_)
        {
            DAS_CORE_LOG_ERROR("Host connection route is not initialized");
            return DAS_E_IPC_NOT_INITIALIZED;
        }

        DAS_CORE_LOG_DEBUG(
            "Routing response through host connection route, body_size = {}",
            body.size());
        return run_loop_->PostSendWithTransport(
            connection_,
            validated_header,
            std::move(body));
    }
    case RouteKind::None:
        break;
    }

    DAS_CORE_LOG_ERROR("Response route kind is invalid");
    return DAS_E_IPC_NOT_INITIALIZED;
}

DasResult IpcResponseSender::SendResponse(
    const ValidatedIPCMessageHeader& validated_header,
    const std::vector<uint8_t>&      body)
{
    std::vector<uint8_t> body_copy(body);
    return SendViaRoute(validated_header, std::move(body_copy));
}

DasResult IpcResponseSender::SendResponse(
    const ValidatedIPCMessageHeader& validated_header,
    std::vector<uint8_t>&&           body)
{
    return SendViaRoute(validated_header, std::move(body));
}

boost::asio::awaitable<DasResult> IpcResponseSender::SendResponseAsync(
    const ValidatedIPCMessageHeader& validated_header,
    const std::vector<uint8_t>&      body)
{
    std::vector<uint8_t> body_copy(body);
    co_return SendViaRoute(validated_header, std::move(body_copy));
}

boost::asio::awaitable<DasResult> IpcResponseSender::SendResponseAsync(
    const ValidatedIPCMessageHeader& validated_header,
    std::vector<uint8_t>&&           body)
{
    co_return SendViaRoute(validated_header, std::move(body));
}

DAS_CORE_IPC_NS_END
