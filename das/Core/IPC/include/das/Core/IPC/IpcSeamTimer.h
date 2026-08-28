#ifndef DAS_CORE_IPC_IPC_SEAM_TIMER_H
#define DAS_CORE_IPC_IPC_SEAM_TIMER_H

// ┌ 性能调查专用计时 seam（非生产特性）┐
// CMake 开关 DAS_IPC_TRACE_SEAMS=ON 时激活（das/Core/CMakeLists.txt 定义
// 宏）：主进程侧记录外部同步调用的完整往返（M_RTT），宿主进程侧按
// call_id 配对「请求到达路由 → 响应入队」的服务时长（H_SERVICE）。
// 进程退出时向 stderr 打印聚合均值，供全链路分段归因。关闭时所有
// 调用点经编译期常量折叠消除，零开销。

#include <das/Core/IPC/IpcErrors.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

DAS_CORE_IPC_NS_BEGIN

namespace IpcSeam
{
    inline bool Enabled()
    {
#ifdef DAS_IPC_TRACE_SEAMS
        return true;
#else
        return false;
#endif
    }

    struct Accumulator
    {
        std::atomic<uint64_t> count{0};
        std::atomic<uint64_t> sum_ns{0};
        std::atomic<uint64_t> max_ns{0};

        void Record(uint64_t delta_ns)
        {
            count.fetch_add(1, std::memory_order_relaxed);
            sum_ns.fetch_add(delta_ns, std::memory_order_relaxed);
            auto prev_max = max_ns.load(std::memory_order_relaxed);
            while (delta_ns > prev_max
                   && !max_ns.compare_exchange_weak(
                       prev_max,
                       delta_ns,
                       std::memory_order_relaxed))
            {
            }
        }
    };

    inline Accumulator& MainRtt()
    {
        static Accumulator accumulator;
        return accumulator;
    }

    inline Accumulator& HostService()
    {
        static Accumulator accumulator;
        return accumulator;
    }

    // 客户端发送同步段：PostSend 调用→返回（序列化+入队+drain 触发）
    inline Accumulator& ClientSend()
    {
        static Accumulator accumulator;
        return accumulator;
    }

    // 客户端响应收尾段：IO 线程开始处理响应帧→完成回调调用完
    inline Accumulator& ClientDispatch()
    {
        static Accumulator accumulator;
        return accumulator;
    }

    inline uint64_t NowNs()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    // 进程级一次性退出上报（首次记录时注册）
    inline void EnsureExitReportRegistered()
    {
        static const bool registered = []
        {
            std::atexit(
                []
                {
                    const auto dump =
                        [](const char* tag, const Accumulator& acc)
                    {
                        const auto count = acc.count.load();
                        if (count == 0)
                        {
                            return;
                        }
                        const double mean_us =
                            static_cast<double>(acc.sum_ns.load()) / 1000.0
                            / static_cast<double>(count);
                        const double max_us =
                            static_cast<double>(acc.max_ns.load()) / 1000.0;
                        std::fprintf(
                            stderr,
                            "IPC-SEAM %s n=%llu mean_us=%.2f max_us=%.2f\n",
                            tag,
                            static_cast<unsigned long long>(count),
                            mean_us,
                            max_us);
                    };
                    dump("M_RTT", MainRtt());
                    dump("H_SERVICE", HostService());
                    dump("C_SEND", ClientSend());
                    dump("C_DISPATCH", ClientDispatch());
                });
            return true;
        }();
        (void)registered;
    }

    inline uint64_t BeginSpan() { return Enabled() ? NowNs() : 0; }
} // namespace IpcSeam

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_IPC_SEAM_TIMER_H
