#include <das/Core/IPC/AnyTransport.h>

#include <das/Core/IPC/AfUnixAvailable.h>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/Logger/Logger.h>

#include <exception>
#include <type_traits>

DAS_CORE_IPC_NS_BEGIN

namespace
{
    enum class RuntimeTransportKind
    {
        UnixSocket,
        NamedPipe,
    };

    RuntimeTransportKind SelectRuntimeTransportKind()
    {
#ifdef DAS_WINDOWS
        if (AfUnixAvailable())
        {
            return RuntimeTransportKind::UnixSocket;
        }
        return RuntimeTransportKind::NamedPipe;
#else
        return RuntimeTransportKind::UnixSocket;
#endif
    }

    const char* ToLogString(RuntimeTransportKind kind) noexcept
    {
        switch (kind)
        {
        case RuntimeTransportKind::UnixSocket:
            return "UnixAsyncIpcTransport";
        case RuntimeTransportKind::NamedPipe:
            return "Win32AsyncIpcTransport";
        }

        return "UnknownTransport";
    }

    AnyTransport CreateUninitializedTransport(
        RuntimeTransportKind        selected_kind,
        boost::asio::io_context& io_context)
    {
#ifdef DAS_WINDOWS
        if (selected_kind == RuntimeTransportKind::NamedPipe)
        {
            auto transport =
                Win32AsyncIpcTransport::CreateUninitialized(io_context);
            return AnyTransport(std::move(*transport));
        }
#else
        (void)selected_kind;
#endif

        auto transport =
            UnixAsyncIpcTransport::CreateUninitialized(io_context);
        return AnyTransport(std::move(*transport));
    }

    template <typename Transport>
    boost::asio::awaitable<DasResult> InitializeTransport(
        Transport&          transport,
        const std::string& read_endpoint,
        const std::string& write_endpoint,
        bool               is_server,
        size_t             max_message_size)
    {
        using TransportType = std::decay_t<Transport>;

        if constexpr (std::is_same_v<TransportType, HttpIpcTransport>)
        {
            DAS_CORE_LOG_ERROR(
                "AnyTransport::InitializeAsync does not support HTTP "
                "transport: read_endpoint = {}, write_endpoint = {}",
                read_endpoint,
                write_endpoint);
            co_return DAS_E_IPC_INVALID_ARGUMENT;
        }
        else
        {
            co_return co_await transport.InitializeAsync(
                read_endpoint,
                write_endpoint,
                is_server,
                max_message_size);
        }
    }

    template <typename Transport>
    concept HasPublicCleanup = requires(Transport& transport)
    {
        transport.Cleanup();
    };

    template <typename Transport>
    void CleanupTransport(Transport& transport)
    {
        if constexpr (HasPublicCleanup<Transport>)
        {
            transport.Cleanup();
        }
        else
        {
#ifdef DAS_WINDOWS
            if constexpr (std::is_same_v<Transport, Win32AsyncIpcTransport>)
            {
                if (!transport.IsConnected())
                {
                    return;
                }
            }
#endif
            auto& io_context = transport.GetIoContext();
            auto  replacement = Transport::CreateUninitialized(io_context);
            if (replacement)
            {
                transport = std::move(*replacement);
            }
        }
    }
} // namespace

#ifdef DAS_WINDOWS
AnyTransport::AnyTransport(Win32AsyncIpcTransport&& t) : transport_(std::move(t))
{
}
#endif
AnyTransport::AnyTransport(UnixAsyncIpcTransport&& t) : transport_(std::move(t))
{
}
AnyTransport::AnyTransport(HttpIpcTransport&& t) : transport_(std::move(t)) {}

AnyTransport::~AnyTransport() = default;
AnyTransport::AnyTransport(AnyTransport&&) noexcept = default;
AnyTransport& AnyTransport::operator=(AnyTransport&&) noexcept = default;

boost::asio::awaitable<AnyTransport::CreateAsyncResult>
AnyTransport::CreateAsync(
    boost::asio::io_context& io_context,
    const std::string&       read_endpoint,
    const std::string&       write_endpoint,
    bool                     is_server,
    size_t                   max_message_size)
{
    const auto selected_kind = SelectRuntimeTransportKind();

    try
    {
        auto transport = CreateUninitializedTransport(selected_kind, io_context);
        auto result = co_await transport.InitializeAsync(
            read_endpoint,
            write_endpoint,
            is_server,
            max_message_size);

        if (result != DAS_S_OK)
        {
            DAS_CORE_LOG_ERROR(
                "AnyTransport::CreateAsync failed: selected_kind = {}, "
                "read_endpoint = {}, write_endpoint = {}, result = {}",
                ToLogString(selected_kind),
                read_endpoint,
                write_endpoint,
                result);
            co_return CreateAsyncResult{result, std::nullopt};
        }

        co_return CreateAsyncResult{DAS_S_OK, std::move(transport)};
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_ERROR(
            "AnyTransport::CreateAsync threw: selected_kind = {}, "
            "read_endpoint = {}, write_endpoint = {}, error = {}",
            ToLogString(selected_kind),
            read_endpoint,
            write_endpoint,
            e.what());
        co_return CreateAsyncResult{
            DAS_E_IPC_MESSAGE_QUEUE_FAILED,
            std::nullopt};
    }
}

AnyTransport AnyTransport::CreateUninitialized(
    boost::asio::io_context& io_context)
{
    const auto selected_kind = SelectRuntimeTransportKind();
    return CreateUninitializedTransport(selected_kind, io_context);
}

boost::asio::awaitable<DasResult> AnyTransport::InitializeAsync(
    const std::string& read_endpoint,
    const std::string& write_endpoint,
    bool               is_server,
    size_t             max_message_size)
{
    return std::visit(
        [&](auto& transport)
        {
            return InitializeTransport(
                transport,
                read_endpoint,
                write_endpoint,
                is_server,
                max_message_size);
        },
        transport_);
}

bool AnyTransport::IsConnected() const
{
    return std::visit(
        [](const auto& t) -> bool { return t.IsConnected(); },
        transport_);
}

boost::asio::io_context& AnyTransport::GetIoContext()
{
    return std::visit(
        [](auto& t) -> boost::asio::io_context& { return t.GetIoContext(); },
        transport_);
}

void AnyTransport::SetSharedMemoryPool(SharedMemoryPool* pool)
{
    std::visit([pool](auto& t) { t.SetSharedMemoryPool(pool); }, transport_);
}

std::string AnyTransport::GetEndpointName() const
{
    return std::visit(
        [](const auto& t) { return t.GetEndpointName(); },
        transport_);
}

void AnyTransport::Cleanup()
{
    std::visit([](auto& t) { CleanupTransport(t); }, transport_);
}

boost::asio::awaitable<DasResult> AnyTransport::SendCoroutine(
    const ValidatedIPCMessageHeader& header,
    const uint8_t*                   body,
    size_t                           body_size)
{
    return std::visit(
        [header, body, body_size](auto& t)
        { return t.SendCoroutine(header, body, body_size); },
        transport_);
}

boost::asio::awaitable<std::variant<DasResult, AsyncIpcMessage>>
AnyTransport::ReceiveCoroutine()
{
    return std::visit([](auto& t) { return t.ReceiveCoroutine(); }, transport_);
}

DAS_CORE_IPC_NS_END
