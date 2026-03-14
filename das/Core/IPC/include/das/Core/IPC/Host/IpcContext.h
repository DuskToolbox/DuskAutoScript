#ifndef DAS_CORE_IPC_HOST_IPC_CONTEXT_H
#define DAS_CORE_IPC_HOST_IPC_CONTEXT_H

#include <atomic>
#include <das/Core/IPC/DefaultAsyncIpcTransport.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/Host/HandshakeHandler.h>
#include <das/Core/IPC/Host/IIpcContext.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <filesystem>
#include <memory>
#include <thread>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        namespace Host
        {
            /**
             * @brief Host 进程 IPC 上下文实现
             *
             * 封装 Host 进程的 IPC 组件，提供统一的初始化和访问接口。
             *
             * 管理的组件：
             * - SessionCoordinator：session_id 管理（动态分配）
             * - DistributedObjectManager：分布式对象生命周期管理
             * - IpcRunLoop：IPC 事件循环
             * - IpcCommandHandler：IPC 命令处理器
             */
            class IpcContext : public IIpcContext
            {
            public:
                explicit IpcContext(const IpcContextConfig& config);
                ~IpcContext();

                IpcContext(const IpcContext&) = delete;
                IpcContext& operator=(const IpcContext&) = delete;
                IpcContext(IpcContext&&) = delete;
                IpcContext& operator=(IpcContext&&) = delete;

                /**
                 * @brief 获取 IpcRunLoop 实例
                 * @return IpcRunLoop& 运行循环实例
                 */
                class IpcRunLoop& GetRunLoop();

                /**
                 * @brief 获取 IpcCommandHandler 实例
                 * @return IpcCommandHandler& 命令处理器实例
                 */
                class IpcCommandHandler& GetCommandHandler();

                /**
                 * @brief 获取 IDistributedObjectManager 实例
                 * @return IDistributedObjectManager& 对象管理器实例
                 */
                struct IDistributedObjectManager& GetObjectManager();

                /**
                 * @brief 启动事件循环
                 * @return DasResult 启动结果
                 */
                DasResult Run();

                /**
                 * @brief 请求停止事件循环
                 */
                void RequestStop();

                /**
                 * @brief 加载插件
                 *
                 * @param json_path 插件配置 JSON 文件路径
                 * @param object_id [out] 创建的插件对象 ID
                 * @return DasResult 加载结果
                 */
                DasResult LoadPlugin(
                    const std::filesystem::path& json_path,
                    ObjectId*                    object_id);

                /**
                 * @brief 检查是否已连接
                 * @return 如果已连接返回 true，否则返回 false
                 */
                /**
                 * @brief 注册自定义命令处理器
                 *
                 * @param cmd_type 命令类型
                 * @param handler 处理函数
                 */
                void RegisterCommandHandler(
                    uint32_t       cmd_type,
                    CommandHandler handler);

                bool IsConnected() const;

                void PostCallback(IDasAsyncCallback* callback);

                DasResult RegisterLocalObject(
                    void*     object_ptr,
                    ObjectId& out_object_id);

            private:
                void Uninitialize();

                DasResult Initialize();
                DasResult InitializeAsHost();
                DasResult CreateHostResources();

                boost::asio::awaitable<void> ReceiveLoopCoroutine();

                void StartParentProcessMonitor();
                void StopParentProcessMonitor();

                IpcContextConfig                          config_;
                uint16_t                                  session_id_ = 0;
                std::unique_ptr<DistributedObjectManager> object_manager_;
                std::unique_ptr<IpcRunLoop>               run_loop_;
                DasPtr<IpcCommandHandler>                 command_handler_;
                DasPtr<HandshakeHandler>                  handshake_handler_;
                std::unique_ptr<SharedMemoryPool>         shared_memory_;
                std::unique_ptr<DefaultAsyncIpcTransport> async_transport_;

                bool is_initialized_ = false;
                bool is_connected_ = false;
                bool is_running_ = false;

                uint32_t host_pid_ = 0;
                uint32_t main_pid_ = 0;

                std::string host_read_queue_;
                std::string host_write_queue_;
                bool        host_is_server_ = false;

                std::thread       parent_monitor_thread_;
                std::atomic<bool> parent_monitor_running_{false};
            };

        } // namespace Host
    } // namespace IPC
} // namespace Core
DAS_NS_END

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif // DAS_CORE_IPC_HOST_IPC_CONTEXT_H
