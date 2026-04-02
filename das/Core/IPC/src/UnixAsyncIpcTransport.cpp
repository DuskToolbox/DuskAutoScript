#include <das/Core/IPC/UnixAsyncIpcTransport.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <filesystem>
#include <cstring>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef DAS_WINDOWS
// MinGW 的 <afunix.h> 有 bug (缺少 windows.h 导致 ADDRESS_FAMILY 未定义)
// 用 _AFUNIX_ 阻止包含损坏头文件，手动提供所需定义
#ifndef _MSC_VER
#define _AFUNIX_
#endif
#include <windows.h> // DeleteFileA
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#include <afunix.h>
#else
// MinGW workaround: 手动定义 sockaddr_un
#include <string.h>
#define UNIX_PATH_MAX 108
struct sockaddr_un
{
    unsigned short sun_family;
    char           sun_path[UNIX_PATH_MAX];
};
#endif
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#ifdef DAS_WINDOWS
#define DAS_UNLINK(path) DeleteFileA(path)
#else
#define DAS_UNLINK(path) unlink(path)
#endif

#include <das/Core/IPC/Config.h>
#include <variant>

DAS_CORE_IPC_NS_BEGIN

inline constexpr uint16_t kFlagLargeMessage = 0x01;

// 归一化端点名称：剥离 _m2h/_h2m 方向后缀，加上 temp 目录前缀得到绝对路径
// 避免相对路径因进程 CWD 不同导致跨进程连接失败
static std::string NormalizeUnixEndpoint(const std::string& endpoint)
{
    std::string base = endpoint;

    auto m2h_pos = endpoint.rfind("_m2h");
    if (m2h_pos != std::string::npos)
    {
        base = endpoint.substr(0, m2h_pos);
    }
    else
    {
        auto h2m_pos = endpoint.rfind("_h2m");
        if (h2m_pos != std::string::npos)
        {
            base = endpoint.substr(0, h2m_pos);
        }
    }

    // 拼接绝对路径: temp_directory_path() / base
    // Linux: /tmp/das_ipc_xxx_yyy
    // Windows: C:\\Users\\xxx\\AppData\\Local\\Temp\\das_ipc_xxx_yyy
    auto temp_dir = std::filesystem::temp_directory_path() / base;
    return temp_dir.string();
}

UnixAsyncIpcTransport::UnixAsyncIpcTransport(
    boost::asio::io_context& io_context)
    : io_context_(io_context), send_mutex_(io_context), socket_(io_context),
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
        DAS_UNLINK(endpoint_name_.c_str());
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

    try
    {
        auto result = co_await instance->InitializeAsync(
            read_endpoint,
            write_endpoint,
            is_server,
            max_message_size);

        if (result != DAS_S_OK)
        {
            co_return DAS::Utils::MakeUnexpected(result);
        }
    }
    catch (const boost::system::system_error& e)
    {
        const auto msg = DAS_FMT_NS::format(
            "CreateAsync failed (system_error): path = {}, error = {}",
            read_endpoint,
            ToString(e.what()));
        DAS_LOG_ERROR(msg.c_str());
        co_return DAS::Utils::MakeUnexpected(DAS_E_IPC_MESSAGE_QUEUE_FAILED);
    }
    catch (const std::exception& e)
    {
        const auto msg = DAS_FMT_NS::format(
            "CreateAsync failed: path = {}, error = {}",
            read_endpoint,
            ToString(e.what()));
        DAS_LOG_ERROR(msg.c_str());
        co_return DAS::Utils::MakeUnexpected(DAS_E_IPC_MESSAGE_QUEUE_FAILED);
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

    endpoint_name_ = NormalizeUnixEndpoint(read_endpoint);
    is_server_ = is_server;
    max_message_size_ = max_message_size;
    body_buffer_.reserve(max_message_size);

    if (is_server)
    {
        boost::system::error_code ec;

        acceptor_ =
            std::make_unique<boost::asio::local::stream_protocol::acceptor>(
                io_context_);

        acceptor_->open(boost::asio::local::stream_protocol(), ec);
        if (ec)
        {
            const auto msg = DAS_FMT_NS::format(
                "acceptor open failed: path = {}, error = {}",
                read_endpoint,
                ToString(ec.message()));
            DAS_LOG_ERROR(msg.c_str());
            return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
        }

        socket_.open(boost::asio::local::stream_protocol(), ec);
        if (ec)
        {
            const auto msg = DAS_FMT_NS::format(
                "socket open failed: path = {}, error = {}",
                read_endpoint,
                ToString(ec.message()));
            DAS_LOG_ERROR(msg.c_str());
            return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
        }

        auto normalized_path = NormalizeUnixEndpoint(read_endpoint);
        DAS_UNLINK(normalized_path.c_str());

        boost::system::error_code bind_ec;
        acceptor_->bind(
            boost::asio::local::stream_protocol::endpoint(normalized_path),
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
            DAS_UNLINK(NormalizeUnixEndpoint(read_endpoint).c_str());
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
            DAS_UNLINK(NormalizeUnixEndpoint(read_endpoint).c_str());
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

    DAS_CORE_LOG_INFO(
        "InitializeAsync: is_server = {}, read_endpoint = {}, normalized = {}",
        is_server,
        read_endpoint,
        NormalizeUnixEndpoint(read_endpoint));

    if (is_connected_)
    {
        co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    endpoint_name_ = NormalizeUnixEndpoint(read_endpoint);
    is_server_ = is_server;
    max_message_size_ = max_message_size;
    body_buffer_.reserve(max_message_size);

    if (is_server)
    {
        auto result = co_await CreateUnixSocketAsync(
            NormalizeUnixEndpoint(read_endpoint));
        co_return result;
    }
    else
    {
        auto result = co_await ConnectToUnixSocketAsync(
            NormalizeUnixEndpoint(read_endpoint));
        co_return result;
    }
}

boost::asio::awaitable<DasResult> UnixAsyncIpcTransport::CreateUnixSocketAsync(
    const std::string& socket_path)
{
    boost::system::error_code ec;

    acceptor_ = std::make_unique<boost::asio::local::stream_protocol::acceptor>(
        io_context_);

    acceptor_->open(boost::asio::local::stream_protocol(), ec);
    if (ec)
    {
        const auto msg = DAS_FMT_NS::format(
            "acceptor open failed: path = {}, error = {}",
            socket_path,
            ToString(ec.message()));
        DAS_LOG_ERROR(msg.c_str());
        co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    socket_.open(boost::asio::local::stream_protocol(), ec);
    if (ec)
    {
        const auto msg = DAS_FMT_NS::format(
            "socket open failed: path = {}, error = {}",
            socket_path,
            ToString(ec.message()));
        DAS_LOG_ERROR(msg.c_str());
        co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    DAS_UNLINK(socket_path.c_str());

    boost::system::error_code bind_ec;
    acceptor_->bind(
        boost::asio::local::stream_protocol::endpoint(socket_path),
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
        DAS_UNLINK(socket_path.c_str());
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
        DAS_UNLINK(socket_path.c_str());
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
    auto                      ex = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(ex);

    constexpr int  max_retries = 100; // 100 * 10ms = 1 second timeout
    constexpr auto retry_interval = std::chrono::milliseconds(10);

    for (int retry = 0; retry < max_retries; ++retry)
    {
        boost::system::error_code open_ec;

        if (!socket_.is_open())
        {
            socket_.open(boost::asio::local::stream_protocol(), open_ec);
            if (open_ec)
            {
                const auto msg = DAS_FMT_NS::format(
                    "socket open failed: path = {}, error = {}",
                    socket_path,
                    ToString(open_ec.message()));
                DAS_LOG_ERROR(msg.c_str());
                co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
            }
        }

        boost::system::error_code connect_ec;
        co_await socket_.async_connect(
            boost::asio::local::stream_protocol::endpoint(socket_path),
            boost::asio::redirect_error(
                boost::asio::use_awaitable,
                connect_ec));

        if (!connect_ec)
        {
            is_connected_ = true;
            co_return DAS_S_OK;
        }

        // 关闭失败的 socket 以便重试
        socket_.close();

        // 检查是否值得重试
        // ECONNREFUSED: 服务端还没 listen
        // ENOENT: socket 文件还不存在
        if (connect_ec == boost::system::errc::connection_refused
            || connect_ec == boost::system::errc::no_such_file_or_directory)
        {
            if (retry < max_retries - 1)
            {
                timer.expires_after(retry_interval);
                co_await timer.async_wait(boost::asio::use_awaitable);
                continue;
            }
        }

        // 其他错误或最后一次重试失败，直接报错
        const auto msg = DAS_FMT_NS::format(
            "connect failed: path = {}, error = {}",
            socket_path,
            ToString(connect_ec.message()));
        DAS_LOG_ERROR(msg.c_str());
        co_return DAS_E_IPC_CONNECTION_LOST;
    }

    const auto msg = DAS_FMT_NS::format(
        "Connect to unix socket timeout: path = {}, max_retries = {}",
        socket_path,
        max_retries);
    DAS_LOG_ERROR(msg.c_str());
    co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
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

    endpoint_name_ = NormalizeUnixEndpoint(read_endpoint);

    boost::system::error_code ec;

    socket_.open(boost::asio::local::stream_protocol(), ec);
    if (ec)
    {
        const auto msg = DAS_FMT_NS::format(
            "socket open failed: path = {}, error = {}",
            read_endpoint,
            ToString(ec.message()));
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    boost::system::error_code connect_ec;
    socket_.connect(
        boost::asio::local::stream_protocol::endpoint(endpoint_name_),
        connect_ec);

    if (connect_ec)
    {
        const auto msg = DAS_FMT_NS::format(
            "connect failed: path = {}, error = {}",
            read_endpoint,
            ToString(connect_ec.message()));
        DAS_LOG_ERROR(msg.c_str());
        socket_.close();
        return DAS_E_IPC_CONNECTION_LOST;
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
    // Lock() 返回 bool：true=cancelled，false=正常获取锁
    bool cancelled = co_await send_mutex_.Lock();
    if (cancelled)
    {
        co_return DAS_E_IPC_CONNECTION_LOST; // 不访问任何 transport 成员
    }

    // scatter-gather: 零拷贝，header 和 body 分开传递
    const auto* raw_header = static_cast<const IPCMessageHeader*>(header);
    std::array<boost::asio::const_buffer, 2> buffers = {
        boost::asio::const_buffer{raw_header, sizeof(IPCMessageHeader)},
        boost::asio::const_buffer{body, body_size}};

    DasResult result = DAS_S_OK;
    try
    {
        co_await boost::asio::async_write(
            socket_,
            buffers,
            boost::asio::use_awaitable);
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_ERROR(
            "SendCoroutine: async_write failed: {}",
            ToString(e.what()));
        result = DAS_E_IPC_SEND_FAILED;
    }

    send_mutex_.Unlock();
    co_return result;
}

DAS_CORE_IPC_NS_END
