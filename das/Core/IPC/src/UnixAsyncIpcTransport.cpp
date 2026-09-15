#include <das/Core/IPC/UnixAsyncIpcTransport.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <cstring>
#include <das/Core/IPC/AsyncIpcTransport.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcFrameReceiveLoop.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/Utils/PathText.h>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>
#include <filesystem>
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
    auto temp_dir =
        std::filesystem::temp_directory_path() / Das::Utils::PathFromUtf8(base);
    return Das::Utils::PathToUtf8(temp_dir);
}

UnixAsyncIpcTransport::UnixAsyncIpcTransport(
    boost::asio::io_context& io_context)
    : io_context_(io_context), send_mutex_(io_context_.get()),
      socket_(io_context_.get())
{
}

UnixAsyncIpcTransport::~UnixAsyncIpcTransport() { Cleanup(); }

UnixAsyncIpcTransport::UnixAsyncIpcTransport(
    UnixAsyncIpcTransport&& other) noexcept
    : acceptor_(std::move(other.acceptor_)), io_context_(other.io_context_),
      send_mutex_(std::move(other.send_mutex_)),
      socket_(std::move(other.socket_)),
      endpoint_name_(std::move(other.endpoint_name_)),
      is_server_(std::exchange(other.is_server_, false)),
      is_connected_(std::exchange(other.is_connected_, false)),
      max_message_size_(other.max_message_size_),
      shared_memory_pool_(other.shared_memory_pool_),
      frame_assembler_(std::move(other.frame_assembler_)),
      read_stage_(std::move(other.read_stage_))
{
}

UnixAsyncIpcTransport& UnixAsyncIpcTransport::operator=(
    UnixAsyncIpcTransport&& other) noexcept
{
    if (this != &other)
    {
        Cleanup();
        acceptor_ = std::move(other.acceptor_);
        io_context_ = other.io_context_;
        send_mutex_ = std::move(other.send_mutex_);
        socket_ = std::move(other.socket_);
        endpoint_name_ = std::move(other.endpoint_name_);
        is_server_ = std::exchange(other.is_server_, false);
        is_connected_ = std::exchange(other.is_connected_, false);
        max_message_size_ = other.max_message_size_;
        shared_memory_pool_ = other.shared_memory_pool_;
        frame_assembler_ = std::move(other.frame_assembler_);
        read_stage_ = std::move(other.read_stage_);
    }
    return *this;
}

void UnixAsyncIpcTransport::Cleanup()
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

boost::asio::awaitable<DasResult> UnixAsyncIpcTransport::InitializeAsync(
    const std::string& read_endpoint,
    const std::string& write_endpoint,
    bool               is_server,
    size_t             max_message_size,
    SharedMemoryPool*  pool)
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
    shared_memory_pool_ = pool;
    frame_assembler_.SetMaxBodySize(max_message_size);

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
        io_context_.get());

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

    const boost::system::error_code accept_ec;
    auto                            client_socket =
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
    // 单次读帧（两平台共享一份循环逻辑）：read_some 收多少吃多少，
    // 重组器吃帧，尾段（SHM/日志）走 FinalizeReceivedFrame
    co_return co_await ReceiveFrameLoop(
        socket_,
        frame_assembler_,
        read_stage_,
        shared_memory_pool_);
}

boost::asio::awaitable<DasResult> UnixAsyncIpcTransport::SendCoroutine(
    const ValidatedIPCMessageHeader& header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    // Lock() 返回 bool：true=cancelled，false=正常获取锁
    const bool cancelled = co_await send_mutex_.Lock();
    if (cancelled)
    {
        co_return DAS_E_IPC_CONNECTION_LOST; // 不访问任何 transport 成员
    }

    DasResult result = DAS_S_OK;

    // SHM 分流路径：大消息必须走 SHM；无 pool 或 Allocate
    // 失败则拒绝（不降级直写—— 直写超大 body 会被对端 64KB
    // 上限拒绝，或阻塞管道）
    if (body_size >= LARGE_MESSAGE_THRESHOLD)
    {
        // 捎带协议防御（D-12 互斥）：SHM 分流重建会静默清零 reserved——
        // bit1 置位的帧禁止进入 SHM 路径（合法发送端拼装规则永不产生）
        if (header.GetFlags() & MessageFlags::RESERVED_AS_SIZE)
        {
            DAS_CORE_LOG_WARN(
                "SendCoroutine: frame with RESERVED_AS_SIZE flag must not "
                "take the SHM path: flags = {}, body_size = {}",
                header.GetFlags(),
                body_size);
            send_mutex_.Unlock();
            co_return DAS_E_IPC_INVALID_MESSAGE;
        }

        if (shared_memory_pool_ == nullptr)
        {
            DAS_CORE_LOG_WARN(
                "SendCoroutine: large message (size={}) but no SHM pool set; rejecting",
                body_size);
            send_mutex_.Unlock();
            co_return DAS_E_IPC_MESSAGE_TOO_LARGE;
        }
        SharedMemoryBlock blk{};
        if (DAS::IsOk(shared_memory_pool_->Allocate(body_size + 8, blk)))
        {
            ShmBlockGuard guard(
                shared_memory_pool_,
                blk.data,
                blk.size,
                blk.handle);
            uint64_t real_size = static_cast<uint64_t>(body_size);
            std::memcpy(guard.Data(), &real_size, 8); // 块首 8B in-band size
            std::memcpy(
                static_cast<uint8_t*>(guard.Data()) + 8,
                body,
                body_size);

            // 重建 header：header_flags |= LARGE_MESSAGE_VIA_SHM, body_size = 8
            auto orig = header.Raw();
            auto validated_shm_header =
                IPCMessageHeaderBuilder()
                    .SetMessageType(static_cast<MessageType>(orig.message_type))
                    .SetHeaderFlags(
                        orig.header_flags | HeaderFlags::LARGE_MESSAGE_VIA_SHM)
                    .SetCallId(orig.call_id)
                    .SetSourceSessionId(orig.source_session_id)
                    .SetTargetSessionId(orig.target_session_id)
                    .SetFlags(orig.flags)
                    .SetInterfaceId(orig.interface_id)
                    .SetErrorCode(orig.error_code)
                    .SetBodySize(sizeof(uint64_t))
                    .Build();

            uint64_t net_handle = guard.Handle();
            const std::array<boost::asio::const_buffer, 2> shm_buffers = {
                boost::asio::const_buffer{
                    static_cast<const IPCMessageHeader*>(validated_shm_header),
                    sizeof(IPCMessageHeader)},
                boost::asio::const_buffer{&net_handle, sizeof(uint64_t)}};

            try
            {
                co_await boost::asio::async_write(
                    socket_,
                    shm_buffers,
                    boost::asio::use_awaitable);
                guard
                    .Disown(); // 成功：转移所有权给 reader（经 wire 的 handle）
                send_mutex_.Unlock();
                co_return DAS_S_OK;
            }
            catch (const std::exception& e)
            {
                DAS_CORE_LOG_ERROR(
                    "SendCoroutine: SHM async_write failed: {}",
                    ToString(e.what()));
                // guard 析构自动 Deallocate
                send_mutex_.Unlock();
                co_return DAS_E_IPC_SEND_FAILED;
            }
        }
        // Allocate 失败：拒绝（不降级直写，否则超大 body 会撞对端 64KB 上限）
        DAS_CORE_LOG_WARN(
            "SendCoroutine: SHM allocation failed for large message (size={}); rejecting",
            body_size);
        send_mutex_.Unlock();
        co_return DAS_E_IPC_MESSAGE_TOO_LARGE;
    }

    // 直写路径（小消息 / pool 空 / Allocate 失败）：scatter-gather
    const auto* raw_header = static_cast<const IPCMessageHeader*>(header);
    const std::array<boost::asio::const_buffer, 2> buffers = {
        boost::asio::const_buffer{raw_header, sizeof(IPCMessageHeader)},
        boost::asio::const_buffer{body, body_size}};

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

boost::asio::awaitable<DasResult> UnixAsyncIpcTransport::SendScatter(
    const ValidatedIPCMessageHeader& header,
    std::vector<SendSegment>&&       segments)
{
    // 段列表 move 进协程帧局部持有：SendSegment 非拥有，段与备存储的
    // 生命周期契约见 SendSegment 定义（存活至 async_write 完成）
    std::vector<SendSegment> owned_segments = std::move(segments);

    // 入口防呆：scatter 只承接直写路径的小帧拼装，总长（header + 段总和）
    // 达到 LARGE_MESSAGE_THRESHOLD 即拒发——SHM 大消息路径不接纳
    // scatter，且无降级直写（拼装越界由调用方按阈值决策，传输层兜底）
    size_t total_size = sizeof(IPCMessageHeader);
    for (const SendSegment& segment : owned_segments)
    {
        total_size += segment.size;
    }
    if (total_size >= LARGE_MESSAGE_THRESHOLD)
    {
        DAS_CORE_LOG_WARN(
            "scatter frame assembled at or over threshold: total_size = {}, "
            "segment_count = {}; rejecting",
            total_size,
            owned_segments.size());
        co_return DAS_E_IPC_MESSAGE_TOO_LARGE;
    }

    // Lock() 返回 bool：true=cancelled，false=正常获取锁
    const bool cancelled = co_await send_mutex_.Lock();
    if (cancelled)
    {
        co_return DAS_E_IPC_CONNECTION_LOST; // 不访问任何 transport 成员
    }

    DasResult result = DAS_S_OK;

    // 直写路径 N 段同型扩展：header + 各段构造 const_buffer 序列，一次
    // async_write
    std::vector<boost::asio::const_buffer> buffers;
    buffers.reserve(owned_segments.size() + 1);
    const auto* raw_header = static_cast<const IPCMessageHeader*>(header);
    buffers.emplace_back(raw_header, sizeof(IPCMessageHeader));
    for (const SendSegment& segment : owned_segments)
    {
        if (segment.size > 0)
        {
            buffers.emplace_back(segment.data, segment.size);
        }
    }

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
            "scatter async_write failed: {}",
            ToString(e.what()));
        result = DAS_E_IPC_SEND_FAILED;
    }

    send_mutex_.Unlock();
    co_return result;
}

DAS_CORE_IPC_NS_END
