#ifdef _WIN32

#include <das/Core/IPC/Win32AsyncIpcTransport.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <cstring>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <windows.h>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

inline constexpr uint16_t         FLAG_LARGE_MESSAGE = 0x01;
inline constexpr std::string_view PIPE_PREFIX = R"(\\.\pipe\)";

namespace {
// RAII 包装器 - 确保 HANDLE 清理
class ScopedHandle
{
    HANDLE handle_ = INVALID_HANDLE_VALUE;

public:
    explicit ScopedHandle(HANDLE h = INVALID_HANDLE_VALUE) : handle_(h) {}
    ~ScopedHandle()
    {
        if (Valid())
            CloseHandle(handle_);
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& o) noexcept : handle_(o.handle_)
    {
        o.handle_ = INVALID_HANDLE_VALUE;
    }

    ScopedHandle& operator=(ScopedHandle&& o) noexcept
    {
        if (this != &o)
        {
            Reset();
            handle_   = o.handle_;
            o.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    bool   Valid() const { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }
    HANDLE Get() const { return handle_; }
    HANDLE* Ptr() { return &handle_; }
    void   Reset(HANDLE h = INVALID_HANDLE_VALUE)
    {
        if (Valid())
            CloseHandle(handle_);
        handle_ = h;
    }
    HANDLE Release()
    {
        auto h   = handle_;
        handle_  = INVALID_HANDLE_VALUE;
        return h;
    }
};
} // namespace

// 私有构造函数（由 Create() 工厂函数调用）
Win32AsyncIpcTransport::Win32AsyncIpcTransport(
    boost::asio::io_context& io_context)
    : io_context_(io_context), read_pipe_(io_context), write_pipe_(io_context)
{
}

// 工厂函数实现
DAS::Utils::Expected<std::unique_ptr<Win32AsyncIpcTransport>> Win32AsyncIpcTransport::Create(
    boost::asio::io_context& io_context,
    const std::string&       read_endpoint,
    const std::string&       write_endpoint,
    bool                     is_server,
    size_t                   max_message_size)
{
    auto instance = std::unique_ptr<Win32AsyncIpcTransport>(
        new Win32AsyncIpcTransport(io_context));

    // 在 io_context 上运行 InitializeAsync 协程
    try
    {
        auto future = boost::asio::co_spawn(
            io_context,
            [&instance, &read_endpoint, &write_endpoint, is_server, max_message_size]() -> boost::asio::awaitable<DasResult>
            {
                co_return co_await instance->InitializeAsync(
                    read_endpoint, write_endpoint, is_server, max_message_size);
            },
            boost::asio::use_future);

        auto result = future.get();
        if (result != DAS_S_OK)
        {
            return DAS::Utils::MakeUnexpected(result);
        }
    }
    catch (...)
    {
        return DAS::Utils::MakeUnexpected(DAS_E_IPC_MESSAGE_QUEUE_FAILED);
    }

    return instance;
}

Win32AsyncIpcTransport::~Win32AsyncIpcTransport() { Close(); }

DasResult Win32AsyncIpcTransport::Initialize(
    const std::string& read_endpoint,
    const std::string& write_endpoint,
    bool               is_server,
    size_t             max_message_size)
{
    try
    {
        return boost::asio::co_spawn(
            io_context_,
            InitializeAsync(read_endpoint, write_endpoint, is_server, max_message_size),
            boost::asio::use_future).get();
    }
    catch (...)
    {
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }
}

DasResult Win32AsyncIpcTransport::Connect(
    const std::string& read_endpoint,
    const std::string& write_endpoint)
{
    if (is_connected_)
    {
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    auto result = ConnectToNamedPipe(read_endpoint, true);
    if (DAS::IsFailed(result))
    {
        return result;
    }

    result = ConnectToNamedPipe(write_endpoint, false);
    if (DAS::IsFailed(result))
    {
        return result;
    }

    is_connected_ = true;
    return DAS_S_OK;
}

void Win32AsyncIpcTransport::Close()
{
    boost::system::error_code ec;

    if (read_pipe_.is_open())
    {
        read_pipe_.close(ec);
    }

    if (write_pipe_.is_open())
    {
        write_pipe_.close(ec);
    }

    is_connected_ = false;
    is_server_ = false;
}

bool Win32AsyncIpcTransport::IsConnected() const { return is_connected_; }

std::string Win32AsyncIpcTransport::GetEndpointName() const
{
    return endpoint_name_;
}

void Win32AsyncIpcTransport::SetSharedMemoryPool(SharedMemoryPool* pool)
{
    shared_memory_pool_ = pool;
}

DasResult Win32AsyncIpcTransport::CreateNamedPipe(
    const std::string& pipe_name,
    bool               is_read_pipe)
{
    const std::string full_pipe_name =
        DAS_FMT_NS::format("{}{}", PIPE_PREFIX, pipe_name);
    const HANDLE h_pipe = ::CreateNamedPipeA(
        full_pipe_name.c_str(),
        is_read_pipe ? PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED
                     : PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        static_cast<DWORD>(max_message_size_),
        static_cast<DWORD>(max_message_size_),
        0,
        nullptr);

    if (h_pipe == INVALID_HANDLE_VALUE)
    {
        const auto msg = DAS_FMT_NS::format(
            "CreateNamedPipe failed: pipe_name = {}, error = {}",
            pipe_name,
            GetLastError());
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    boost::system::error_code ec;
    if (is_read_pipe)
    {
        read_pipe_.assign(h_pipe, ec);
    }
    else
    {
        write_pipe_.assign(h_pipe, ec);
    }

    if (ec)
    {
        CloseHandle(h_pipe);
        const auto msg = DAS_FMT_NS::format(
            "assign pipe handle failed: pipe_name = {}, error = {}",
            pipe_name,
            ec.message());
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    is_connected_ = true;
    return DAS_S_OK;
}

DasResult Win32AsyncIpcTransport::ConnectToNamedPipe(
    const std::string& pipe_name,
    bool               is_read_pipe)
{
    const std::string full_pipe_name =
        DAS_FMT_NS::format("{}{}", PIPE_PREFIX, pipe_name);

    if (WaitNamedPipeA(full_pipe_name.c_str(), NMPWAIT_WAIT_FOREVER) == FALSE)
    {
        const auto msg = DAS_FMT_NS::format(
            "WaitNamedPipe failed: pipe_name = {}, error = {}",
            pipe_name,
            GetLastError());
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    const HANDLE h_pipe = CreateFileA(
        full_pipe_name.c_str(),
        is_read_pipe ? GENERIC_READ : GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr);

    if (h_pipe == INVALID_HANDLE_VALUE)
    {
        const auto msg = DAS_FMT_NS::format(
            "CreateFile failed: pipe_name = {}, error = {}",
            pipe_name,
            GetLastError());
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    boost::system::error_code ec;
    if (is_read_pipe)
    {
        read_pipe_.assign(h_pipe, ec);
    }
    else
    {
        write_pipe_.assign(h_pipe, ec);
    }

    if (ec)
    {
        CloseHandle(h_pipe);
        const auto msg = DAS_FMT_NS::format(
            "assign pipe handle failed: pipe_name = {}, error = {}",
            pipe_name,
            ec.message());
        DAS_LOG_ERROR(msg.c_str());
        return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    return DAS_S_OK;
}

boost::asio::awaitable<std::variant<DasResult, HANDLE>>
Win32AsyncIpcTransport::OpenPipeAsync(
    const std::string& full_name,
    bool               is_read_pipe)
{
    auto   ex = co_await boost::asio::this_coro::executor;
    boost::asio::steady_timer timer(ex);

    constexpr int    max_retries     = 100; // 100 * 10ms = 1 second timeout
    constexpr auto   retry_interval  = std::chrono::milliseconds(10);

    for (int retry = 0; retry < max_retries; ++retry)
    {
        // 尝试打开管道（OVERLAPPED 模式）
        ScopedHandle h_pipe(CreateFileA(
            full_name.c_str(),
            is_read_pipe ? GENERIC_READ : GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr));

        if (h_pipe.Valid())
        {
            co_return h_pipe.Release(); // 成功
        }

        const DWORD err = GetLastError();

        if (err == ERROR_PIPE_BUSY || err == ERROR_FILE_NOT_FOUND)
        {
            // 管道忙或不存在，等待后重试
            timer.expires_after(retry_interval);
            co_await timer.async_wait(boost::asio::use_awaitable);
            continue;
        }

        // 其他错误，直接失败
        const auto msg = DAS_FMT_NS::format(
            "OpenPipe failed: pipe = {}, error = {}", full_name, err);
        DAS_LOG_ERROR(msg.c_str());
        co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    DAS_LOG_ERROR(DAS_FMT_NS::format(
        "OpenPipe timeout: pipe = {}", full_name).c_str());
    co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
}

boost::asio::awaitable<DasResult> Win32AsyncIpcTransport::WaitForClientConnectionAsync(
    HANDLE h_pipe)
{
    // 创建事件用于 OVERLAPPED I/O
    // 使用 shared_ptr 管理 ScopedHandle，然后用 Release 转移所有权给 object_handle
    // 避免双重关闭：event 析构不关闭，object_handle 析构时关闭
    auto event = std::make_shared<ScopedHandle>(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event->Valid())
    {
        DAS_LOG_ERROR("CreateEvent failed for ConnectNamedPipe");
        co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    OVERLAPPED overlapped = {};
    overlapped.hEvent = event->Get();

    // 发起异步连接
    if (ConnectNamedPipe(h_pipe, &overlapped))
    {
        co_return DAS_S_OK; // 立即完成
    }

    const DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED)
    {
        co_return DAS_S_OK; // 客户端已连接
    }

    if (err == ERROR_IO_PENDING)
    {
        // 等待异步完成
        // 使用 Release() 转移 HANDLE 所有权给 object_handle，避免双重关闭
        auto ex = co_await boost::asio::this_coro::executor;
        boost::asio::windows::object_handle wait_handle(ex, event->Release());
        co_await wait_handle.async_wait(boost::asio::use_awaitable);
        co_return DAS_S_OK;
    }

    DAS_LOG_ERROR(DAS_FMT_NS::format(
        "ConnectNamedPipe failed: error = {}", err).c_str());
    co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
}

boost::asio::awaitable<std::variant<DasResult, HANDLE>>
Win32AsyncIpcTransport::CreateNamedPipeAsync(
    const std::string& pipe_name,
    bool               is_read_pipe,
    bool               wait_for_connection)
{
    const std::string full_pipe_name =
        DAS_FMT_NS::format("{}{}", PIPE_PREFIX, pipe_name);

    // 1. 创建管道（OVERLAPPED 模式）
    ScopedHandle h_pipe(::CreateNamedPipeA(
        full_pipe_name.c_str(),
        is_read_pipe ? PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED
                     : PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        static_cast<DWORD>(max_message_size_),
        static_cast<DWORD>(max_message_size_),
        0,
        nullptr));

    if (!h_pipe.Valid())
    {
        DAS_LOG_ERROR(DAS_FMT_NS::format(
            "CreateNamedPipe failed: pipe_name = {}, error = {}",
            pipe_name, GetLastError()).c_str());
        co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    // 如果不需要等待连接，直接返回
    if (!wait_for_connection)
    {
        co_return h_pipe.Release();
    }

    // 2. 异步等待客户端连接
    auto result = co_await WaitForClientConnectionAsync(h_pipe.Get());
    if (DAS::IsFailed(result))
    {
        co_return result; // h_pipe 自动清理
    }

    // 3. 成功，释放所有权
    co_return h_pipe.Release();
}

boost::asio::awaitable<DasResult> Win32AsyncIpcTransport::InitializeAsync(
    const std::string& read_endpoint,
    const std::string& write_endpoint,
    bool               is_server,
    size_t             max_message_size)
{
    max_message_size_ = max_message_size;

    if (is_server)
    {
        // === 服务端 (MainProcess): 创建管道 + 等待连接 ===
        // 重要：必须先创建两个管道，再等待连接。
        is_server_ = true;

        // 第一步：创建两个管道（等待连接由 ConnectNamedPipe 处理）
        // CreateNamedPipeAsync 会自动等待连接，这里不需要 wait_for_connection
        // 但需要先创建两个管道，让两端都能看到
        auto write_result =
            co_await CreateNamedPipeAsync(write_endpoint, false, true);
        if (auto* err = std::get_if<DasResult>(&write_result))
            co_return *err;
        auto* h_write_ptr = std::get_if<HANDLE>(&write_result);
        if (!h_write_ptr)
        {
            co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
        }
        HANDLE h_write = *h_write_ptr;

        auto read_result = co_await CreateNamedPipeAsync(read_endpoint, true, true);
        if (auto* err = std::get_if<DasResult>(&read_result))
        {
            CloseHandle(h_write);
            co_return *err;
        }
        auto* h_read_ptr = std::get_if<HANDLE>(&read_result);
        if (!h_read_ptr)
        {
            CloseHandle(h_write);
            co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
        }
        HANDLE h_read = *h_read_ptr;

        // 分配到 stream_handle
        boost::system::error_code ec;
        read_pipe_.assign(h_read, ec);
        if (ec)
        {
            CloseHandle(h_read);
            CloseHandle(h_write);
            co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
        }

        write_pipe_.assign(h_write, ec);
        if (ec)
        {
            read_pipe_.close();
            CloseHandle(h_write);
            co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
        }

        is_connected_ = true;
        co_return DAS_S_OK;
    }

    // === 客户端 (Host): 连接管道 ===
    is_server_ = false;

    const std::string read_full_name = DAS_FMT_NS::format("{}{}", PIPE_PREFIX, read_endpoint);
    auto              read_result    = co_await OpenPipeAsync(read_full_name, true);
    if (auto* err = std::get_if<DasResult>(&read_result))
        co_return *err;
    HANDLE h_read = std::get<HANDLE>(read_result);

    const std::string write_full_name = DAS_FMT_NS::format("{}{}", PIPE_PREFIX, write_endpoint);
    auto              write_result    = co_await OpenPipeAsync(write_full_name, false);
    if (auto* err = std::get_if<DasResult>(&write_result))
    {
        CloseHandle(h_read);
        co_return *err;
    }
    HANDLE h_write = std::get<HANDLE>(write_result);

    // 分配到 stream_handle
    boost::system::error_code ec;
    read_pipe_.assign(h_read, ec);
    if (ec)
    {
        CloseHandle(h_read);
        CloseHandle(h_write);
        co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    write_pipe_.assign(h_write, ec);
    if (ec)
    {
        read_pipe_.close();
        CloseHandle(h_write);
        co_return DAS_E_IPC_MESSAGE_QUEUE_FAILED;
    }

    is_connected_ = true;
    co_return DAS_S_OK;
}

boost::asio::awaitable<std::variant<DasResult, AsyncIpcMessage>>
Win32AsyncIpcTransport::ReceiveCoroutine()
{
    // 使用局部变量而非成员变量，避免并发协程访问导致数据竞争
    // Header 大小固定，使用 std::array 避免堆分配
    std::array<uint8_t, sizeof(IPCMessageHeader)> header_buffer{};

    try {
        co_await boost::asio::async_read(
            read_pipe_,
            boost::asio::buffer(header_buffer),
            boost::asio::use_awaitable);
    } catch (const std::exception&) {
        throw;
    }

    HeaderValidationResult validation_error;
    auto validated_header = ValidatedIPCMessageHeader::Deserialize(
        header_buffer.data(),
        header_buffer.size(),
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

    // 使用局部变量而非成员变量，避免并发协程访问导致数据竞争
    std::vector<uint8_t> body_buffer(body_size);

    if (body_size > 0)
    {
        co_await boost::asio::async_read(
            read_pipe_,
            boost::asio::buffer(body_buffer),
            boost::asio::use_awaitable);
    }

    if (header.Raw().flags & FLAG_LARGE_MESSAGE)
    {
        if (shared_memory_pool_ == nullptr)
        {
            DAS_LOG_ERROR(
                "Large message received but shared memory pool not set");
            co_return DAS_E_IPC_INVALID_MESSAGE;
        }

        if (body_buffer.size() < sizeof(uint64_t))
        {
            DAS_LOG_ERROR("Large message handle missing");
            co_return DAS_E_IPC_INVALID_MESSAGE;
        }

        uint64_t handle;
        std::memcpy(&handle, body_buffer.data(), sizeof(uint64_t));

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

    co_return AsyncIpcMessage{header, std::move(body_buffer)};
}

boost::asio::awaitable<DasResult> Win32AsyncIpcTransport::SendCoroutine(
    const ValidatedIPCMessageHeader& header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    co_await boost::asio::async_write(
        write_pipe_,
        boost::asio::buffer(
            static_cast<const IPCMessageHeader*>(header),
            sizeof(IPCMessageHeader)),
        boost::asio::use_awaitable);

    if (body_size > 0)
    {
        co_await boost::asio::async_write(
            write_pipe_,
            boost::asio::buffer(body, body_size),
            boost::asio::use_awaitable);
    }

    co_return DAS_S_OK;
}

DAS_CORE_IPC_NS_END

#endif
