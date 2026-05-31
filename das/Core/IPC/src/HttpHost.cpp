#include <das/Core/IPC/HttpHost.h>

#include <boost/asio/post.hpp>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/Logger/Logger.h>

DAS_CORE_IPC_NS_BEGIN

HttpHost::HttpHost(uint16_t session_id, AnyTransport&& transport)
    : transport_(std::move(transport)),
      endpoint_name_(transport_.GetEndpointName()), session_id_(session_id)
{
}

HttpHost::~HttpHost() = default;

TransportLookupResult HttpHost::GetTransport()
{
    if (!IsRunning())
    {
        return {DAS_E_IPC_CONNECTION_LOST, std::nullopt};
    }

    return {DAS_S_OK, std::optional<AnyTransportRef>{std::ref(transport_)}};
}

boost::asio::io_context& HttpHost::GetIoContext()
{
    return transport_.GetIoContext();
}

uint16_t HttpHost::GetSessionId() const { return session_id_; }

uint32_t HttpHost::GetPid() const { return 0; }

bool HttpHost::IsRunning() const
{
    return !cleanup_requested_.load(std::memory_order_acquire)
           && transport_.IsConnected();
}

void HttpHost::Stop() { ScheduleCleanup("stop"); }

void HttpHost::ClearCallbacks() {}

void HttpHost::NotifyHeartbeatTimeout() {}

void HttpHost::TerminateIfRunning() { ScheduleCleanup("heartbeat timeout"); }

const std::string& HttpHost::GetEndpointName() const noexcept
{
    return endpoint_name_;
}

uint32_t HttpHost::AddRef()
{
    return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint32_t HttpHost::Release()
{
    const auto value = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (value == 0)
    {
        delete this;
    }
    return value;
}

DasResult HttpHost::QueryInterface(const DasGuid& iid, void** pp_object)
{
    if (pp_object == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    if (iid == DasIidOf<IHostConnection>() || iid == DasIidOf<IDasBase>())
    {
        AddRef();
        *pp_object = static_cast<IHostConnection*>(this);
        return DAS_S_OK;
    }

    *pp_object = nullptr;
    return DAS_E_NO_INTERFACE;
}

void HttpHost::ScheduleCleanup(const char* reason)
{
    if (cleanup_requested_.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    // HttpHost owns transport_ as stable backing storage. Cleanup only closes
    // the transport so owner-guarded async operations can unwind safely.
    auto& io_context = transport_.GetIoContext();
    AddRef();
    boost::asio::post(
        io_context,
        [this, reason]
        {
            DAS_CORE_LOG_INFO(
                "HttpHost cleanup: session_id={}, endpoint={}, reason={}",
                session_id_,
                endpoint_name_.c_str(),
                reason);
            transport_.Cleanup();
            static_cast<void>(Release());
        });
}

DAS_CORE_IPC_NS_END
