#include "UpdateJobRegistry.h"

#include <utility>
#include <vector>

namespace Das::Plugins::DasMaaPi
{
    UpdateJobRegistry& UpdateJobRegistry::Instance()
    {
        // 函数级 static：进程退出 / DLL 卸载时析构触发 RequestCancel + join
        // 收尾（插件卸载路径无需显式调用）。
        static UpdateJobRegistry registry;
        return registry;
    }

    std::shared_ptr<UpdateJobSlot> UpdateJobRegistry::TryStart(
        const std::filesystem::path&  pi_dir,
        std::shared_ptr<PiHttpClient> http)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        // lexically_normal 统一分隔符形态（interface_path 投影出 '/' 形、
        // 原生构造出 '\' 形，Windows 下二者必须命中同一 slot）。
        const std::string key = pi_dir.lexically_normal().string();
        auto              it = slots_.find(key);
        if (it != slots_.end() && it->second->running.load())
        {
            // 防重入：该 pi_dir 已有 running job。
            return nullptr;
        }
        std::shared_ptr<UpdateJobSlot> slot;
        if (it != slots_.end())
        {
            // slot 复用：旧 job 已结束（running=false），对已结束的 worker
            // join 立即返回；绝不在此之外的路径 erase/替换 slot——job 线程
            // 只经 Finish 置位，自删除会在 worker 自身栈上 join 自己。
            slot = it->second;
            if (slot->worker.joinable())
            {
                slot->worker.join();
            }
        }
        else
        {
            slot = std::make_shared<UpdateJobSlot>();
            slots_.emplace(key, slot);
        }
        slot->running.store(true);
        slot->http = std::move(http);
        return slot;
    }

    bool UpdateJobRegistry::IsRunning(const std::filesystem::path& pi_dir) const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = slots_.find(pi_dir.lexically_normal().string());
        return it != slots_.end() && it->second->running.load();
    }

    void UpdateJobRegistry::Finish(const std::shared_ptr<UpdateJobSlot>& slot)
    {
        if (slot == nullptr)
        {
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            slot->running.store(false);
        }
        idle_cv_.notify_all();
    }

    bool UpdateJobRegistry::WaitForSlotIdleForTest(
        const std::filesystem::path& pi_dir,
        std::chrono::milliseconds    timeout)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const std::string            key = pi_dir.lexically_normal().string();
        const auto                   it = slots_.find(key);
        if (it == slots_.end())
        {
            return true;
        }
        const std::shared_ptr<UpdateJobSlot> slot = it->second;
        return idle_cv_.wait_for(
            lock,
            timeout,
            [&slot] { return !slot->running.load(); });
    }

    UpdateJobRegistry::~UpdateJobRegistry()
    {
        std::vector<std::shared_ptr<UpdateJobSlot>> snapshot;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            snapshot.reserve(slots_.size());
            for (auto& [key, slot] : slots_)
            {
                snapshot.push_back(slot);
            }
        }
        for (const auto& slot : snapshot)
        {
            // 先 cancel 使 join 有界（在途请求尽快中断）；未 running 的 job
            // 已不再触碰 http（worker 在 ApplyUpdate 返回后只调 reporter /
            // Finish），不 dereference http——测试注入的 fake 可能已析构。
            if (slot->running.load())
            {
                if (slot->http != nullptr)
                {
                    slot->http->RequestCancel();
                }
            }
            if (slot->worker.joinable())
            {
                slot->worker.join();
            }
        }
    }
} // namespace Das::Plugins::DasMaaPi
