#ifndef DAS_CORE_IPC_MAIN_PROCESS_IPC_CONTEXT_H
#define DAS_CORE_IPC_MAIN_PROCESS_IPC_CONTEXT_H

#include <atomic>
#include <bitset>
#include <chrono>
#include <das/Core/IPC/BusinessThread.h>
#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/HttpIpcServer.h>
#include <das/Core/IPC/HttpIpcTransport.h>
#include <das/Core/IPC/InternalCallbackHandler.h>
#include <das/Core/IPC/IpcCommandHandler.h>
#include <das/Core/IPC/IpcMessageQueue.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/Core/IPC/ProxyFactory.h>
#include <das/Core/IPC/RemoteObjectRegistry.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/IDasAsyncLoadPluginOperation.h>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        namespace MainProcess
        {
        } // namespace MainProcess

        // 前置声明（在 IPC 命名空间内）
        class HostLauncher;

        namespace MainProcess
        {
            /**
             * @brief 主进程 IPC 上下文实现
             *
             * RAII 风格：构造即初始化，析构即清理
             * 封装主进程端所有 IPC 组件的初始化和管理
             *
             * 管理的组件：
             * - DistributedObjectManager：分布式对象生命周期管理
             * - ProxyFactory：Proxy 工厂，负责创建远程对象代理
             * - RemoteObjectRegistry：远程对象注册表
             * - IpcRunLoop：IPC 运行循环（持有 ConnectionManager）
             */
            class IpcContext final : public IIpcContext, public IResolveContext
            {
            public:
                explicit IpcContext(bool enable_heartbeat);
                /// HTTP/WebSocket transport mode constructor
                explicit IpcContext(
                    bool                  enable_heartbeat,
                    HttpIpcServer::Config http_config);
                ~IpcContext() override; // 自动清理

                IpcContext(const IpcContext&) = delete;
                IpcContext& operator=(const IpcContext&) = delete;
                IpcContext(IpcContext&&) = delete;
                IpcContext& operator=(IpcContext&&) = delete;

                ProxyFactory& GetProxyFactory() DAS_LIFETIMEBOUND;

                DasResult CreateHostLauncher(
                    IHostLauncher** pp_out_launcher) override;

                DasResult LoadPluginAsync(
                    IHostLauncher*                 host_launcher,
                    const char*                    u8_plugin_path,
                    IDasAsyncLoadPluginOperation** pp_out_operation,
                    std::chrono::milliseconds      timeout =
                        std::chrono::seconds(30)) override;

                DasResult LoadPluginAsync(
                    uint16_t                       target_session_id,
                    const char*                    u8_plugin_path,
                    IDasAsyncLoadPluginOperation** pp_out_operation,
                    std::chrono::milliseconds      timeout =
                        std::chrono::seconds(30)) override;

                DasResult ListFilesAsync(
                    uint16_t                  session_id,
                    const char*               u8_relative_path,
                    bool                      recursive,
                    FileListCallback          callback,
                    std::chrono::milliseconds timeout =
                        std::chrono::seconds(10)) override;

                DasResult ReadFileAsync(
                    uint16_t                  session_id,
                    const char*               u8_relative_path,
                    FileContentCallback       callback,
                    std::chrono::milliseconds timeout =
                        std::chrono::seconds(10)) override;

                void PostCallback(IDasAsyncCallback* callback) override;

                void PostToBusinessThread(IDasAsyncCallback* callback) override;

                DasResult Run() override;
                void      RequestStop() override;

                boost::asio::io_context& GetIoContext() override;

                uint16_t AllocateSessionId() override;
                void     ReleaseSessionId(uint16_t session_id) override;

                DasResult CreateRemoteProxy(
                    ObjectId       object_id,
                    const DasGuid& iid,
                    IDasBase**     pp_out) override;

                DasResult ResolveMainProcessInterface(
                    const DasGuid& iid,
                    IDasBase**     pp_out_object) override;

                DasResult RegisterService(
                    IDasBase*      p_object,
                    const DasGuid& iid) override;

                DasResult UnregisterService(const DasGuid& iid) override;

                DasResult ResolveMainProcessInterfaceByName(
                    const char* name,
                    IDasBase**  pp_out_object) override;

                DasResult RegisterServiceByName(
                    IDasBase*      p_object,
                    const DasGuid& iid,
                    const char*    name) override;

                DasResult UnregisterServiceByName(const char* name) override;

                /// Internal registration method (called by HostLauncher after
                /// Start succeeds)
                DasResult InternalRegisterHostLauncher(uint16_t session_id);

            private:
                /// IPC 命令处理器
                DasPtr<IpcCommandHandler> command_handler_;

                /// 内部回调处理器（PostToBusinessThread 用）
                DasPtr<InternalCallbackHandler> internal_callback_handler_;

                /// 远程对象注册表（值成员）
                RemoteObjectRegistry registry_;

                /// 入站消息队列（值成员）-- 在 runloop_ 之前声明，确保先初始化
                IpcMessageQueue<InboundMessage> inbound_queue_{1024};

                /// Proxy 工厂（值持有 DistributedObjectManager，optional 因为
                /// ProxyFactory 引用非默认可构造）
                /// 必须在 runloop_ 之前声明（IpcRunLoop 构造需要
                /// ProxyFactory&）
                std::optional<ProxyFactory> proxy_factory_;

                /// IPC 运行循环（值成员，构造即初始化）
                IpcRunLoop runloop_;

                /// 业务线程
                std::shared_ptr<BusinessThread> business_thread_;

                /// Created launchers indexed by session_id
                std::unordered_map<uint16_t, DAS::DasPtr<HostLauncher>>
                    launchers_;

                /// HTTP/WebSocket IPC server (optional, for HTTP transport
                /// mode)
                std::unique_ptr<HttpIpcServer> http_server_;

                /// HTTP accepted session IDs allocated by this context.
                std::unordered_set<uint16_t> http_session_ids_;

                std::atomic<uint16_t> session_id_cursor_{2};
                mutable std::mutex    allocated_ids_mutex_;
                std::bitset<65536>    allocated_ids_{};
                static const uint16_t reserved_session_ids_[3];

                uint16_t FindAvailableSessionId();
                void     MarkSessionIdAsAllocated(uint16_t session_id);
                void     MarkSessionIdAsFree(uint16_t session_id);
                void     TrackHttpSessionId(uint16_t session_id);
                void     ReleaseHttpSessionId(uint16_t session_id);

                /// RegisterService 的实际实现（在 BusinessThread 上执行）
                DasResult RegisterServiceImpl(
                    IDasBase*      p_object,
                    const DasGuid& iid,
                    const char*    custom_name = nullptr);

                /// UnregisterService 的实际实现（在 BusinessThread 上执行）
                DasResult UnregisterServiceImpl(const DasGuid& iid);

                /// UnregisterServiceByName 的实际实现（在 BusinessThread
                /// 上执行）
                DasResult UnregisterServiceByNameImpl(const std::string& name);

                /// ResolveMainProcessInterface 的实际实现（在 BusinessThread
                /// 上执行）
                DasResult ResolveMainProcessInterfaceImpl(
                    const DasGuid& iid,
                    IDasBase**     pp_out_object);

                /// ResolveMainProcessInterfaceByName 的实际实现（在
                /// BusinessThread 上执行）
                DasResult ResolveMainProcessInterfaceByNameImpl(
                    const std::string& name,
                    IDasBase**         pp_out_object);
            };

        } // namespace MainProcess
    } // namespace IPC
} // namespace Core
DAS_NS_END

#endif // DAS_CORE_IPC_MAIN_PROCESS_IPC_CONTEXT_H
