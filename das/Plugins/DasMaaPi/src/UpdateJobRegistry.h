#pragma once

#include "PiHttpClient.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
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
    struct UpdateJobSlot
    {
        std::atomic<bool>             running{false};
        std::thread                   worker;
        std::shared_ptr<PiHttpClient> http;
    };

    // 进程级更新 job registry（函数级 static 单例，DLL 卸载析构触发收尾）。
    // 键 = pi_dir（一个 Host 可承载多个 PI 项目，每个 distinct pi_dir 一个
    // slot；map 大小上界 = distinct pi_dir 数，无需主动清理）。
    // 线程契约：TryStart/IsRunning 在 IPC BusinessThread 调用；Finish 由
    // job 线程调用；WaitForSlotIdleForTest 供测试有界等待。
    class UpdateJobRegistry final
    {
    public:
        static UpdateJobRegistry& Instance();

        UpdateJobRegistry(const UpdateJobRegistry&) = delete;
        UpdateJobRegistry& operator=(const UpdateJobRegistry&) = delete;

        // 已有该 pi_dir 的 running job → nullptr（防重入）；否则复用或新建
        // slot（复用前对已结束的旧 worker join——立即返回），置 running=true
        // 并存 http。返回的 slot 由 job 启动方持有（worker lambda 亦持有，
        // 保证 job 执行期间 slot 存活）。
        std::shared_ptr<UpdateJobSlot> TryStart(
            const std::filesystem::path&  pi_dir,
            std::shared_ptr<PiHttpClient> http);

        bool IsRunning(const std::filesystem::path& pi_dir) const;

        // job 线程唯一写者：置 running=false + notify（绝不 erase slots_
        // 条目、绝不 reset slot 的 shared_ptr）。
        void Finish(const std::shared_ptr<UpdateJobSlot>& slot);

        // 测试缝：有界等待 slot running=false（wait_for，绝不无限等）。
        // slot 不存在视为 idle。
        bool WaitForSlotIdleForTest(
            const std::filesystem::path& pi_dir,
            std::chrono::milliseconds    timeout);

        // 对所有 running slot 先 http->RequestCancel()（使 join 有界）再
        // join worker；未 running 的 slot 只 join（worker 已结束，立即返回，
        // 且不 dereference http——别名构造的测试 fake 可能已析构）。
        ~UpdateJobRegistry();

    private:
        UpdateJobRegistry() = default;

        mutable std::mutex                                    mutex_;
        std::condition_variable                               idle_cv_;
        std::map<std::string, std::shared_ptr<UpdateJobSlot>> slots_;
    };
} // namespace Das::Plugins::DasMaaPi
