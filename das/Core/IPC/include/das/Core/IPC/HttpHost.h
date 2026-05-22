#ifndef DAS_CORE_IPC_HTTP_HOST_H
#define DAS_CORE_IPC_HTTP_HOST_H

#include <atomic>
#include <cstdint>
#include <das/Core/IPC/AnyTransport.h>
#include <das/Core/IPC/IInternalHost.h>
#include <string>

DAS_CORE_IPC_NS_BEGIN

class HttpHost final : public IInternalHost
{
public:
    HttpHost(uint16_t session_id, AnyTransport&& transport);
    ~HttpHost() override;

    HttpHost(const HttpHost&) = delete;
    HttpHost& operator=(const HttpHost&) = delete;

    TransportLookupResult GetTransport() override;
    boost::asio::io_context& GetIoContext() override;
    uint16_t GetSessionId() const override;
    uint32_t GetPid() const override;
    bool IsRunning() const override;
    void Stop() override;
    void ClearCallbacks() override;
    void NotifyHeartbeatTimeout() override;
    void TerminateIfRunning() override;

    [[nodiscard]]
    const std::string& GetEndpointName() const noexcept;

    uint32_t AddRef() override;
    uint32_t Release() override;
    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override;

private:
    void ScheduleCleanup(const char* reason);

    AnyTransport          transport_;
    std::string           endpoint_name_;
    uint16_t              session_id_ = 0;
    std::atomic<uint32_t> ref_count_{1};
    std::atomic<bool>     cleanup_requested_{false};
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_HTTP_HOST_H
