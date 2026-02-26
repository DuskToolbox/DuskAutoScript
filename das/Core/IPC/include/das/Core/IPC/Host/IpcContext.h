#ifndef DAS_CORE_IPC_HOST_IPC_CONTEXT_H
#define DAS_CORE_IPC_HOST_IPC_CONTEXT_H

#include <das/Core/IPC/Host/IIpcContext.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/DasApi.h>
#include <filesystem>
#include <memory>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

DAS_NS_BEGIN
namespace Core
{
    namespace IPC
    {
        // 前置声明（在 IPC 命名空间内）
        class IpcRunLoop;
        class IpcCommandHandler;

        namespace Host
        {
            class IpcContextImpl;

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
                 * @brief 设置握手完成回调
                 *
                 * @param handler 回调函数
                 * @param user_data 用户数据
                 */
                void SetOnHandshakeComplete(
                    OnHandshakeComplete handler,
                    void*               user_data);

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

            private:
                std::unique_ptr<IpcContextImpl> impl_;
            };

        } // namespace Host
    } // namespace IPC
} // namespace Core
DAS_NS_END

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif // DAS_CORE_IPC_HOST_IPC_CONTEXT_H
