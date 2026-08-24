#ifndef DAS_CORE_IPC_ADD_REF_DISPATCH_GUARD_H
#define DAS_CORE_IPC_ADD_REF_DISPATCH_GUARD_H

#include <das/Core/IPC/IpcRunLoop.h>

DAS_CORE_IPC_NS_BEGIN

/**
 * @brief 周期末 ADD_REF pending 出清 guard（闸3）
 *
 * 构造于两个分发漏斗入口（BusinessThread::ProcessInboundMessage 与
 * IpcRunLoop::DispatchToHandlerWithSender），析构时 pending 队列非空则
 * FlushPendingStandalone 独立发出全部帧（逐帧 PostSend，session 内
 * FIFO）；空队列零开销直通（一次互斥锁空查）。这保证入队后"同向下一帧"
 * 与"周期结束"必有先到者——滞留上界 = 周期末（非 BT 物化不经队列
 * 当场直发，滞留上界为零）。
 *
 * 挂点定案两处：BT 漏斗覆盖嵌套 pump（外层函数帧的 guard 在整个嵌套
 * 处理期间存活，内层 guard 先析构先出清）；IO 控制面漏斗为防御性挂点
 * （今日该路径零物化）。保险带（1000ms 心跳帧过闸1 搭车）由后续
 * 搭车机制提供。
 */
class AddRefDispatchGuard
{
public:
    explicit AddRefDispatchGuard(IpcRunLoop& run_loop) : run_loop_(run_loop) {}

    ~AddRefDispatchGuard()
    {
        if (!run_loop_.PendingAddRefsEmpty())
        {
            run_loop_.FlushPendingStandalone();
        }
    }

    AddRefDispatchGuard(const AddRefDispatchGuard&) = delete;
    AddRefDispatchGuard& operator=(const AddRefDispatchGuard&) = delete;
    AddRefDispatchGuard(AddRefDispatchGuard&&) = delete;
    AddRefDispatchGuard& operator=(AddRefDispatchGuard&&) = delete;

private:
    IpcRunLoop& run_loop_;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_ADD_REF_DISPATCH_GUARD_H
