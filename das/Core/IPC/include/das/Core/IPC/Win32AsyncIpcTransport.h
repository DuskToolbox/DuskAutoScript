#ifndef DAS_CORE_IPC_WIN32_ASYNC_IPC_TRANSPORT_H
#define DAS_CORE_IPC_WIN32_ASYNC_IPC_TRANSPORT_H

#ifdef _WIN32

#include <asioexec/use_sender.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/windows/stream_handle.hpp>
#include <cstdint>
#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/DasExport.h>
#include <memory>
#include <string>
#include <vector>

#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IpcErrors.h>
#include <optional>
#include <variant>

DAS_CORE_IPC_NS_BEGIN

class Win32AsyncIpcTransport
{
public:
    explicit Win32AsyncIpcTransport(boost::asio::io_context& io_context);
    ~Win32AsyncIpcTransport();

    Win32AsyncIpcTransport(const Win32AsyncIpcTransport&) = delete;
    Win32AsyncIpcTransport& operator=(const Win32AsyncIpcTransport&) = delete;

    Win32AsyncIpcTransport(Win32AsyncIpcTransport&&) noexcept = default;
    Win32AsyncIpcTransport& operator=(Win32AsyncIpcTransport&&) noexcept =
        default;

    [[nodiscard]]
    auto Receive()
    {
        return boost::asio::co_spawn(
            io_context_,
            ReceiveCoroutine(),
            asioexec::use_sender);
    }

    [[nodiscard]]
    auto Send(
        const ValidatedIPCMessageHeader& header,
        const uint8_t*                   body,
        size_t                           body_size)
    {
        return boost::asio::co_spawn(
            io_context_,
            SendCoroutine(header, body, body_size),
            asioexec::use_sender);
    }

    DasResult Initialize(
        const std::string& endpoint_name,
        bool               is_server,
        size_t             max_message_size = 65536);

    DasResult Connect(const std::string& endpoint_name);

    void Close();

    [[nodiscard]]
    bool IsConnected() const;

    [[nodiscard]]
    std::string GetEndpointName() const;

    void SetSharedMemoryPool(SharedMemoryPool* pool);

private:
    DasResult CreateNamedPipe(const std::string& pipe_name, bool is_read_pipe);
    DasResult ConnectToNamedPipe(
        const std::string& pipe_name,
        bool               is_read_pipe);

    boost::asio::awaitable<std::variant<DasResult, AsyncIpcMessage>>
                                      ReceiveCoroutine();
    boost::asio::awaitable<DasResult> SendCoroutine(
        const ValidatedIPCMessageHeader& header,
        const uint8_t*                   body,
        size_t                           body_size);

    boost::asio::io_context&            io_context_;
    boost::asio::windows::stream_handle read_pipe_;
    boost::asio::windows::stream_handle write_pipe_;

    std::string endpoint_name_;
    bool        is_server_ = false;
    bool        is_connected_ = false;
    size_t      max_message_size_ = 65536;

    SharedMemoryPool* shared_memory_pool_ = nullptr;

    std::vector<uint8_t> header_buffer_;
    std::vector<uint8_t> body_buffer_;
};

DAS_CORE_IPC_NS_END

#endif // _WIN32

#endif // DAS_CORE_IPC_WIN32_ASYNC_IPC_TRANSPORT_H
