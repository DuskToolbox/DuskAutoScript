#ifndef DAS_CORE_IPC_IPC_RUN_LOOP_H
#define DAS_CORE_IPC_IPC_RUN_LOOP_H

#include <cstddef>
#include <cstdint>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/IDasBase.h>
#include <functional>
#include <memory>
#include <vector>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        struct NestedCallContext
        {
            uint64_t             call_id;
            std::vector<uint8_t> response_buffer;
            bool                 completed;
        };

        class IpcTransport;

        class IpcRunLoop
        {
        public:
            using RequestHandler = std::function<
                DasResult(const IPCMessageHeader&, const uint8_t*, size_t)>;

            IpcRunLoop();
            ~IpcRunLoop();

            DasResult Initialize();
            DasResult Shutdown();
            DasResult Run();
            DasResult Stop();

            void SetRequestHandler(RequestHandler handler);

            DasResult SendRequest(
                const IPCMessageHeader& request_header,
                const uint8_t*          body,
                size_t                  body_size,
                std::vector<uint8_t>&   response_body);

            DasResult SendResponse(
                const IPCMessageHeader& response_header,
                const uint8_t*          body,
                size_t                  body_size);

            DasResult SendEvent(
                const IPCMessageHeader& event_header,
                const uint8_t*          body,
                size_t                  body_size);

            bool IsRunning() const;

        private:
            DasResult ProcessMessage(
                const IPCMessageHeader& header,
                const uint8_t*          body,
                size_t                  body_size);

            void RunInternal();

            struct Impl;
            std::unique_ptr<Impl> impl_;
        };
    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_IPC_RUN_LOOP_H
