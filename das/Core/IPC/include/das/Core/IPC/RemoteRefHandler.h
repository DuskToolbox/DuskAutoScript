#ifndef DAS_CORE_IPC_REMOTE_REF_HANDLER_H
#define DAS_CORE_IPC_REMOTE_REF_HANDLER_H

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <das/Core/IPC/Config.h>
#include <das/Core/IPC/IMessageHandler.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/Utils/CommonUtils.hpp>
#include <vector>

DAS_CORE_IPC_NS_BEGIN

class DistributedObjectManager;
class IpcRunLoop;

/**
 * @brief CONTROL_PLANE IO 域远端引用记账 handler
 *
 * 处理对端物化申报（RemoteAddRef）与释放（RemoteRelease）的
 * fire-and-forget EVENT：
 * - holder 身份取 header.GetSourceSessionId()（body 只承载 ObjectId）；
 * - 记账为纯 DOM 表操作（free-threaded shared_mutex，IO 协程域合法）；
 * - 条目移除时对象本体的 final Release 包装 DasPtr 经
 *   IpcRunLoop::PostToBusinessThread 桥接回 BusinessThread 执行
 *   （线程模型不变量：真实对象析构不落在 IO 线程）；
 * - 条目不存在 / 打空的迟到消息按 D-14 known-edge 处理：
 *   WARN 日志（oid 三元组 + source session）后忽略，不崩溃不重建条目。
 *
 * 76.3 红线：本 handler 不触碰远端对象注册表、proxy 缓存，也不构造
 * response——CONTROL_PLANE IO 路径只允许 IO-safe 状态与 IAwaitableMessageHandler
 * 形态。
 *
 * @note 线程安全：HandleMessage 仅在 IO 协程域执行；DOM 表操作由其内部
 *       shared_mutex 保护。
 */
class RemoteRefHandler final : public IAwaitableMessageHandler
{
public:
    /**
     * @brief 工厂函数：创建 RemoteRefHandler 实例
     *
     * @param object_manager owner 侧 DOM（holder 记账目标）
     * @param run_loop IO 运行时（条目移除时 final Release 回 BT 的桥）
     * @return DasPtr<RemoteRefHandler> 智能指针
     */
    [[nodiscard]]
    static DasPtr<RemoteRefHandler> Create(
        DistributedObjectManager& object_manager,
        IpcRunLoop&               run_loop);

    ~RemoteRefHandler() override;

    // 禁止拷贝
    RemoteRefHandler(const RemoteRefHandler&) = delete;
    RemoteRefHandler& operator=(const RemoteRefHandler&) = delete;

    DAS_UTILS_IDASBASE_AUTO_IMPL(RemoteRefHandler)

    /**
     * @brief 处理 CONTROL_PLANE RemoteAddRef/RemoteRelease EVENT
     *
     * RemoteAddRef：DOM AddRemoteHolder(oid, source session)。
     * RemoteRelease：DOM ReleaseRemoteHolder(oid, source session)；
     * 条目被移除时对象本体唯一一次移交出参并桥接 BT 析构。
     * 两者均 fire-and-forget：不发送 response，未知条目仅 WARN。
     *
     * @param header 消息头（interface_id = 130/132）
     * @param body 消息体（ObjectId 8B）
     * @param sender 响应发送器（本 handler 不使用）
     * @param context IO-safe 控制平面上下文（本 handler 不使用）
     * @return boost::asio::awaitable<DasResult> 协程结果
     */
    boost::asio::awaitable<DasResult> HandleMessage(
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body,
        IpcResponseSender&               sender,
        ControlPlaneContext&             context) override;

    [[nodiscard]]
    uint32_t GetInterfaceId() const noexcept override
    {
        // 本 handler 按 (header_flags, interface_id) 显式注册到两个命令，
        // 不经 IpcRunLoop::GetHandler() 单接口查找
        return 0;
    }

private:
    explicit RemoteRefHandler(
        DistributedObjectManager& object_manager,
        IpcRunLoop&               run_loop);

    DistributedObjectManager& object_manager_;

    // final Release 桥接 BusinessThread 时使用（PostToBusinessThread）
    [[maybe_unused]]
    IpcRunLoop& run_loop_;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_REMOTE_REF_HANDLER_H
