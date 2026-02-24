#pragma once
#include <das/DasApi.h>
#include <memory>

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
            class IIpcContext;

            // 前置声明 DestroyIpcContext，用于友元
            DAS_API void DestroyIpcContext(IIpcContext* ctx);

            /**
             * @brief 主进程 IPC 上下文接口
             *
             * 纯虚函数接口，protected 析构函数防止用户直接 delete。
             * 使用 CreateIpcContext() 创建实例。
             */
            class IIpcContext
            {
            public:
                virtual MainProcessServer&        GetServer() = 0;
                virtual DistributedObjectManager& GetObjectManager() = 0;
                virtual ProxyFactory&             GetProxyFactory() = 0;
                virtual RemoteObjectRegistry&     GetRegistry() = 0;

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

        } // namespace MainProcess
    } // namespace IPC
} // namespace Core
DAS_NS_END
