#pragma once
#include <das/Core/IPC/IpcMessageHeader.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/DasApi.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        // 前置声明（在 IPC 命名空间内）
        class IpcRunLoop;
        class IpcCommandHandler;
        class IDistributedObjectManager;
        struct IpcCommandResponse;

        namespace Host
        {
            struct IpcContextConfig
            {
                const char* main_process_queue_name;
            };

            struct IIpcContext;

            // 前置声明 DestroyIpcContext，用于友元
            DAS_API void DestroyIpcContext(IIpcContext* ctx);

            using OnHandshakeComplete = void (*)(
                struct IIpcContext* ctx,
                DasResult          result,
                void*              user_data);

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
                virtual IpcRunLoop&                GetRunLoop() = 0;
                virtual IpcCommandHandler&         GetCommandHandler() = 0;
                virtual IDistributedObjectManager& GetObjectManager() = 0;

                virtual void SetOnHandshakeComplete(
                    OnHandshakeComplete handler,
                    void*               user_data) = 0;

                virtual DasResult Run() = 0;
                virtual void      RequestStop() = 0;

                virtual DasResult LoadPlugin(
                    const std::filesystem::path& json_path,
                    ObjectId*                    object_id) = 0;
                virtual bool IsConnected() const = 0;

                // 注册自定义命令处理器
                virtual void RegisterCommandHandler(
                    uint32_t       cmd_type,
                    CommandHandler handler) = 0;

            protected:
                virtual ~IIpcContext() = default;

                // 允许 DestroyIpcContext 访问 protected 析构函数
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

        } // namespace Host
    } // namespace IPC
} // namespace Core
DAS_NS_END
