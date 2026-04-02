#pragma once
#include <chrono>
#include <das/Core/IPC/DasAsyncSender.h>
#include <das/Core/IPC/MainProcess/IHostLauncher.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/DasApi.h>
#include <das/DasTypes.hpp>
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
                 * @param host_launcher 目标 Host 进程启动器（从中获取
                 * session_id）
                 * @param u8_plugin_path 插件 manifest 路径 (UTF-8)
                 * @param pp_out_operation 输出：异步操作对象
                 * @param timeout 超时时间（默认30秒）
                 * @return DasResult DAS_S_OK 成功创建操作
                 */
                virtual DasResult LoadPluginAsync(
                    IHostLauncher*                 host_launcher,
                    const char*                    u8_plugin_path,
                    IDasAsyncLoadPluginOperation** pp_out_operation,
                    std::chrono::milliseconds      timeout =
                        std::chrono::seconds(30)) = 0;

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
                 * @brief 获取已连接的 session ID 列表
                 *
                 * @return std::vector<uint16_t> 已连接的 session ID 列表
                 */
                virtual std::vector<uint16_t> GetConnectedSessions() = 0;

                /**
                 * @brief 分配新的 session_id（主进程侧）
                 * @return 分配的 session_id，如果失败返回 0
                 */
                virtual uint16_t AllocateSessionId() = 0;

                /**
                 * @brief 释放 session_id（主进程侧）
                 * @param session_id 要释放的 session_id
                 */
                virtual void ReleaseSessionId(uint16_t session_id) = 0;

                /**
                 * @brief Create a remote proxy from an ObjectId
                 *
                 * Uses RemoteObjectRegistry::ComputeInterfaceId to convert
                 * DasGuid to uint32_t hash, then calls
                 * DasIpcProxy::CreateProxyByInterfaceId to create a typed
                 * proxy.
                 *
                 * @param object_id The ObjectId of the remote object
                 * @param iid The requested interface GUID (DasGuid) - will be
                 * hashed to uint32_t
                 * @param pp_out Output: proxy as IDasBase* (caller must
                 * Release)
                 * @return DasResult DAS_S_OK on success
                 */
                virtual DasResult CreateRemoteProxy(
                    ObjectId       object_id,
                    const DasGuid& iid,
                    IDasBase**     pp_out) = 0;

                /**
                 * @brief Resolve a main process global service interface by IID
                 *
                 * - Host path: sends LOOKUP_BY_INTERFACE to main process,
                 * creates remote proxy
                 * - Main path: directly looks up local object from
                 * DistributedObjectManager
                 *
                 * @param iid Interface GUID of the desired service
                 * @param pp_out_object [out] Receives the interface pointer
                 * (caller must Release)
                 * @return DasResult DAS_S_OK on success
                 */
                virtual DasResult ResolveMainProcessInterface(
                    const DasGuid& iid,
                    IDasBase**     pp_out_object) = 0;

                /**
                 * @brief 注册一个服务对象到主进程全局服务表
                 * @param p_object 服务对象指针
                 * @param iid 对象实现的接口 IID（作为唯一键）
                 * @return DasResult DAS_S_OK 成功
                 */
                virtual DasResult RegisterService(
                    IDasBase*        p_object,
                    const DasGuid&   iid) = 0;

                /**
                 * @brief 从主进程全局服务表注销一个服务对象
                 * @param iid 要注销的服务 IID
                 * @return DasResult DAS_S_OK 成功
                 */
                virtual DasResult UnregisterService(
                    const DasGuid&   iid) = 0;

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
             * @param enable_heartbeat
             * 是否启用心跳线程（调试时可禁用，避免超时杀进程）
             * @return IIpcContext* 上下文指针，失败返回 nullptr
             */
            DAS_API IIpcContext* CreateIpcContext(bool enable_heartbeat = true);

            /**
             * @brief 便捷创建函数（返回 unique_ptr）
             * @param enable_heartbeat 是否启用心跳线程
             * @return IpcContextPtr 自动管理生命周期的智能指针
             */
            inline IpcContextPtr CreateIpcContextEz(
                bool enable_heartbeat = true)
            {
                return IpcContextPtr{CreateIpcContext(enable_heartbeat)};
            }

            /**
             * @brief 便捷创建函数（返回 shared_ptr）
             * @param enable_heartbeat 是否启用心跳线程
             * @return std::shared_ptr<IIpcContext> 可共享的智能指针
             */
            inline std::shared_ptr<IIpcContext> CreateIpcContextShared(
                bool enable_heartbeat = true)
            {
                return std::shared_ptr<IIpcContext>(
                    CreateIpcContext(enable_heartbeat),
                    DestroyIpcContext);
            }

        } // namespace MainProcess
    } // namespace IPC
} // namespace Core
DAS_NS_END
