#pragma once
#include <chrono>
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/MainProcess/IHostLauncher.h>
#include <das/DasApi.h>
#include <das/IDasAsyncCallback.h>
#include <das/IDasAsyncLoadPluginOperation.h>
#include <memory>

// Forward declaration for boost::asio::io_context
namespace boost::asio
{
    class io_context;
}

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
            struct IIpcContext;

        } // namespace MainProcess

        // HostLauncher 前置声明
        class HostLauncher;

        namespace MainProcess
        {

            // 前置声明 DestroyIpcContext，用于友元
            DAS_API void DestroyIpcContext(IIpcContext* ctx);

            /**
             * @brief 主进程 IPC 上下文接口
 *

             * * 纯虚函数接口，protected 析构函数防止用户直接
             * delete。

             * * 使用 CreateIpcContext() 创建实例。
             */
            struct IIpcContext
            {
                virtual DistributedObjectManager& GetObjectManager() = 0;
                virtual RemoteObjectRegistry&     GetRegistry() = 0;

                /**
                 * @brief 创建 HostLauncher 实例
 *
                 * * HostLauncher 使用 IIpcContext 的 io_context
                 * 进行异步操作。
 *
                 * @param
                 * pp_out_launcher 输出：HostLauncher 接口指针
 * @return
                 * DAS_S_OK 成功
                 */
                virtual DasResult CreateHostLauncher(
                    IHostLauncher** pp_out_launcher) = 0;

                /**
                 * @brief 异步加载插件到指定 Host 进程
                 *
                 * @param host_launcher 目标 Host 进程启动器（从中获取 session_id）
                 * @param u8_plugin_path 插件 manifest 路径 (UTF-8)
                 * @param pp_out_operation 输出：异步操作对象
                 * @param timeout 超时时间（默认30秒）
                 * @return DasResult DAS_S_OK 成功创建操作
                 */
                virtual DasResult LoadPluginAsync(
                    IHostLauncher*                 host_launcher,
                    const char*                    u8_plugin_path,
                    IDasAsyncLoadPluginOperation** pp_out_operation,
                    std::chrono::milliseconds      timeout = std::chrono::seconds(30)) = 0;

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
                 * @brief 阻塞运行事件循环
                 *
                 * 调用内部 IpcRunLoop::Run() 阻塞运行 io_context 事件循环。
                 * 通常在独立线程中调用。
                 *
                 * @return DasResult 运行结果
                 */
                virtual DasResult Run() = 0;

                /**
                 * @brief 请求停止事件循环
                 *
                 * 非阻塞调用，设置停止标志后立即返回。
                 * 事件循环会在当前操作完成后退出。
                 */
                virtual void RequestStop() = 0;

                /**
                 * @brief 获取底层 io_context 引用
                 *
                 * 用于需要共享 io_context 的场景，如 HostLauncher。
                 *
                 * @return boost::asio::io_context& io_context 引用
                 */
                virtual boost::asio::io_context& GetIoContext() = 0;

                /**
                 * @brief 注册 HostLauncher 到 IPC 上下文
                 *
                 * 在 HostLauncher::Start() 成功后调用。
                 * Transport 将在注册后立即开始接收消息。
                 *
                 * @param launcher HostLauncher 实例（共享所有权）
                 * @return DasResult DAS_S_OK 成功
                 */
                virtual DasResult RegisterHostLauncher(
                    std::shared_ptr<HostLauncher> launcher) = 0;

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

        } // namespace MainProcess
    } // namespace IPC
} // namespace Core
DAS_NS_END
