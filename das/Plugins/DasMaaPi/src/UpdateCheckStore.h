#pragma once

#include "PiUpdateService.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace Das::Plugins::DasMaaPi
{
    // per-pi_dir 检查结果条目（四态）。FoundUpdate 态携带 PiUpdateInfo 全量
    // （含 download_url / progress_key）；UpToDate / Failed 态记
    // info.has_update = false（PutOutcome 强制）。progress_key 是写入方标识：
    // 受理线程写 Checking 时填本次生成的 key，回滚恒等判定（条目仍 Checking
    // 且 key 相同才恢复 prev）用它区分「还是我写的」。Failed 态记原始失败
    // 分类（reason_key + http_status），消费侧各自 Translate，不在 store 内
    // 合成文案——语言切换后投影与推送两侧一致。
    struct UpdateCheckEntry
    {
        enum class State
        {
            Checking,
            FoundUpdate,
            UpToDate,
            Failed
        };

        State        state = State::Checking;
        PiUpdateInfo info;               // FoundUpdate 全量（含 download_url）
        std::string  progress_key;       // 写入方标识（回滚恒等判定用）
        std::string  failure_reason_key; // Failed：原始分类（""=通用）
        int          failure_http_status = 0;
    };

    // 进程级检查结果 store（函数级 static 单例，mutex 全方法保护）。键 =
    // pi_dir 的 lexically_normal 字符串形态（与 UpdateJobRegistry 同一归一
    // 先例，Windows 下 '/' 与 '\' 形态必须命中同一条目）。条目活到下次
    // checkUpdate 覆盖或进程退出，无 TTL；map 大小上界 = distinct pi_dir 数。
    // 后台线程不许碰 session（session 会被驱逐重建），结果必须进程级存活
    // ——这是本 store 存在的根因。
    // 线程契约：SaveAndPutChecking / RollbackCheckingIfUnchanged / Get 在
    // IPC BusinessThread 调用；PutOutcome 由 job 线程调用——双线程写故
    // mutex 全方法保护。
    class UpdateCheckStore final
    {
    public:
        static UpdateCheckStore& Instance();

        UpdateCheckStore(const UpdateCheckStore&) = delete;
        UpdateCheckStore& operator=(const UpdateCheckStore&) = delete;

        // 受理（BT）：锁内原子完成「保存旧值 → 写 Checking」，返回 prev 供
        // TryStart 被拒时回滚。prev 为 nullopt 表示此前无条目。
        std::optional<UpdateCheckEntry> SaveAndPutChecking(
            const std::filesystem::path& pi_dir,
            const std::string&           progress_key);

        // 受理（BT）：TryStart 拒绝后回滚。锁内恒等判定「条目仍 Checking 且
        // progress_key == 本次 key」才恢复 prev，否则不动（三方竞态：并发
        // 落下的 job 终态不被回滚糊掉）。
        void RollbackCheckingIfUnchanged(
            const std::filesystem::path&           pi_dir,
            const std::string&                     progress_key,
            const std::optional<UpdateCheckEntry>& prev);

        // job 线程：覆盖终态（FoundUpdate 带 info 全量；UpToDate / Failed 记
        // has_update=false 与失败分类——非 FoundUpdate 态强制清
        // info.has_update）。
        void PutOutcome(
            const std::filesystem::path& pi_dir,
            UpdateCheckEntry             entry);

        // BT 读（投影 / 按钮门 / applyUpdate 前置校验）；job 线程在 Finish
        // 之后也读（按钮态终态推送取同源条目）。返回条目快照。
        std::optional<UpdateCheckEntry> Get(
            const std::filesystem::path& pi_dir) const;

    private:
        UpdateCheckStore() = default;

        mutable std::mutex                      mutex_;
        std::map<std::string, UpdateCheckEntry> entries_;
    };
} // namespace Das::Plugins::DasMaaPi
