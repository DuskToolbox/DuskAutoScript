#include "UpdateCheckStore.h"

#include <das/Utils/PathText.h>

#include <utility>

namespace Das::Plugins::DasMaaPi
{
    UpdateCheckStore& UpdateCheckStore::Instance()
    {
        // 函数级 static：进程级存活，session 被驱逐重建后结果依然可读
        // （后台线程不许碰 session，且 session 生命周期短于检查结果）。
        static UpdateCheckStore store;
        return store;
    }

    namespace
    {
        // 键归一：lexically_normal 统一分隔符与冗余段形态（interface_path
        // 投影出 '/' 形、原生构造出 '\' 形，Windows 下二者必须命中同一
        // 条目——与 UpdateJobRegistry 同一归一先例）。
        std::string NormalizeKey(const std::filesystem::path& pi_dir)
        {
            return Das::Utils::PathToUtf8(pi_dir.lexically_normal());
        }
    } // namespace

    std::optional<UpdateCheckEntry> UpdateCheckStore::SaveAndPutChecking(
        const std::filesystem::path& pi_dir,
        const std::string&           progress_key)
    {
        // 「保存旧值 → 写 Checking」必须锁内原子完成：拆成两步锁外做的
        // 话，prev 可能是别的线程刚写完 Checking 后的值，回滚恢复的就是
        // 别人的中间态而非真正的前值。
        const std::lock_guard<std::mutex> lock(mutex_);
        const std::string                 key = NormalizeKey(pi_dir);
        std::optional<UpdateCheckEntry>   prev;
        const auto                        it = entries_.find(key);
        if (it != entries_.end())
        {
            prev = it->second;
        }
        UpdateCheckEntry checking;
        checking.state = UpdateCheckEntry::State::Checking;
        checking.progress_key = progress_key;
        entries_[key] = std::move(checking);
        return prev;
    }

    void UpdateCheckStore::RollbackCheckingIfUnchanged(
        const std::filesystem::path&           pi_dir,
        const std::string&                     progress_key,
        const std::optional<UpdateCheckEntry>& prev)
    {
        // 恒等判定与恢复必须锁内原子：锁外判定后、恢复前，job 终态可能
        // 恰好落下，无条件恢复会糊掉它（三方竞态：受理 B 写 Checking →
        // 拒绝 → job A 终态落下 → B 回滚）。仅当条目仍是本受理线程写的
        // Checking（state 与 key 双恒等）才恢复 prev，否则让真相留下。
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(NormalizeKey(pi_dir));
        if (it == entries_.end())
        {
            return;
        }
        if (it->second.state != UpdateCheckEntry::State::Checking
            || it->second.progress_key != progress_key)
        {
            return;
        }
        if (prev.has_value())
        {
            it->second = *prev;
        }
        else
        {
            entries_.erase(it);
        }
    }

    void UpdateCheckStore::PutOutcome(
        const std::filesystem::path& pi_dir,
        UpdateCheckEntry             entry)
    {
        // 终态不变量：非 FoundUpdate 态强制 info.has_update=false——
        // UpToDate/Failed 条目绝不让 applyUpdate 按钮门携带陈旧的
        // has_update/download_url（防御性归一，合规调用方本来就不带）。
        if (entry.state != UpdateCheckEntry::State::FoundUpdate)
        {
            entry.info.has_update = false;
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        entries_[NormalizeKey(pi_dir)] = std::move(entry);
    }

    std::optional<UpdateCheckEntry> UpdateCheckStore::Get(
        const std::filesystem::path& pi_dir) const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(NormalizeKey(pi_dir));
        if (it == entries_.end())
        {
            return std::nullopt;
        }
        return it->second;
    }
} // namespace Das::Plugins::DasMaaPi
