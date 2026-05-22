#include <das/Core/IPC/AnyTransport.h>

#include <type_traits>

DAS_CORE_IPC_NS_BEGIN

namespace
{
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

AnyTransport::AnyTransport(Win32AsyncIpcTransport&& t)
    : transport_(std::move(t))
{
}
AnyTransport::AnyTransport(UnixAsyncIpcTransport&& t) : transport_(std::move(t))
{
}
AnyTransport::AnyTransport(HttpIpcTransport&& t) : transport_(std::move(t)) {}

AnyTransport::~AnyTransport() = default;
AnyTransport::AnyTransport(AnyTransport&&) noexcept = default;
AnyTransport& AnyTransport::operator=(AnyTransport&&) noexcept = default;

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
