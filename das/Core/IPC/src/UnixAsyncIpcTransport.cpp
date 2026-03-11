#ifndef _WIN32

#include <das/Core/IPC/UnixAsyncIpcTransport.h>

#include <asioexec/use_sender.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <cstring>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>
#include <memory>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <das/Core/IPC/Config.h>
#include <variant>

DAS_CORE_IPC_NS_BEGIN

inline constexpr uint16_t kFlagLargeMessage = 0x01;

struct UnixAsyncIpcTransport::Impl
{
    boost::asio::io_context&                    io_context;
    boost::asio::local::stream_protocol::socket socket;

    std::string endpoint_name;
    bool        is_server = false;
    bool        is_connected = false;
    size_t      max_message_size = 65536;

    SharedMemoryPool* shared_memory_pool = nullptr;

    std::vector<uint8_t> header_buffer;
    std::vector<uint8_t> body_buffer;

    explicit Impl(boost::asio::io_context& ctx)
        : io_context(ctx), socket(ctx), header_buffer(sizeof(IPCMessageHeader))
    {
    }
};

// 私有构造函数（由 Create() 工厂函数调用）
UnixAsyncIpcTransport::UnixAsyncIpcTransport(
    boost::asio::io_context& io_context)
    : impl_(std::make_unique<Impl>(io_context))
{
}

// 工厂函数实现
DAS::Utils::Expected<std::unique_ptr<UnixAsyncIpcTransport>> UnixAsyncIpcTransport::Create(
    boost::asio::io_context& io_context,
    const std::string&       read_endpoint,
    const std::string&       write_endpoint,
    bool                     is_server,
    size_t                   max_message_size)
{
    auto instance = std::unique_ptr<UnixAsyncIpcTransport>(
        new UnixAsyncIpcTransport(io_context));

    // 初始化
    auto result = instance->Initialize(
        read_endpoint, write_endpoint, is_server, max_message_size);

    if (result != DAS_S_OK)
    {
        return DAS::Utils::MakeUnexpected(result);
    }

    return instance;
}

UnixAsyncIpcTransport::~UnixAsyncIpcTransport() { Uninitialize(); }

DasResult UnixAsyncIpcTransport::Initialize(
    const std::string& read_endpoint,
    const std::string& write_endpoint,
    bool               is_server,
    size_t             max_message_size)
{
    (void)write_endpoint;

    if (impl_->is_connected)
    {
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    impl_->endpoint_name = read_endpoint;
    impl_->is_server = is_server;
    impl_->max_message_size = max_message_size;
    impl_->body_buffer.reserve(max_message_size);

    if (is_server)
    {
        return CreateUnixSocket(read_endpoint);
    }

    return DAS_S_OK;
}

DasResult UnixAsyncIpcTransport::Connect(
    const std::string& read_endpoint,
    const std::string& write_endpoint)
{
    (void)write_endpoint;

    if (impl_->is_connected)
    {
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    impl_->endpoint_name = read_endpoint;
    return ConnectToUnixSocket(read_endpoint);
}

void UnixAsyncIpcTransport::Uninitialize()
{
    boost::system::error_code ec;

    if (impl_->socket.is_open())
    {
        impl_->socket.close(ec);
    }

    impl_->is_connected = false;
    impl_->is_server = false;
}

bool UnixAsyncIpcTransport::IsConnected() const { return impl_->is_connected; }

std::string UnixAsyncIpcTransport::GetEndpointName() const
{
    return impl_->endpoint_name;
}

void UnixAsyncIpcTransport::SetSharedMemoryPool(SharedMemoryPool* pool)
{
    impl_->shared_memory_pool = pool;
}

DasResult UnixAsyncIpcTransport::CreateUnixSocket(
    const std::string& socket_path)
{
    boost::system::error_code ec;

    impl_->socket.open(ec);
    if (ec)
    {
        const auto msg = DAS_FMT_NS::format(
            "socket open failed: path = {}, error = {}",
            socket_path,
            ec.message());
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    unlink(socket_path.c_str());

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    boost::system::error_code bind_ec;
    impl_->socket.bind(
        boost::asio::local::stream_protocol::endpoint(
            reinterpret_cast<struct sockaddr*>(&addr),
            sizeof(addr)),
        bind_ec);

    if (bind_ec)
    {
        const auto msg = DAS_FMT_NS::format(
            "bind failed: path = {}, error = {}",
            socket_path,
            bind_ec.message());
        DAS_LOG_ERROR(msg.c_str());
        impl_->socket.close();
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    boost::system::error_code listen_ec;
    impl_->socket.listen(1, listen_ec);
    if (listen_ec)
    {
        const auto msg = DAS_FMT_NS::format(
            "listen failed: path = {}, error = {}",
            socket_path,
            listen_ec.message());
        DAS_LOG_ERROR(msg.c_str());
        impl_->socket.close();
        unlink(socket_path.c_str());
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    boost::system::error_code                   accept_ec;
    boost::asio::local::stream_protocol::socket client_socket =
        impl_->socket.accept(accept_ec);

    if (accept_ec)
    {
        const auto msg = DAS_FMT_NS::format(
            "accept failed: path = {}, error = {}",
            socket_path,
            accept_ec.message());
        DAS_LOG_ERROR(msg.c_str());
        impl_->socket.close();
        unlink(socket_path.c_str());
        return DAS_E_IPC_HANDSHAKE_FAILED;
    }

    impl_->socket = std::move(client_socket);
    impl_->is_connected = true;
    return DAS_S_OK;
}

DasResult UnixAsyncIpcTransport::ConnectToUnixSocket(
    const std::string& socket_path)
{
    boost::system::error_code ec;

    impl_->socket.open(ec);
    if (ec)
    {
        const auto msg = DAS_FMT_NS::format(
            "socket open failed: path = {}, error = {}",
            socket_path,
            ec.message());
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    boost::system::error_code connect_ec;
    impl_->socket.connect(
        boost::asio::local::stream_protocol::endpoint(
            reinterpret_cast<struct sockaddr*>(&addr),
            sizeof(addr)),
        connect_ec);

    if (connect_ec)
    {
        const auto msg = DAS_FMT_NS::format(
            "connect failed: path = {}, error = {}",
            socket_path,
            connect_ec.message());
        DAS_LOG_ERROR(msg.c_str());
        impl_->socket.close();
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    impl_->is_connected = true;
    return DAS_S_OK;
}

boost::asio::awaitable<std::variant<DasResult, AsyncIpcMessage>>
UnixAsyncIpcTransport::ReceiveCoroutine()
{
    co_await boost::asio::async_read(
        impl_->socket,
        boost::asio::buffer(impl_->header_buffer),
        boost::asio::use_awaitable);

    HeaderValidationResult validation_error;
    auto validated_header = ValidatedIPCMessageHeader::Deserialize(
        impl_->header_buffer.data(),
        impl_->header_buffer.size(),
        &validation_error);

    if (!validated_header.has_value())
    {
        DAS_LOG_ERROR(
            DAS_FMT_NS::format(
                "Header validation failed: {}",
                validation_error.message)
                .c_str());
        co_return DAS_E_IPC_INVALID_MESSAGE;
    }

    auto header = *validated_header;

    const auto body_size = header.Raw().body_size;
    if (body_size > impl_->max_message_size)
    {
        DAS_LOG_ERROR(
            DAS_FMT_NS::format(
                "Message body too large: body_size = {}, max = {}",
                body_size,
                impl_->max_message_size)
                .c_str());
        co_return DAS_E_IPC_INVALID_MESSAGE;
    }

    impl_->body_buffer.resize(body_size);

    if (body_size > 0)
    {
        co_await boost::asio::async_read(
            impl_->socket,
            boost::asio::buffer(impl_->body_buffer),
            boost::asio::use_awaitable);
    }

    if (header.Raw().flags & kFlagLargeMessage)
    {
        if (impl_->shared_memory_pool == nullptr)
        {
            DAS_LOG_ERROR(
                "Large message received but shared memory pool not set");
            co_return DAS_E_IPC_INVALID_MESSAGE;
        }

        if (impl_->body_buffer.size() < sizeof(uint64_t))
        {
            DAS_LOG_ERROR("Large message handle missing");
            co_return DAS_E_IPC_INVALID_MESSAGE;
        }

        uint64_t handle;
        std::memcpy(&handle, impl_->body_buffer.data(), sizeof(uint64_t));

        SharedMemoryBlock shm_block;
        const auto        result =
            impl_->shared_memory_pool->GetBlockByHandle(handle, shm_block);
        if (result != DAS_S_OK)
        {
            DAS_LOG_ERROR(
                DAS_FMT_NS::format(
                    "Failed to get shared memory block for handle = {}",
                    handle)
                    .c_str());
            co_return DAS_E_IPC_INVALID_MESSAGE;
        }

        std::vector<uint8_t> large_body(shm_block.size);
        std::memcpy(large_body.data(), shm_block.data, shm_block.size);

        impl_->shared_memory_pool->Deallocate(handle);

        co_return AsyncIpcMessage{header, std::move(large_body)};
    }

    co_return AsyncIpcMessage{header, impl_->body_buffer};
}

boost::asio::awaitable<DasResult> UnixAsyncIpcTransport::SendCoroutine(
    const ValidatedIPCMessageHeader& header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    co_await boost::asio::async_write(
        impl_->socket,
        boost::asio::buffer(
            static_cast<const IPCMessageHeader*>(header),
            sizeof(IPCMessageHeader)),
        boost::asio::use_awaitable);

    if (body_size > 0)
    {
        co_await boost::asio::async_write(
            impl_->socket,
            boost::asio::buffer(body, body_size),
            boost::asio::use_awaitable);
    }

    co_return DAS_S_OK;
}

auto UnixAsyncIpcTransport::Receive()
{
    return boost::asio::co_spawn(
        impl_->io_context,
        ReceiveCoroutine(),
        asioexec::use_sender);
}

auto UnixAsyncIpcTransport::Send(
    const ValidatedIPCMessageHeader& header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    return boost::asio::co_spawn(
        impl_->io_context,
        SendCoroutine(header, body, body_size),
        asioexec::use_sender);
}

DAS_CORE_IPC_NS_END

#endif // !_WIN32
