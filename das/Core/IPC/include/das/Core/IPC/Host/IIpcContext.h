#pragma once
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/DasApi.h>
#include <das/IDasAsyncCallback.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stdexec/execution.hpp>
DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        // 前置声明（在 IPC 命名空间内）
        class IpcRunLoop;
        class IpcCommandHandler;
        struct IDistributedObjectManager;
        struct IpcCommandResponse;

        namespace Host
        {
            struct IpcContextConfig
            {
                const char* main_process_queue_name;

                // 主进程 PID
                // - 0: 独立模式，Host 自行生成 session_id
                // - 非0: 连接模式，通过 IPC 向主进程请求 session_id
                uint32_t main_pid = 0;
            };

            struct IIpcContext;

            // 前置声明 DestroyIpcContext，用于友元
            DAS_API void DestroyIpcContext(IIpcContext* ctx);

            using OnHandshakeComplete = void (*)(
                struct IIpcContext* ctx,
                DasResult           result,
                void*               user_data);

            // 命令处理器类型
            using CommandHandler = std::function<DasResult(
                const IPCMessageHeader&  header,
                std::span<const uint8_t> payload,
                IpcCommandResponse&      response)>;

            /**
             * @brief Host 进程 IPC 上下文接口
             */
            struct IIpcContext
            {
                virtual void SetOnHandshakeComplete(
                    OnHandshakeComplete handler,
                    void*               user_data) = 0;

                virtual DasResult Run() = 0;
                virtual void      RequestStop() = 0;

                virtual bool IsConnected() const = 0;

                virtual void RegisterCommandHandler(
                    uint32_t       cmd_type,
                    CommandHandler handler) = 0;

                /**
                 * @brief 将回调投递到 io_context 线程执行
                 *
                 * 使用 DasPtr 管理 callback 生命周期，保证在 post
                 * 之前获取所有权，确保回调在执行时有效。
                 *
                 * @param callback 回调接口指针（调用者传递所有权）
                 */
                virtual void PostCallback(IDasAsyncCallback* callback) = 0;

            protected:
                virtual ~IIpcContext() = default;

                friend void DestroyIpcContext(IIpcContext* ctx);
            };

            struct IpcContextDeleter
            {
                DAS_API void operator()(IIpcContext* ctx) const;
            };

            using IpcContextPtr =
                std::unique_ptr<IIpcContext, IpcContextDeleter>;

            DAS_API IIpcContext* CreateIpcContext(
                const IpcContextConfig& config);

            inline IpcContextPtr CreateIpcContextEz(
                const IpcContextConfig& config)
            {
                return IpcContextPtr{CreateIpcContext(config)};
            }

            /**
             * @brief 便捷创建函数（返回 shared_ptr）
             * @return std::shared_ptr<IIpcContext> 可共享的智能指针
             */
            inline std::shared_ptr<IIpcContext> CreateIpcContextShared(
                const IpcContextConfig& config)
            {
                return std::shared_ptr<IIpcContext>(
                    CreateIpcContext(config),
                    DestroyIpcContext);
            }

            /**
             * @brief Scheduler 类型擦除包装器
             *
             * 包装 shared_ptr<IIpcContext>，提供 scheduler concept 支持。
             * 用于解决抽象类无法直接作为 stdexec scheduler 的问题。
             */
            class Scheduler
            {
            public:
                explicit Scheduler(std::shared_ptr<IIpcContext> ctx) noexcept
                    : ctx_(std::move(ctx))
                {
                }

                Scheduler() noexcept = default;

                bool operator==(const Scheduler& other) const noexcept
                {
                    return ctx_ == other.ctx_;
                }

                IIpcContext& context() const noexcept { return *ctx_; }

                std::shared_ptr<IIpcContext> const& ptr() const noexcept
                {
                    return ctx_;
                }

            private:
                std::shared_ptr<IIpcContext> ctx_;
            };

            // tag_invoke - 使 stdexec::schedule(scheduler) 工作
            inline auto tag_invoke(
                stdexec::schedule_t,
                const Scheduler& sched) noexcept
            {
                return DAS::Core::IPC::ScheduleSender<Scheduler>{
                    const_cast<Scheduler*>(&sched)};
            }

        } // namespace Host
    } // namespace IPC
} // namespace Core
DAS_NS_END
