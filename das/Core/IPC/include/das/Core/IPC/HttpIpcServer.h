#ifndef DAS_CORE_IPC_HTTP_IPC_SERVER_H
#define DAS_CORE_IPC_HTTP_IPC_SERVER_H

#include <boost/asio/ip/tcp.hpp>
#include <das/Core/IPC/Config.h>
#include <das/IDasBase.h>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_BEAST_WARNING
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
DAS_DISABLE_WARNING_END

#include <functional>
#include <memory>
#include <string>

DAS_CORE_IPC_NS_BEGIN

// Callback type: Called when a Host completes WebSocket upgrade
// ws: the WebSocket stream (ownership transferred to HttpIpcTransport)
// endpoint_name: descriptive endpoint string for logging
using OnHostConnected = std::function<DasResult(
    std::unique_ptr<
        boost::beast::websocket::stream<boost::asio::ip::tcp::socket>> ws,
    const std::string& endpoint_name)>;

class HttpIpcServer
{
public:
    struct Config
    {
        std::string listen_address = "127.0.0.1";
        uint16_t    listen_port = 9527;
    };

    /// RAII constructor: binds, listens, and starts async_accept immediately.
    /// Throws std::runtime_error if bind/listen fails.
    HttpIpcServer(
        boost::asio::io_context& io_context,
        Config                   config,
        OnHostConnected          on_connected);

    ~HttpIpcServer();

    void Stop();

    uint16_t GetPort() const;
    bool     IsRunning() const;

    HttpIpcServer(const HttpIpcServer&) = delete;
    HttpIpcServer& operator=(const HttpIpcServer&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_HTTP_IPC_SERVER_H
