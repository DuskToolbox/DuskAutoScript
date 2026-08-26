#ifndef DAS_CORE_IPC_ADD_REF_DISPATCH_GUARD_H
#define DAS_CORE_IPC_ADD_REF_DISPATCH_GUARD_H

#include <das/Core/IPC/IpcRunLoop.h>

DAS_CORE_IPC_NS_BEGIN

/**
 * @brief 周期末出站 FIFO 出清 guard（drain 兜底触发）
 *
 * 构造于两个分发漏斗入口（BusinessThread::ProcessInboundMessage 与
 * IpcRunLoop::DispatchToHandlerWithSender），析构时对全部有滞留帧的
 * session 触发一次 drain（合写形态出清）；空队列零开销直通（一次互斥
 * 锁空查）。这保证入队后"同向下一帧"与"周期结束"必有先到者——控制
 * 帧滞留上界 = 一个 dispatch 周期（非 BT 线程的控制帧入队即触发，滞留
 * 上界为零）。
 *
 * 挂点定案两处：BT 漏斗覆盖嵌套 pump（外层函数帧的 guard 在整个嵌套
 * 处理期间存活，内层 guard 先析构先出清）；IO 控制面漏斗为防御性挂点
 * （今日该路径零物化）。零流量保险带（1000ms 心跳班车入队即触发）由
 * ConnectionManager 心跳线程提供。
 */
class AddRefDispatchGuard
{
public:
    explicit AddRefDispatchGuard(IpcRunLoop& run_loop) : run_loop_(run_loop) {}

    ~AddRefDispatchGuard() { run_loop_.FlushOutboundFifo(); }

    AddRefDispatchGuard(const AddRefDispatchGuard&) = delete;
    AddRefDispatchGuard& operator=(const AddRefDispatchGuard&) = delete;
    AddRefDispatchGuard(AddRefDispatchGuard&&) = delete;
    AddRefDispatchGuard& operator=(AddRefDispatchGuard&&) = delete;

private:
    IpcRunLoop& run_loop_;
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_ADD_REF_DISPATCH_GUARD_H
