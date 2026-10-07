#pragma once

#include "PiHttpClient.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace Das::Plugins::DasMaaPi
{
    // per-pi_dir 更新 job slot。running 由 job 线程经 Finish 唯一写 false；
    // worker 持有本结构（shared_ptr），slot 复用不自删除（job 线程结束时
    // erase map 条目会在 worker 自身栈上 join 自己 → std::terminate）。
    // cancel_requested 由 registry 析构收尾 / RequestCancel 置 true，job 体
    // 经 cancel 观察缝（下载失败分支 / 解压循环）消费；替换阶段不观察取消
    // （全有或全无）。replace_stage 由 apply job 的阶段回调在首个 Replace
    // 进度时置 true（TryStart 复用 slot 时复位），置位后 RequestCancel 拒绝
    // 取消——替换中段取消会留下混合坏态且侧备份已逐文件删除，无法自动回滚。
    struct UpdateJobSlot
    {
        std::atomic<bool>             running{false};
        std::atomic<bool>             cancel_requested{false};
        std::atomic<bool>             replace_stage{false};
        std::thread                   worker;
        std::shared_ptr<PiHttpClient> http;
    };

    // RequestCancel 结果三态：NotFound = 该 pi_dir 无 running job（含 slot
    // 不存在）；ReplaceStage = 不可回退点拒绝（job 已进入替换阶段）；
    // Cancelled = 取消已受理（标志置位 + 在途 HTTP 中断，job 以取消终态
    // 收尾，终态经 SSE 推送）。
    enum class UpdateCancelOutcome
    {
        Cancelled,
        NotFound,
        ReplaceStage
    };

    // 进程级更新 job registry（函数级 static 单例，DLL 卸载析构触发收尾）。
    // 键 = pi_dir（一个 Host 可承载多个 PI 项目，每个 distinct pi_dir 一个
    // slot；map 大小上界 = distinct pi_dir 数，无需主动清理）。
    // 线程契约：TryStart/IsRunning 在 IPC BusinessThread 调用；Finish 由
    // job 线程调用；WaitForSlotIdleForTest 供测试有界等待。
    class UpdateJobRegistry final
    {
    public:
        // job 线程体：registry 在持锁构造 worker 时以 slot 为实参调用（只有
        // TryStart 能创建/复用 slot，调用方无法提前捕获）。
        using JobBody =
            std::function<void(const std::shared_ptr<UpdateJobSlot>&)>;

        static UpdateJobRegistry& Instance();

        UpdateJobRegistry(const UpdateJobRegistry&) = delete;
        UpdateJobRegistry& operator=(const UpdateJobRegistry&) = delete;

        // 已有该 pi_dir 的 running job → nullptr（防重入）；否则复用或新建
        // slot（复用前对已结束的旧 worker join——立即返回），锁内完成
        // running=true + http 存档 + worker 线程构造。不变量：
        // running==true ⇒ worker.joinable()==true（线程构造不外泄到锁外，
        // 析构快照不可能撞上「running 已置而 worker 未赋」的跳 join 窗口）。
        // 线程构造失败（资源耗尽）时回滚 running 并返回 nullptr。
        std::shared_ptr<UpdateJobSlot> TryStart(
            const std::filesystem::path&  pi_dir,
            std::shared_ptr<PiHttpClient> http,
            JobBody                       job_body);

        bool IsRunning(const std::filesystem::path& pi_dir) const;

        // 请求取消该 pi_dir 的 running job（applyUpdate toggle 语义下运行中
        // 再点 = 本调用）。锁内分流：无 running job → NotFound；replace_stage
        // 已置位 → ReplaceStage（不可回退点拒绝）；否则 cancel_requested=true
        // + slot->http->RequestCancel()（中断在途 HTTP）→ Cancelled。
        // http 的取消实现线程安全（可与执行请求的线程并发调用），在锁内
        // 触达不构成锁序风险（http 不回调 registry）。
        UpdateCancelOutcome RequestCancel(const std::filesystem::path& pi_dir);

        // job 线程唯一写者：置 running=false + notify（绝不 erase slots_
        // 条目、绝不 reset slot 的 shared_ptr）。
        void Finish(const std::shared_ptr<UpdateJobSlot>& slot);

        // 测试缝：有界等待 slot running=false（wait_for，绝不无限等）。
        // slot 不存在视为 idle。
        bool WaitForSlotIdleForTest(
            const std::filesystem::path& pi_dir,
            std::chrono::milliseconds    timeout);

        // 对所有 running slot 先置 cancel_requested（job 体的解压/替换循环
        // 观察到即走失败终态收尾，使本地 IO 阶段的 join 有界）再
        // http->RequestCancel()（中断在途 HTTP）后 join worker；未 running
        // 的 slot 只 join（worker 已结束，立即返回，且不 dereference
        // http——别名构造的测试 fake 可能已析构）。
        ~UpdateJobRegistry();

    private:
        UpdateJobRegistry() = default;

        mutable std::mutex                                    mutex_;
        std::condition_variable                               idle_cv_;
        std::map<std::string, std::shared_ptr<UpdateJobSlot>> slots_;
    };
} // namespace Das::Plugins::DasMaaPi
