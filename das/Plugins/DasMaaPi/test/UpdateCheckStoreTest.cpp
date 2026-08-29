#include "../src/UpdateCheckStore.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <thread>

namespace
{
    using namespace Das::Plugins::DasMaaPi;

    // FoundUpdate 全量种子：info 与条目双 progress_key 同流（apply 侧 reporter
    // 用条目内 key）。
    UpdateCheckEntry MakeFoundUpdateEntry(
        const std::string& version,
        const std::string& key)
    {
        UpdateCheckEntry entry;
        entry.state = UpdateCheckEntry::State::FoundUpdate;
        entry.info.has_update = true;
        entry.info.version_name = version;
        entry.info.release_note = "note-" + version;
        entry.info.download_url = "https://example.com/" + version + ".zip";
        entry.info.progress_key = key;
        entry.progress_key = key;
        return entry;
    }

    // Test 1: 空 pi_dir 首写 → 返回 nullopt prev；Get 返回 Checking 态条目
    // 且 progress_key 等于传入 key。
    TEST(UpdateCheckStoreTest, SaveAndPutCheckingFirstWriteReturnsNulloptPrev)
    {
        auto&                       store = UpdateCheckStore::Instance();
        const std::filesystem::path pi_dir = "C:/store/test/first-write";

        const auto prev = store.SaveAndPutChecking(pi_dir, "key-a");
        EXPECT_FALSE(prev.has_value());

        const auto entry = store.Get(pi_dir);
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->state, UpdateCheckEntry::State::Checking);
        EXPECT_EQ(entry->progress_key, "key-a");
    }

    // Test 2: 已有 FoundUpdate 终态的 pi_dir 再 SaveAndPutChecking → prev
    // 返回该 FoundUpdate 条目（保存旧值语义），当前条目翻为 Checking。
    TEST(UpdateCheckStoreTest, SaveAndPutCheckingOverTerminalReturnsPrev)
    {
        auto&                       store = UpdateCheckStore::Instance();
        const std::filesystem::path pi_dir = "C:/store/test/over-terminal";

        store.PutOutcome(pi_dir, MakeFoundUpdateEntry("v2.23.0", "key-old"));

        const auto prev = store.SaveAndPutChecking(pi_dir, "key-new");
        ASSERT_TRUE(prev.has_value());
        EXPECT_EQ(prev->state, UpdateCheckEntry::State::FoundUpdate);
        EXPECT_EQ(prev->info.version_name, "v2.23.0");
        EXPECT_EQ(prev->progress_key, "key-old");

        const auto entry = store.Get(pi_dir);
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->state, UpdateCheckEntry::State::Checking);
        EXPECT_EQ(entry->progress_key, "key-new");
    }

    // Test 3: PutOutcome(FoundUpdate 全量 info) 后 Get 返回 info 的
    // version_name / release_note / download_url / progress_key 全量；
    // 非 FoundUpdate 态强制 has_update=false（UpToDate/Failed 条目
    // 绝不让 apply 按钮携带 has_update/download_url）。
    TEST(UpdateCheckStoreTest, PutOutcomeFoundUpdateStoresFullInfo)
    {
        auto&                       store = UpdateCheckStore::Instance();
        const std::filesystem::path pi_dir = "C:/store/test/full-info";
        const std::filesystem::path other_dir = "C:/store/test/full-info-2";

        store.SaveAndPutChecking(pi_dir, "key-a");
        store.PutOutcome(pi_dir, MakeFoundUpdateEntry("v2.23.0", "key-a"));

        const auto entry = store.Get(pi_dir);
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->state, UpdateCheckEntry::State::FoundUpdate);
        EXPECT_TRUE(entry->info.has_update);
        EXPECT_EQ(entry->info.version_name, "v2.23.0");
        EXPECT_EQ(entry->info.release_note, "note-v2.23.0");
        EXPECT_EQ(entry->info.download_url, "https://example.com/v2.23.0.zip");
        EXPECT_EQ(entry->info.progress_key, "key-a");

        // 终态不变量：即使种子带 has_update=true，UpToDate 落库后必为 false。
        UpdateCheckEntry up_to_date;
        up_to_date.state = UpdateCheckEntry::State::UpToDate;
        up_to_date.info.has_update = true;
        store.PutOutcome(other_dir, up_to_date);
        const auto other = store.Get(other_dir);
        ASSERT_TRUE(other.has_value());
        EXPECT_EQ(other->state, UpdateCheckEntry::State::UpToDate);
        EXPECT_FALSE(other->info.has_update);
    }

    // Test 4: 回滚恢复 prev——条目仍 Checking 且 progress_key 匹配时：
    // prev 为 nullopt → 条目消失；prev 为旧终态 → 恢复该终态。
    TEST(UpdateCheckStoreTest, RollbackCheckingIfUnchangedRestoresPrev)
    {
        auto& store = UpdateCheckStore::Instance();

        // 子例 1：prev 为 nullopt → 条目消失。
        const std::filesystem::path empty_prev_dir =
            "C:/store/test/rollback-empty";
        const auto prev1 = store.SaveAndPutChecking(empty_prev_dir, "key-a");
        store.RollbackCheckingIfUnchanged(empty_prev_dir, "key-a", prev1);
        EXPECT_FALSE(store.Get(empty_prev_dir).has_value());

        // 子例 2：prev 为旧终态 → 恢复该终态。
        const std::filesystem::path terminal_prev_dir =
            "C:/store/test/rollback-terminal";
        store.PutOutcome(
            terminal_prev_dir,
            MakeFoundUpdateEntry("v2.23.0", "key-old"));
        const auto prev2 =
            store.SaveAndPutChecking(terminal_prev_dir, "key-new");
        store.RollbackCheckingIfUnchanged(terminal_prev_dir, "key-new", prev2);
        const auto entry = store.Get(terminal_prev_dir);
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->state, UpdateCheckEntry::State::FoundUpdate);
        EXPECT_EQ(entry->progress_key, "key-old");
        EXPECT_EQ(entry->info.version_name, "v2.23.0");
    }

    // Test 5: 回滚恒等拒绝——条目已被 PutOutcome 覆盖为终态、或条目
    // progress_key 与本次不同（另一受理线程已覆盖 Checking）→ 回滚不修改
    // 条目（三方竞态推演：B 写 Checking → A 的 job 终态落下 → B 回滚不得
    // 糊掉终态）。
    TEST(UpdateCheckStoreTest, RollbackCheckingIfUnchangedRejectsStaleIdentity)
    {
        auto& store = UpdateCheckStore::Instance();

        // 子例 1：条目已被 PutOutcome 覆盖为 Failed 终态 → 回滚不得动。
        const std::filesystem::path overwritten_dir =
            "C:/store/test/rollback-overwritten";
        const auto prev1 = store.SaveAndPutChecking(overwritten_dir, "key-a");
        UpdateCheckEntry failed;
        failed.state = UpdateCheckEntry::State::Failed;
        failed.progress_key = "key-a";
        failed.failure_reason_key = "diagnostic.checkUpdateRequestFailed";
        failed.failure_http_status = 503;
        store.PutOutcome(overwritten_dir, failed);
        store.RollbackCheckingIfUnchanged(overwritten_dir, "key-a", prev1);
        const auto entry1 = store.Get(overwritten_dir);
        ASSERT_TRUE(entry1.has_value());
        EXPECT_EQ(entry1->state, UpdateCheckEntry::State::Failed);
        EXPECT_EQ(
            entry1->failure_reason_key,
            "diagnostic.checkUpdateRequestFailed");
        EXPECT_EQ(entry1->failure_http_status, 503);

        // 子例 2：条目 progress_key 与本次不同 → 回滚不动（保留新写入方）。
        const std::filesystem::path key_mismatch_dir =
            "C:/store/test/rollback-key-mismatch";
        const auto prev2 = store.SaveAndPutChecking(key_mismatch_dir, "key-a");
        (void)store.SaveAndPutChecking(key_mismatch_dir, "key-b");
        store.RollbackCheckingIfUnchanged(key_mismatch_dir, "key-a", prev2);
        const auto entry2 = store.Get(key_mismatch_dir);
        ASSERT_TRUE(entry2.has_value());
        EXPECT_EQ(entry2->state, UpdateCheckEntry::State::Checking);
        EXPECT_EQ(entry2->progress_key, "key-b");
    }

    // Test 6: 键归一——同一目录的 \ 与 / 分隔形态、含 . 或 .. 的相对形态
    // 命中同一条目（interface_path 投影出 '/' 形、原生构造出 '\' 形，
    // Windows 下二者必须命中同一条目）。
    TEST(UpdateCheckStoreTest, KeyNormalizationHitsSameEntry)
    {
        auto&                       store = UpdateCheckStore::Instance();
        const std::filesystem::path pi_dir = "C:/store/test/normalize";

        store.SaveAndPutChecking(pi_dir, "key-a");

        // '\\' 分隔形态。
        const auto via_backslash =
            store.Get(std::filesystem::path("C:\\store\\test\\normalize"));
        ASSERT_TRUE(via_backslash.has_value());
        EXPECT_EQ(via_backslash->progress_key, "key-a");

        // 含 '.' 段的形态。
        const auto via_dot =
            store.Get(std::filesystem::path("C:/store/test/./normalize"));
        ASSERT_TRUE(via_dot.has_value());
        EXPECT_EQ(via_dot->progress_key, "key-a");

        // 含 '..' 回退段的形态。
        const auto via_dotdot = store.Get(
            std::filesystem::path("C:/store/test/sibling/../normalize"));
        ASSERT_TRUE(via_dotdot.has_value());
        EXPECT_EQ(via_dotdot->progress_key, "key-a");

        // 写侧同样归一：经 '\' 形态 PutOutcome，经 '/' 形态可读回。
        store.PutOutcome(
            std::filesystem::path("C:\\store\\test\\normalize"),
            MakeFoundUpdateEntry("v2.23.0", "key-a"));
        const auto entry = store.Get(pi_dir);
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->state, UpdateCheckEntry::State::FoundUpdate);
        EXPECT_EQ(entry->info.version_name, "v2.23.0");
    }

    // Test 7: 并发写（两线程交错 SaveAndPutChecking / Rollback / PutOutcome
    // 若干轮）无数据竞争报告（ember 即 ASAN 构建）且终态可观测。
    TEST(UpdateCheckStoreTest, ConcurrentWritesFinalStateObservable)
    {
        auto&                       store = UpdateCheckStore::Instance();
        const std::filesystem::path pi_dir = "C:/store/test/concurrent";
        constexpr int               kRounds = 200;

        const auto hammer = [&store, &pi_dir](const std::string& key_prefix)
        {
            for (int i = 0; i < kRounds; ++i)
            {
                const std::string key = key_prefix + std::to_string(i);
                const auto        prev = store.SaveAndPutChecking(pi_dir, key);
                if (i % 3 == 0)
                {
                    store.RollbackCheckingIfUnchanged(pi_dir, key, prev);
                }
                UpdateCheckEntry outcome =
                    MakeFoundUpdateEntry("v" + std::to_string(i), key);
                if (i % 2 == 0)
                {
                    outcome.state = UpdateCheckEntry::State::UpToDate;
                }
                else if (i % 5 == 0)
                {
                    outcome.state = UpdateCheckEntry::State::Failed;
                    outcome.failure_reason_key =
                        "diagnostic.checkUpdateNetworkFailed";
                    outcome.failure_http_status = 0;
                }
                store.PutOutcome(pi_dir, std::move(outcome));
                (void)store.Get(pi_dir);
            }
        };

        std::thread writer_a{hammer, "key-a-"};
        std::thread writer_b{hammer, "key-b-"};
        writer_a.join();
        writer_b.join();

        const auto entry = store.Get(pi_dir);
        ASSERT_TRUE(entry.has_value());
        EXPECT_TRUE(
            entry->state == UpdateCheckEntry::State::Checking
            || entry->state == UpdateCheckEntry::State::FoundUpdate
            || entry->state == UpdateCheckEntry::State::UpToDate
            || entry->state == UpdateCheckEntry::State::Failed);
    }

    // Test 8: 并发双 check 的确定性交错序列（受理化 e2e 语义在 store 层钉死；
    // e2e 侧无法确定性构造「B 回滚晚于 A 终态」的窗口——受理与回滚在同一
    // BT 调用内相邻）：A 写 Checking(A) → B 受理写 Checking(B)（prev=A 的
    // Checking）→ A 的 job 终态落下 → B 被拒回滚——恒等判定见条目已非
    // Checking，prev（A 的 Checking）不得恢复：A 终态不被糊掉，也不留任何
    // Checking 残留。
    TEST(UpdateCheckStoreTest, RollbackAfterTerminalOverwriteKeepsFirstTerminal)
    {
        auto&                       store = UpdateCheckStore::Instance();
        const std::filesystem::path pi_dir =
            "C:/store/test/rollback-after-terminal";

        // A 受理（首写，prev 为空）→ 条目 Checking(A)。
        const auto prev_a = store.SaveAndPutChecking(pi_dir, "key-a");
        EXPECT_FALSE(prev_a.has_value());

        // B 受理（保存 prev = A 的 Checking）→ 条目 Checking(B)。
        const auto prev_b = store.SaveAndPutChecking(pi_dir, "key-b");
        ASSERT_TRUE(prev_b.has_value());
        EXPECT_EQ(prev_b->state, UpdateCheckEntry::State::Checking);
        EXPECT_EQ(prev_b->progress_key, "key-a");

        // A 的 job 终态在 B 回滚前落下 → 条目翻为 A 终态。
        store.PutOutcome(pi_dir, MakeFoundUpdateEntry("v2.23.0", "key-a"));

        // B 回滚：条目已非 Checking → 不恢复 prev、不动终态。
        store.RollbackCheckingIfUnchanged(pi_dir, "key-b", prev_b);

        const auto entry = store.Get(pi_dir);
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->state, UpdateCheckEntry::State::FoundUpdate);
        EXPECT_EQ(entry->progress_key, "key-a");
        EXPECT_EQ(entry->info.version_name, "v2.23.0");
    }
} // namespace
