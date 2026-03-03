#ifndef DAS_CORE_IPC_IPC_TRANSPORT_H
#define DAS_CORE_IPC_IPC_TRANSPORT_H

#include <boost/interprocess/ipc/message_queue.hpp>
#include <cstdint>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/DasExport.h>
#include <das/IDasBase.h>
#include <memory>
#include <string>
#include <vector>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN
class SharedMemoryPool;

class IpcTransport
{
public:
    IpcTransport();
    ~IpcTransport();

    DasResult Initialize(
        const std::string& host_queue_name,
        const std::string& plugin_queue_name,
        uint32_t           max_message_size,
        uint32_t           max_messages);
    DasResult Connect(
        const std::string& host_queue_name,
        const std::string& plugin_queue_name);
    DasResult Shutdown();

    DasResult Send(
        const ValidatedIPCMessageHeader& header,
        const uint8_t*                   body,
        size_t                           body_size);

    DasResult Receive(
        IPCMessageHeader&     out_header,
        std::vector<uint8_t>& out_body,
        uint32_t              timeout_ms);

    DasResult SetSharedMemoryPool(SharedMemoryPool* pool);

    bool IsConnected() const;

    static std::string MakeQueueName(
        uint32_t main_pid,
        uint32_t host_pid,
        bool     is_main_to_host);

private:
    DasResult SendSmallMessage(
        const ValidatedIPCMessageHeader& header,
        const uint8_t*                   body,
        size_t                           body_size);

    DasResult SendLargeMessage(
        const ValidatedIPCMessageHeader& header,
        const uint8_t*                   body,
        size_t                           body_size);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_TRANSPORT_H
