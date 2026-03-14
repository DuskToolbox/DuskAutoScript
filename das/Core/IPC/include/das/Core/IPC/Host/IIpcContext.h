#pragma once
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
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

            // 命令处理器类型
            using CommandHandler = std::function<DasResult(
                const ValidatedIPCMessageHeader& header,
                std::span<const uint8_t>       payload,
                IpcCommandResponse&             response)>;

            /**
             * @brief Host 进程 IPC 上下文接口
             */
            struct IIpcContext
            {
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

                /**
                 * @brief 注册本地对象到分布式对象管理器
                 *
                 * @param object_ptr 对象指针
                 * @param out_object_id 输出分配的对象ID
                 * @return DasResult
                 */
                virtual DasResult RegisterLocalObject(
                    void*     object_ptr,
                    ObjectId& out_object_id) = 0;

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

        } // namespace Host
    } // namespace IPC
} // namespace Core
DAS_NS_END
