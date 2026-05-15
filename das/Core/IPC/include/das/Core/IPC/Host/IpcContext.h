#ifndef DAS_CORE_IPC_HOST_IPC_CONTEXT_H
#define DAS_CORE_IPC_HOST_IPC_CONTEXT_H

#include <atomic>
#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <das/Core/IPC/DefaultAsyncIpcTransport.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/Host/HandshakeHandler.h>
#include <das/Core/IPC/Host/IIpcContext.h>
#include <das/Core/IPC/HttpIpcClient.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcMessageQueue.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/Core/IPC/SharedMemoryPool.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <memory>
#include <optional>
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
             * - DistributedObjectManager：分布式对象生命周期管理
             * - IpcRunLoop：IPC 事件循环
             * - IpcCommandHandler：IPC 命令处理器
             */
            class IpcContext : public IIpcContext, public IResolveContext
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
                class IpcRunLoop& GetRunLoop() DAS_LIFETIMEBOUND;

                /**
                 * @brief 获取 IpcCommandHandler 实例
                 * @return IpcCommandHandler& 命令处理器实例
                 */
                class IpcCommandHandler& GetCommandHandler() DAS_LIFETIMEBOUND;

                /**
                 * @brief 获取 IDistributedObjectManager 实例
                 * @return IDistributedObjectManager& 对象管理器实例
                 */
                struct IDistributedObjectManager& GetObjectManager()
                    DAS_LIFETIMEBOUND override;

                /**
                 * @brief 启动事件循环
                 * @return DasResult 启动结果
                 */
                DasResult Run() override;

                /**
                 * @brief 请求停止事件循环
                 */
                void RequestStop() override;

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
                    CommandHandler handler) override;

                bool IsConnected() const override;

                void PostCallback(IDasAsyncCallback* callback) override;

                DasResult RegisterLocalObject(
                    IDasBase* object_ptr,
                    ObjectId& out_object_id) override;

                DasResult ResolveMainProcessInterface(
                    const DasGuid& iid,
                    IDasBase**     pp_out_object) override;

                DasResult ResolveMainProcessInterfaceByName(
                    const char* name,
                    IDasBase**  pp_out_object) override;

                DasResult RegisterService(
                    IDasBase*      p_object,
                    const DasGuid& iid) override;

                DasResult UnregisterService(const DasGuid& iid) override;

                // IResolveContext ByName overrides
                // ResolveMainProcessInterfaceByName: single override covers
                // both IIpcContext and IResolveContext (same signature)

                DasResult RegisterServiceByName(
                    IDasBase*      p_object,
                    const DasGuid& iid,
                    const char*    name) override
                {
                    (void)p_object;
                    (void)iid;
                    (void)name;
                    return DAS_E_NO_IMPLEMENTATION;
                }

                DasResult UnregisterServiceByName(const char* name) override
                {
                    (void)name;
                    return DAS_E_NO_IMPLEMENTATION;
                }

            private:
                boost::asio::awaitable<void> ReceiveLoopCoroutine();

                /// HTTP/WebSocket 接收循环协程
                boost::asio::awaitable<void> HttpReceiveLoopCoroutine();

                void StartParentProcessMonitor();
                void StopParentProcessMonitor();

                IpcContextConfig config_;
                uint16_t         session_id_ = 0;

                /// 远程对象注册表（值成员）
                RemoteObjectRegistry registry_;

                /// 入站消息队列（值成员）-- 在 run_loop_ 之前声明，确保先初始化
                IpcMessageQueue<InboundMessage> inbound_queue_{1024};

                /// Proxy 工厂（值持有 DistributedObjectManager，optional 因为
                /// ProxyFactory 引用非默认可构造）
                /// 必须在 run_loop_ 之前声明（IpcRunLoop 构造需要
                /// ProxyFactory&）
                std::optional<ProxyFactory> proxy_factory_;

                /// IPC 运行循环（值成员，构造即初始化）
                IpcRunLoop run_loop_;

                /// 业务线程
                std::shared_ptr<BusinessThread> business_thread_;

                DasPtr<IpcCommandHandler> command_handler_;
                DasPtr<HandshakeHandler>  handshake_handler_;

                /// 共享内存池（optional，因为 shm_name 在构造函数体中计算）
                std::optional<SharedMemoryPool> shared_memory_;

                /// 异步传输层（unique_ptr，因为 Transport 构造函数是私有的，
                /// 只能通过 CreateUninitialized 工厂方法创建，无法原地构造）
                std::unique_ptr<DefaultAsyncIpcTransport> async_transport_;

                /// HTTP/WebSocket 客户端（HTTP 传输模式）
                std::unique_ptr<HttpIpcClient> http_client_;

                /// HTTP 传输层非拥有指针（所有权已转移至 ConnectionManager）
                HttpIpcTransport* http_transport_ = nullptr;

                /// HTTP 连接参数（从 connect_url 解析）
                std::string http_host_;
                std::string http_port_;

                bool is_connected_ = false;
                bool is_running_ = false;

                uint32_t host_pid_ = 0;
                uint32_t main_pid_ = 0;

                std::string host_read_queue_;
                std::string host_write_queue_;
                bool        host_is_server_ = false;

                /// Whether HTTP/WebSocket transport mode is active
                bool use_http_transport_ = false;

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
