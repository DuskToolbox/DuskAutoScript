#ifndef _WIN32

#include <das/Core/IPC/UnixAsyncIpcTransport.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <cstring>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/DasApi.h>
#include <das/Utils/StringUtils.h>
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

UnixAsyncIpcTransport::UnixAsyncIpcTransport(
    boost::asio::io_context& io_context)
    : io_context_(io_context), socket_(io_context),
      header_buffer_(sizeof(IPCMessageHeader))
{
}

UnixAsyncIpcTransport::~UnixAsyncIpcTransport() { Uninitialize(); }

void UnixAsyncIpcTransport::Uninitialize()
{
    boost::system::error_code ec;

    if (socket_.is_open())
    {
        socket_.close(ec);
    }

    if (acceptor_)
    {
        acceptor_->close(ec);
        acceptor_.reset();
    }

    if (is_server_ && !endpoint_name_.empty())
    {
        unlink(endpoint_name_.c_str());
    }

    is_connected_ = false;
    is_server_ = false;
}

std::unique_ptr<UnixAsyncIpcTransport>
UnixAsyncIpcTransport::CreateUninitialized(boost::asio::io_context& io_context)
{
    return std::unique_ptr<UnixAsyncIpcTransport>(
        new UnixAsyncIpcTransport(io_context));
}

boost::asio::awaitable<
    DAS::Utils::Expected<std::unique_ptr<UnixAsyncIpcTransport>>>
UnixAsyncIpcTransport::CreateAsync(
    boost::asio::io_context& io_context,
    const std::string&       read_endpoint,
    const std::string&       write_endpoint,
    bool                     is_server,
    size_t                   max_message_size)
{
    auto instance = std::unique_ptr<UnixAsyncIpcTransport>(
        new UnixAsyncIpcTransport(io_context));

    auto result = co_await instance->InitializeAsync(
        read_endpoint,
        write_endpoint,
        is_server,
        max_message_size);

    if (result != DAS_S_OK)
    {
        co_return DAS::Utils::MakeUnexpected(result);
    }

    co_return instance;
}

DasResult UnixAsyncIpcTransport::Initialize(
    const std::string& read_endpoint,
    const std::string& write_endpoint,
    bool               is_server,
    size_t             max_message_size)
{
    (void)write_endpoint;

    if (is_connected_)
    {
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    endpoint_name_ = read_endpoint;
    is_server_ = is_server;
    max_message_size_ = max_message_size;
    body_buffer_.reserve(max_message_size);

    if (is_server)
    {
        boost::system::error_code ec;

        acceptor_ =
            std::make_unique<boost::asio::local::stream_protocol::acceptor>(
                io_context_);

        socket_.open(ec);
        if (ec)
        {
            const auto msg = DAS_FMT_NS::format(
                "socket open failed: path = {}, error = {}",
                read_endpoint,
                ToString(ec.message()));
            DAS_LOG_ERROR(msg.c_str());
            return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
        }

        unlink(read_endpoint.c_str());

        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(
            addr.sun_path,
            read_endpoint.c_str(),
            sizeof(addr.sun_path) - 1);

        boost::system::error_code bind_ec;
        acceptor_->bind(
            boost::asio::local::stream_protocol::endpoint(
                reinterpret_cast<struct sockaddr*>(&addr),
                sizeof(addr)),
            bind_ec);

        if (bind_ec)
        {
            const auto msg = DAS_FMT_NS::format(
                "bind failed: path = {}, error = {}",
                read_endpoint,
                ToString(bind_ec.message()));
            DAS_LOG_ERROR(msg.c_str());
            socket_.close();
            return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
        }

        boost::system::error_code listen_ec;
        acceptor_->listen(1, listen_ec);
        if (listen_ec)
        {
            const auto msg = DAS_FMT_NS::format(
                "listen failed: path = {}, error = {}",
                read_endpoint,
                ToString(listen_ec.message()));
            DAS_LOG_ERROR(msg.c_str());
            socket_.close();
            unlink(read_endpoint.c_str());
            return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
        }

        boost::system::error_code accept_ec;
        auto                      client_socket = acceptor_->accept(accept_ec);

        if (accept_ec)
        {
            const auto msg = DAS_FMT_NS::format(
                "accept failed: path = {}, error = {}",
                read_endpoint,
                ToString(accept_ec.message()));
            DAS_LOG_ERROR(msg.c_str());
            socket_.close();
            acceptor_->close();
            unlink(read_endpoint.c_str());
            return DAS_E_IPC_HANDSHAKE_FAILED;
        }

        socket_ = std::move(client_socket);
        acceptor_->close();
        is_connected_ = true;
        return DAS_S_OK;
    }

    return DAS_S_OK;
}

boost::asio::awaitable<DasResult> UnixAsyncIpcTransport::InitializeAsync(
    const std::string& read_endpoint,
    const std::string& write_endpoint,
    bool               is_server,
    size_t             max_message_size)
{
    (void)write_endpoint;

    if (is_connected_)
    {
        co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    endpoint_name_ = read_endpoint;
    is_server_ = is_server;
    max_message_size_ = max_message_size;
    body_buffer_.reserve(max_message_size);

    if (is_server)
    {
        auto result = co_await CreateUnixSocketAsync(read_endpoint);
        co_return result;
    }

    co_return DAS_S_OK;
}

boost::asio::awaitable<DasResult> UnixAsyncIpcTransport::CreateUnixSocketAsync(
    const std::string& socket_path)
{
    boost::system::error_code ec;

    acceptor_ = std::make_unique<boost::asio::local::stream_protocol::acceptor>(
        io_context_);

    socket_.open(ec);
    if (ec)
    {
        const auto msg = DAS_FMT_NS::format(
            "socket open failed: path = {}, error = {}",
            socket_path,
            ToString(ec.message()));
        DAS_LOG_ERROR(msg.c_str());
        co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    unlink(socket_path.c_str());

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    boost::system::error_code bind_ec;
    acceptor_->bind(
        boost::asio::local::stream_protocol::endpoint(
            reinterpret_cast<struct sockaddr*>(&addr),
            sizeof(addr)),
        bind_ec);

    if (bind_ec)
    {
        const auto msg = DAS_FMT_NS::format(
            "bind failed: path = {}, error = {}",
            socket_path,
            ToString(bind_ec.message()));
        DAS_LOG_ERROR(msg.c_str());
        socket_.close();
        co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    boost::system::error_code listen_ec;
    acceptor_->listen(1, listen_ec);
    if (listen_ec)
    {
        const auto msg = DAS_FMT_NS::format(
            "listen failed: path = {}, error = {}",
            socket_path,
            ToString(listen_ec.message()));
        DAS_LOG_ERROR(msg.c_str());
        socket_.close();
        unlink(socket_path.c_str());
        co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    boost::system::error_code accept_ec;
    auto                      client_socket =
        co_await acceptor_->async_accept(boost::asio::use_awaitable);

    if (accept_ec)
    {
        const auto msg = DAS_FMT_NS::format(
            "accept failed: path = {}, error = {}",
            socket_path,
            ToString(accept_ec.message()));
        DAS_LOG_ERROR(msg.c_str());
        socket_.close();
        acceptor_->close();
        unlink(socket_path.c_str());
        co_return DAS_E_IPC_HANDSHAKE_FAILED;
    }

    socket_ = std::move(client_socket);
    acceptor_->close();
    is_connected_ = true;
    co_return DAS_S_OK;
}

boost::asio::awaitable<DasResult>
UnixAsyncIpcTransport::ConnectToUnixSocketAsync(const std::string& socket_path)
{
    boost::system::error_code ec;

    socket_.open(ec);
    if (ec)
    {
        const auto msg = DAS_FMT_NS::format(
            "socket open failed: path = {}, error = {}",
            socket_path,
            ToString(ec.message()));
        DAS_LOG_ERROR(msg.c_str());
        co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    co_await socket_.async_connect(
        boost::asio::local::stream_protocol::endpoint(
            reinterpret_cast<struct sockaddr*>(&addr),
            sizeof(addr)),
        boost::asio::use_awaitable);

    is_connected_ = true;
    co_return DAS_S_OK;
}

DasResult UnixAsyncIpcTransport::Connect(
    const std::string& read_endpoint,
    const std::string& write_endpoint)
{
    (void)write_endpoint;

    if (is_connected_)
    {
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    endpoint_name_ = read_endpoint;

    boost::system::error_code ec;

    socket_.open(ec);
    if (ec)
    {
        const auto msg = DAS_FMT_NS::format(
            "socket open failed: path = {}, error = {}",
            read_endpoint,
            ToString(ec.message()));
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(
        addr.sun_path,
        read_endpoint.c_str(),
        sizeof(addr.sun_path) - 1);

    boost::system::error_code connect_ec;
    socket_.connect(
        boost::asio::local::stream_protocol::endpoint(
            reinterpret_cast<struct sockaddr*>(&addr),
            sizeof(addr)),
        connect_ec);

    if (connect_ec)
    {
        const auto msg = DAS_FMT_NS::format(
            "connect failed: path = {}, error = {}",
            read_endpoint,
            ToString(connect_ec.message()));
        DAS_LOG_ERROR(msg.c_str());
        socket_.close();
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    is_connected_ = true;
    return DAS_S_OK;
}

bool UnixAsyncIpcTransport::IsConnected() const { return is_connected_; }

std::string UnixAsyncIpcTransport::GetEndpointName() const
{
    return endpoint_name_;
}

void UnixAsyncIpcTransport::SetSharedMemoryPool(SharedMemoryPool* pool)
{
    shared_memory_pool_ = pool;
}

boost::asio::awaitable<std::variant<DasResult, AsyncIpcMessage>>
UnixAsyncIpcTransport::ReceiveCoroutine()
{
    co_await boost::asio::async_read(
        socket_,
        boost::asio::buffer(header_buffer_),
        boost::asio::use_awaitable);

    HeaderValidationResult validation_error;
    auto validated_header = ValidatedIPCMessageHeader::Deserialize(
        header_buffer_.data(),
        header_buffer_.size(),
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
    if (body_size > max_message_size_)
    {
        DAS_LOG_ERROR(
            DAS_FMT_NS::format(
                "Message body too large: body_size = {}, max = {}",
                body_size,
                max_message_size_)
                .c_str());
        co_return DAS_E_IPC_INVALID_MESSAGE;
    }

    body_buffer_.resize(body_size);

    if (body_size > 0)
    {
        co_await boost::asio::async_read(
            socket_,
            boost::asio::buffer(body_buffer_),
            boost::asio::use_awaitable);
    }

    if (header.Raw().flags & kFlagLargeMessage)
    {
        if (shared_memory_pool_ == nullptr)
        {
            DAS_LOG_ERROR(
                "Large message received but shared memory pool not set");
            co_return DAS_E_IPC_INVALID_MESSAGE;
        }

        if (body_buffer_.size() < sizeof(uint64_t))
        {
            DAS_LOG_ERROR("Large message handle missing");
            co_return DAS_E_IPC_INVALID_MESSAGE;
        }

        uint64_t handle;
        std::memcpy(&handle, body_buffer_.data(), sizeof(uint64_t));

        SharedMemoryBlock shm_block;
        const auto        result =
            shared_memory_pool_->GetBlockByHandle(handle, shm_block);
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

        shared_memory_pool_->Deallocate(handle);

        co_return AsyncIpcMessage{header, std::move(large_body)};
    }

    co_return AsyncIpcMessage{header, body_buffer_};
}

boost::asio::awaitable<DasResult> UnixAsyncIpcTransport::SendCoroutine(
    const ValidatedIPCMessageHeader& header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    co_await boost::asio::async_write(
        socket_,
        boost::asio::buffer(
            static_cast<const IPCMessageHeader*>(header),
            sizeof(IPCMessageHeader)),
        boost::asio::use_awaitable);

    if (body_size > 0)
    {
        co_await boost::asio::async_write(
            socket_,
            boost::asio::buffer(body, body_size),
            boost::asio::use_awaitable);
    }

    co_return DAS_S_OK;
}

DAS_CORE_IPC_NS_END

#endif // !_WIN32
