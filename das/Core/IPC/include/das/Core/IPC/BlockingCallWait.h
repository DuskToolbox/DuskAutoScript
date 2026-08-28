#ifndef DAS_CORE_IPC_BLOCKING_CALL_WAIT_H
#define DAS_CORE_IPC_BLOCKING_CALL_WAIT_H

#include <das/Core/IPC/IpcErrors.h>
#include <das/Core/IPC/IpcRunLoop.h>
#include <das/Core/IPC/IpcSeamTimer.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

DAS_CORE_IPC_NS_BEGIN

namespace detail
{
    // 同步调用的完成状态盒：IO 线程完成回调写入，调用线程自旋或停车等待。
    // 完成序：持锁写结果 + release 存 done 标志；自旋读者 acquire 读标志后经
    // 互斥量取结果，停车路径由条件变量与同一把锁保证。
    struct BlockingCallCompletionState
    {
        // IO 线程侧完成入口（作为 PostSend 的完成回调）
        void Complete(
            DasResult            result,
            std::vector<uint8_t> response_body,
            uint16_t             response_flags)
        {
            // notify 必须持锁发出：调用线程可能经自旋路径零阻塞取走结果并
            // 使本状态盒析构（栈对象），锁内通知保证通知方的最后一次触碰与
            // 等待方取结果构成 happens-before，杜绝「析构 vs notify」竞态
            const std::lock_guard<std::mutex> lock(mutex_);
            result_ = std::make_optional(
                std::make_tuple(
                    result,
                    std::move(response_body),
                    response_flags));
            done_.store(true, std::memory_order_release);
            cv_.notify_all();
        }

        bool IsDone() const noexcept
        {
            return done_.load(std::memory_order_acquire);
        }

        // 自旋耗尽后的停车等待：谓词检查 done，醒来后取走结果
        std::optional<std::tuple<DasResult, std::vector<uint8_t>, uint16_t>>
        WaitForCompletion()
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(
                lock,
                [this]() { return done_.load(std::memory_order_acquire); });
            return std::move(result_);
        }

    private:
        mutable std::mutex      mutex_;
        std::condition_variable cv_;
        std::optional<std::tuple<DasResult, std::vector<uint8_t>, uint16_t>>
                          result_;
        std::atomic<bool> done_{false};
    };
} // namespace detail

// 自旋窗口参数：响应经 pending_calls_ fast-path 直达完成回调，期望到达时延
// 在数十微秒级——先自旋吃掉 CV 停车-唤醒延迟（Windows 上典型 5~20us），预算
// 耗尽回落到条件变量停车。批次内 _mm_pause 放松流水线，周期性 yield 让出核
// 避免在满载机器上烧 CPU。
inline constexpr unsigned RESPONSE_WAIT_PAUSE_BATCH_SIZE = 32;
inline constexpr unsigned RESPONSE_WAIT_MAX_SPIN_BATCHES = 384;
inline constexpr unsigned RESPONSE_WAIT_YIELD_EVERY_N_BATCHES = 16;

// 发送请求并同步等待响应（bounded spin-then-park）。
//
// PostSend(带回调) 直接展开替代 AwaitResponseSender + stdexec::sync_wait：
// 完成语义一致（IO 线程注册 pending 后发送、pending_calls_ fast-path 回填，
// PostSend 失败路径同步完成回调），返回形态保持
// optional<tuple<DasResult, vector<uint8_t>, uint16_t>>。
inline std::optional<std::tuple<DasResult, std::vector<uint8_t>, uint16_t>>
StartAndSpinWaitResponseCall(
    IpcRunLoop&                      run_loop,
    const ValidatedIPCMessageHeader& header,
    std::vector<uint8_t>&&           request_body,
    std::chrono::milliseconds        timeout)
{
    detail::BlockingCallCompletionState completion_state;

    const uint64_t seam_start_ns = IpcSeam::BeginSpan();

    const uint64_t seam_send_begin_ns =
        seam_start_ns != 0 ? IpcSeam::NowNs() : 0;

    static_cast<void>(run_loop.PostSend(
        header,
        std::move(request_body),
        [&completion_state, seam_start_ns](
            DasResult            result,
            std::vector<uint8_t> response_body,
            uint16_t             response_flags)
        {
            if (seam_start_ns != 0)
            {
                IpcSeam::MainRtt().Record(IpcSeam::NowNs() - seam_start_ns);
                IpcSeam::EnsureExitReportRegistered();
            }
            completion_state.Complete(
                result,
                std::move(response_body),
                response_flags);
        },
        std::chrono::steady_clock::now() + timeout));

    if (seam_send_begin_ns != 0)
    {
        IpcSeam::ClientSend().Record(IpcSeam::NowNs() - seam_send_begin_ns);
    }

    unsigned pause_batches_remaining = RESPONSE_WAIT_MAX_SPIN_BATCHES;
    while (pause_batches_remaining > 0 && !completion_state.IsDone())
    {
        for (unsigned pause_index = 0;
             pause_index < RESPONSE_WAIT_PAUSE_BATCH_SIZE;
             ++pause_index)
        {
            if (completion_state.IsDone())
            {
                break;
            }
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)                \
    || defined(_M_IX86)
            _mm_pause();
#else
            std::this_thread::yield();
#endif
        }
        --pause_batches_remaining;
        const auto batches_consumed =
            RESPONSE_WAIT_MAX_SPIN_BATCHES - pause_batches_remaining;
        if (batches_consumed % RESPONSE_WAIT_YIELD_EVERY_N_BATCHES == 0)
        {
            std::this_thread::yield();
        }
    }

    // 自旋内命中则谓词立刻通过；未命中则以同一谓词停车等待到完成
    return completion_state.WaitForCompletion();
}

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_BLOCKING_CALL_WAIT_H
