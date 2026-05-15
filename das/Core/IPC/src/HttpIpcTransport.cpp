#include <das/Core/IPC/HttpIpcTransport.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <cstring>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/Logger/Logger.h>
#include <das/Utils/StringUtils.h>
#include <das/Utils/fmt.h>
#include <utility>
#include <vector>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

struct HttpIpcTransport::Impl
{
    explicit Impl(boost::asio::ip::tcp::socket&& socket)
        : ws_(std::move(socket))
    {
        // 设置二进制模式（IPC 使用二进制帧）
        ws_.binary(true);

        // 设置超时建议
        ws_.set_option(
            boost::beast::websocket::stream_base::timeout::suggested(
                boost::beast::role_type::server));
    }

    explicit Impl(
        boost::beast::websocket::stream<boost::asio::ip::tcp::socket>&& ws)
        : ws_(std::move(ws))
    {
        // 确保二进制模式
        ws_.binary(true);
    }

    void Close()
    {
        if (!is_connected)
        {
            return;
        }

        boost::system::error_code ec;
        ws_.close(boost::beast::websocket::close_code::normal, ec);
        if (ec)
        {
            auto msg = DAS_FMT_NS::format(
                "Close: WebSocket close failed: {}",
                ToString(ec.message()));
            DAS_CORE_LOG_WARN("{}", msg.c_str());
        }

        ws_.next_layer().close(ec);
        if (ec)
        {
            auto msg = DAS_FMT_NS::format(
                "Close: socket close failed: {}",
                ToString(ec.message()));
            DAS_CORE_LOG_WARN("{}", msg.c_str());
        }

        is_connected = false;
    }

    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws_;
    bool is_connected = true;
};

HttpIpcTransport::HttpIpcTransport(boost::asio::ip::tcp::socket&& socket)
    : impl_(std::make_unique<Impl>(std::move(socket)))
{
}

HttpIpcTransport::HttpIpcTransport(
    boost::beast::websocket::stream<boost::asio::ip::tcp::socket>&& ws)
    : impl_(std::make_unique<Impl>(std::move(ws)))
{
}

HttpIpcTransport::~HttpIpcTransport() { Cleanup(); }

HttpIpcTransport::HttpIpcTransport(HttpIpcTransport&&) noexcept = default;
HttpIpcTransport& HttpIpcTransport::operator=(HttpIpcTransport&&) noexcept =
    default;

boost::asio::awaitable<DasResult> HttpIpcTransport::SendCoroutine(
    const ValidatedIPCMessageHeader& header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    if (!impl_ || !impl_->is_connected)
    {
        co_return DAS_E_IPC_CONNECTION_LOST;
    }

    try
    {
        // 构建 buffer: header (32 bytes) + body
        // WebSocket 不支持 scatter-gather write，需要连续内存
        const auto* raw_header = static_cast<const IPCMessageHeader*>(header);

        if (body_size > 0 && body != nullptr)
        {
            std::vector<uint8_t> combined(sizeof(IPCMessageHeader) + body_size);
            std::memcpy(combined.data(), raw_header, sizeof(IPCMessageHeader));
            std::memcpy(
                combined.data() + sizeof(IPCMessageHeader),
                body,
                body_size);

            co_await impl_->ws_.async_write(
                boost::asio::buffer(combined),
                boost::asio::use_awaitable);
        }
        else
        {
            co_await impl_->ws_.async_write(
                boost::asio::buffer(raw_header, sizeof(IPCMessageHeader)),
                boost::asio::use_awaitable);
        }
    }
    catch (const std::exception& e)
    {
        auto msg = DAS_FMT_NS::format("SendCoroutine: {}", ToString(e.what()));
        DAS_CORE_LOG_ERROR("{}", msg.c_str());
        impl_->is_connected = false;
        co_return DAS_E_IPC_SEND_FAILED;
    }

    co_return DAS_S_OK;
}

boost::asio::awaitable<std::variant<DasResult, AsyncIpcMessage>>
HttpIpcTransport::ReceiveCoroutine()
{
    if (!impl_ || !impl_->is_connected)
    {
        co_return DAS_E_IPC_CONNECTION_LOST;
    }

    boost::beast::flat_buffer read_buffer;

    try
    {
        co_await impl_->ws_.async_read(read_buffer, boost::asio::use_awaitable);
    }
    catch (const boost::system::system_error&)
    {
        impl_->is_connected = false;
        co_return DAS_E_IPC_CONNECTION_LOST;
    }
    catch (const std::exception& e)
    {
        auto msg =
            DAS_FMT_NS::format("ReceiveCoroutine: {}", ToString(e.what()));
        DAS_CORE_LOG_ERROR("{}", msg.c_str());
        impl_->is_connected = false;
        co_return DAS_E_IPC_CONNECTION_LOST;
    }

    if (!impl_->ws_.got_binary())
    {
        DAS_CORE_LOG_WARN(
            "ReceiveCoroutine: received non-binary frame, discarding");
        co_return DAS_E_IPC_INVALID_MESSAGE;
    }

    const auto*  data = static_cast<const uint8_t*>(read_buffer.data().data());
    const size_t size = read_buffer.size();

    if (size < sizeof(IPCMessageHeader))
    {
        DAS_CORE_LOG_ERROR(
            "ReceiveCoroutine: frame too small: size = {}",
            size);
        co_return DAS_E_IPC_INVALID_MESSAGE;
    }

    HeaderValidationResult validation_error;
    auto validated_header = ValidatedIPCMessageHeader::Deserialize(
        data,
        sizeof(IPCMessageHeader),
        &validation_error);

    if (!validated_header.has_value())
    {
        std::string hex_bytes;
        for (size_t i = 0; i < std::min(size, sizeof(IPCMessageHeader)); ++i)
        {
            hex_bytes += DAS_FMT_NS::format("{:02X} ", data[i]);
        }
        DAS_CORE_LOG_ERROR(
            "Header validation failed: {}, raw bytes: [{}]",
            validation_error.message,
            hex_bytes);
        co_return DAS_E_IPC_INVALID_MESSAGE;
    }

    auto       header = *validated_header;
    const auto body_size = header.Raw().body_size;

    DAS_CORE_LOG_INFO(
        "ReceiveCoroutine: received header: msg_type = {}, "
        "interface_id = {}, call_id = {}, body_size = {}, header_flags = {}",
        static_cast<int>(header.Raw().message_type),
        header.Raw().interface_id,
        header.Raw().call_id,
        body_size,
        static_cast<int>(header.Raw().header_flags));

    const size_t expected_frame_size = sizeof(IPCMessageHeader) + body_size;
    if (size < expected_frame_size)
    {
        DAS_CORE_LOG_ERROR(
            "ReceiveCoroutine: frame body truncated: expected = {}, "
            "got = {}",
            expected_frame_size,
            size);
        co_return DAS_E_IPC_INVALID_MESSAGE;
    }

    std::vector<uint8_t> body(body_size);
    if (body_size > 0)
    {
        std::memcpy(body.data(), data + sizeof(IPCMessageHeader), body_size);
    }

    co_return AsyncIpcMessage{header, std::move(body)};
}

bool HttpIpcTransport::IsConnected() const
{
    return impl_ && impl_->is_connected;
}

boost::asio::io_context& HttpIpcTransport::GetIoContext()
{
    return static_cast<boost::asio::io_context&>(
        impl_->ws_.get_executor().context());
}

void HttpIpcTransport::SetSharedMemoryPool(SharedMemoryPool* /*pool*/)
{
    // HTTP 模式不使用共享内存，大消息直接通过 WebSocket 帧传输
}

std::string HttpIpcTransport::GetEndpointName() const
{
    if (!impl_ || !impl_->is_connected)
    {
        return {};
    }

    boost::system::error_code ec;
    auto                      ep = impl_->ws_.next_layer().remote_endpoint(ec);
    if (ec)
    {
        return {};
    }

    return DAS_FMT_NS::format("{}:{}", ep.address().to_string(), ep.port());
}

void HttpIpcTransport::Cleanup()
{
    if (impl_)
    {
        impl_->Close();
    }
}

DAS_CORE_IPC_NS_END
