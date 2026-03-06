#pragma once
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/DasApi.h>
#include <memory>
#include <stdexec/execution.hpp>
DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        // 前置声明（在 IPC 命名空间内）
        class DistributedObjectManager;
        class ProxyFactory;
        class RemoteObjectRegistry;

        namespace MainProcess
        {
            // 前置声明
            class MainProcessServer;
            struct IIpcContext;

            // 前置声明 DestroyIpcContext，用于友元
            DAS_API void DestroyIpcContext(IIpcContext* ctx);

            /**
             * @brief 主进程 IPC 上下文接口
             *
             * 纯虚函数接口，protected 析构函数防止用户直接 delete。
             * 使用 CreateIpcContext() 创建实例。
             */
            struct IIpcContext
            {
                virtual MainProcessServer&        GetServer() = 0;
                virtual DistributedObjectManager& GetObjectManager() = 0;
                virtual RemoteObjectRegistry&     GetRegistry() = 0;

                //=== stdexec Scheduler 原语 ===

                /**
                 * @brief 投递请求到消息循环线程
                 */
                virtual void PostRequest(
                    void (*callback)(void* user_data),
                    void* user_data) = 0;

                /**
                 * @brief 阻塞等待并处理一轮消息
                 */
                virtual void PumpMessage() = 0;

            protected:
                virtual ~IIpcContext() = default;

                // 允许 DestroyIpcContext 访问 protected 析构函数
                friend void DestroyIpcContext(IIpcContext* ctx);
            };

            /**
             * @brief IIpcContext 的删除器
             */
            struct IpcContextDeleter
            {
                DAS_API void operator()(IIpcContext* ctx) const;
            };

            using IpcContextPtr =
                std::unique_ptr<IIpcContext, IpcContextDeleter>;

            /**
             * @brief 创建主进程 IPC 上下文（返回裸指针）
             * @return IIpcContext* 上下文指针，失败返回 nullptr
             */
            DAS_API IIpcContext* CreateIpcContext();

            /**
             * @brief 便捷创建函数（返回 unique_ptr）
             * @return IpcContextPtr 自动管理生命周期的智能指针
             */
            inline IpcContextPtr CreateIpcContextEz()
            {
                return IpcContextPtr{CreateIpcContext()};
            }

            /**
             * @brief 便捷创建函数（返回 shared_ptr）
             * @return std::shared_ptr<IIpcContext> 可共享的智能指针
             */
            inline std::shared_ptr<IIpcContext> CreateIpcContextShared()
            {
                return std::shared_ptr<IIpcContext>(
                    CreateIpcContext(),
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

                IIpcContext& context() const noexcept
                {
                    return *ctx_;
                }

                std::shared_ptr<IIpcContext> const& ptr() const noexcept
                {
                    return ctx_;
                }

                void PostRequest(void (*callback)(void*), void* user_data)
                {
                    ctx_->PostRequest(callback, user_data);
                }

                void PumpMessage() { ctx_->PumpMessage(); }

            private:
                std::shared_ptr<IIpcContext> ctx_;
            };

            // tag_invoke - 使 stdexec::schedule(scheduler) 工作
            inline auto tag_invoke(stdexec::schedule_t, const Scheduler& sched) noexcept
            {
                return DAS::Core::IPC::ScheduleSender<Scheduler>{
                    const_cast<Scheduler*>(&sched)};
            }


        } // namespace MainProcess
    } // namespace IPC
} // namespace Core
DAS_NS_END
