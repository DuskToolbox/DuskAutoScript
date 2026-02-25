#ifndef DAS_CORE_IPC_IPC_RUN_LOOP_H
#define DAS_CORE_IPC_IPC_RUN_LOOP_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/IDasBase.h>

#include <memory>
#include <mutex>
#include <stdexec/execution.hpp>
#include <thread>
#include <unordered_map>
#include <vector>

// Forward declarations
DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        class IMessageHandler;
        class IpcTransport;
    }
}
DAS_NS_END

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

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

        // 内部类型，不对外导出
        class IpcRunLoop
        {
        public:


            IpcRunLoop();
            ~IpcRunLoop();

            DasResult Initialize();
            DasResult Shutdown();
            // 阻塞式消息循环
            DasResult Run();

            DasResult Stop();

            void SetTransport(std::unique_ptr<IpcTransport> transport);

            IpcTransport* GetTransport() const;

            // 等待消息循环结束
            DasResult WaitForShutdown();


            /**
             * @brief 注册消息处理器
             * @param handler 处理器实例（所有权转移）
             */
            void RegisterHandler(std::unique_ptr<IMessageHandler> handler);

            /**
             * @brief 按接口 ID 查找处理器
             * @param interface_id 接口 ID
             * @return 处理器指针，未找到返回 nullptr
             */
            [[nodiscard]]
            IMessageHandler* GetHandler(uint32_t interface_id) const;



            /**
             * @brief 同步阻塞 IPC 调用（类似 Win32 SendMessage）
             *
             * 发送消息后进入消息循环等待，支持可重入调用。
             * 内部使用 ReceiveAndDispatch() 处理消息。
             *
             * @param request_header 请求头
             * @param body 请求体
             * @param body_size 请求体大小
             * @param response_body [out] 响应体
             * @param timeout 超时时间（默认30秒）
             * @return 调用结果
             */
            DasResult SendMessage(
                const IPCMessageHeader&   request_header,
                const uint8_t*            body,
                size_t                    body_size,
                std::vector<uint8_t>&     response_body,
                std::chrono::milliseconds timeout = std::chrono::seconds(30));

            /**
             * @brief 异步 IPC 调用（返回 sender）
             *
             * 返回可 co_await 的 sender，支持协程调用。
             *
             * @param request_header 请求头
             * @param body 请求体
             * @param body_size 请求体大小
             * @return stdexec::sender 包含 pair<DasResult, vector<uint8_t>>
             */
            [[nodiscard]]
            stdexec::sender auto SendMessageAsync(
                const IPCMessageHeader& request_header,
                const uint8_t*          body,
                size_t                  body_size);

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

            /**
             * @brief 分发消息到注册的处理器
             */
            DasResult DispatchToHandler(
                const IPCMessageHeader&     header,
                const std::vector<uint8_t>& body);

            /**
             * @brief 内部 receive 方法 - 核心可重入逻辑
             */
            bool ReceiveAndDispatch(std::chrono::milliseconds timeout);

            void RunInternal();

            // 直接成员（移除 pimpl）
            std::unordered_map<uint64_t, NestedCallContext> pending_calls_;
            std::unique_ptr<IpcTransport>                   transport_;

            std::unordered_map<uint32_t, std::unique_ptr<IMessageHandler>>
                                   handlers_;
            std::atomic<uint64_t>  next_call_id_{1};
            std::atomic<bool>      running_{false};
            std::atomic<DasResult> exit_code_{DAS_S_OK};
            std::thread            io_thread_;
            std::mutex             pending_mutex_;
        };

    }
}
DAS_NS_END

#ifdef _MSC_VER
#pragma warning(pop)
#endif

// stdexec 方法实现（必须在头文件中，因为返回 auto 类型）
DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        inline stdexec::sender auto IpcRunLoop::SendMessageAsync(
            const IPCMessageHeader& request_header,
            const uint8_t*          body,
            size_t                  body_size)
        {
            // 使用同步实现包装
            std::vector<uint8_t> response;
            DasResult result = SendMessage(request_header, body, body_size, response);
            return stdexec::just(std::make_pair(result, std::move(response)));
        }
    }
}
DAS_NS_END

#endif // DAS_CORE_IPC_IPC_RUN_LOOP_H
