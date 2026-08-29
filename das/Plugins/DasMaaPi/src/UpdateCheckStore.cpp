#include "UpdateCheckStore.h"

namespace Das::Plugins::DasMaaPi
{
    UpdateCheckStore& UpdateCheckStore::Instance()
    {
        // 函数级 static：进程级存活，session 被驱逐重建后结果依然可读。
        static UpdateCheckStore store;
        return store;
    }

    std::optional<UpdateCheckEntry> UpdateCheckStore::SaveAndPutChecking(
        const std::filesystem::path& pi_dir,
        const std::string&           progress_key)
    {
        (void)pi_dir;
        (void)progress_key;
        return std::nullopt;
    }

    void UpdateCheckStore::RollbackCheckingIfUnchanged(
        const std::filesystem::path&           pi_dir,
        const std::string&                     progress_key,
        const std::optional<UpdateCheckEntry>& prev)
    {
        (void)pi_dir;
        (void)progress_key;
        (void)prev;
    }

    void UpdateCheckStore::PutOutcome(
        const std::filesystem::path& pi_dir,
        UpdateCheckEntry             entry)
    {
        (void)pi_dir;
        (void)entry;
    }

    std::optional<UpdateCheckEntry> UpdateCheckStore::Get(
        const std::filesystem::path& pi_dir) const
    {
        (void)pi_dir;
        return std::nullopt;
    }
} // namespace Das::Plugins::DasMaaPi
