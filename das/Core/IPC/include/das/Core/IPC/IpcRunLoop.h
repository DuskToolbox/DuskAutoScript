#ifndef DAS_CORE_IPC_IPC_RUN_LOOP_H
#define DAS_CORE_IPC_IPC_RUN_LOOP_H

#include <cstddef>
#include <cstdint>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/IDasBase.h>
#include <functional>
#include <memory>
#include <stdexec/execution.hpp>
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

            // stdexec sender 接口
            // 启动 IPC 线程，返回一个在正常关闭时完成的 sender
            // 使用方式：auto sender = run_loop.RunAsync();
            //          stdexec::sync_wait(std::move(sender)); // 等待线程退出
            [[nodiscard]]
            stdexec::sender auto RunAsync();

            // 等待 IPC 线程关闭，返回一个在关闭完成时完成的 sender
            // 调用 Stop() 并等待线程退出
            [[nodiscard]]
            stdexec::sender auto WaitForShutdown();

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
