#ifndef DAS_CORE_IPC_MESSAGE_QUEUE_TRANSPORT_H
#define DAS_CORE_IPC_MESSAGE_QUEUE_TRANSPORT_H

#include <boost/interprocess/ipc/message_queue.hpp>
#include <cstdint>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/DasExport.h>
#include <das/IDasBase.h>
#include <memory>
#include <string>
#include <vector>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        class SharedMemoryPool;

// Disable C4251 warning for std::unique_ptr in exported classes
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

        class DAS_API IpcTransport
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
                const IPCMessageHeader& header,
                const uint8_t*          body,
                size_t                  body_size);

            DasResult Receive(
                IPCMessageHeader&     out_header,
                std::vector<uint8_t>& out_body,
                uint32_t              timeout_ms);

            DasResult SetSharedMemoryPool(SharedMemoryPool* pool);

            bool IsConnected() const;

            static std::string MakeQueueName(
                uint16_t host_id,
                uint16_t plugin_id,
                bool     is_host_to_plugin);

        private:
            DasResult SendSmallMessage(
                const IPCMessageHeader& header,
                const uint8_t*          body,
                size_t                  body_size);

            DasResult SendLargeMessage(
                const IPCMessageHeader& header,
                const uint8_t*          body,
                size_t                  body_size);

            struct Impl;
            std::unique_ptr<Impl> impl_;
        };

#ifdef _MSC_VER
#pragma warning(pop)
#endif

    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_MESSAGE_QUEUE_TRANSPORT_H
