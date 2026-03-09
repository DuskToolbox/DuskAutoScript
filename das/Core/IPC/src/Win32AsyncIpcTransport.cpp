#ifdef _WIN32

#include <das/Core/IPC/Win32AsyncIpcTransport.h>

#include <asioexec/use_sender.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <cstring>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/DasApi.h>
#include <das/Utils/fmt.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <windows.h>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

inline constexpr uint16_t         FLAG_LARGE_MESSAGE = 0x01;
inline constexpr std::string_view PIPE_PREFIX = R"(\\.\pipe\)";

Win32AsyncIpcTransport::Win32AsyncIpcTransport(
    boost::asio::io_context& io_context)
    : io_context_(io_context), read_pipe_(io_context), write_pipe_(io_context),
      header_buffer_(sizeof(IPCMessageHeader))
{
}

Win32AsyncIpcTransport::~Win32AsyncIpcTransport() { Close(); }

DasResult Win32AsyncIpcTransport::Initialize(
    const std::string& read_endpoint,
    const std::string& write_endpoint,
    bool               is_server,
    size_t             max_message_size)
{
    max_message_size_ = max_message_size;

    if (is_server)
    {
        is_server_ = true;

        auto result = CreateNamedPipe(read_endpoint, true);
        if (DAS::IsFailed(result))
        {
            return result;
        }

        return CreateNamedPipe(write_endpoint, false);
    }

    return Connect(read_endpoint, write_endpoint);
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

boost::asio::awaitable<std::variant<DasResult, AsyncIpcMessage>>
Win32AsyncIpcTransport::ReceiveCoroutine()
{
    co_await boost::asio::async_read(
        read_pipe_,
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
