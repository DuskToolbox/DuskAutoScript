#ifndef DAS_CORE_IPC_MAIN_PROCESS_IPC_CONTEXT_H
#define DAS_CORE_IPC_MAIN_PROCESS_IPC_CONTEXT_H

#include <chrono>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/DasApi.h>
#include <das/IDasAsyncLoadPluginOperation.h>
#include <memory>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        namespace MainProcess
        {
            class MainProcessServer;

        } // namespace MainProcess

        // 前置声明（在 IPC 命名空间内）
        class DistributedObjectManager;
        class ProxyFactory;
        class RemoteObjectRegistry;

        namespace MainProcess
        {
            class IpcContextImpl;

            /**
             * @brief 主进程 IPC 上下文实现
             *
             * RAII 风格：构造即初始化，析构即清理
             * 封装主进程端所有 IPC 组件的初始化和管理
             *
             * 管理的组件：
             * - SessionCoordinator（单例）：session_id 管理
             * - DistributedObjectManager：分布式对象生命周期管理
             * - ProxyFactory：Proxy 工厂，负责创建远程对象代理
             * - RemoteObjectRegistry（单例）：远程对象注册表
             * - MainProcessServer（单例）：主进程 IPC 服务端
             */
            class IpcContext final : public IIpcContext
            {
            public:
                IpcContext();
                ~IpcContext(); // 自动清理

                IpcContext(const IpcContext&) = delete;
                IpcContext& operator=(const IpcContext&) = delete;
                IpcContext(IpcContext&&) = delete;
                IpcContext& operator=(IpcContext&&) = delete;

                /**
                 * @brief 获取 MainProcessServer 实例
                 * @return MainProcessServer& 服务端实例
                 */
                class MainProcessServer& GetServer() override;

                class DistributedObjectManager& GetObjectManager() override;

                class ProxyFactory& GetProxyFactory();

                class RemoteObjectRegistry& GetRegistry() override;

                DasResult CreateHostLauncher(
                    IHostLauncher** pp_out_launcher) override;

                DasResult LoadPluginAsync(
                    uint16_t                       session_id,
                    const char*                    u8_plugin_path,
                    IDasAsyncLoadPluginOperation** pp_out_operation,
                    std::chrono::milliseconds      timeout = std::chrono::seconds(30)) override;

                void PostCallback(IDasAsyncCallback* callback) override;

            private:
                std::unique_ptr<IpcContextImpl> impl_;
            };

        } // namespace MainProcess
    } // namespace IPC
} // namespace Core
DAS_NS_END

#endif // DAS_CORE_IPC_MAIN_PROCESS_IPC_CONTEXT_H
