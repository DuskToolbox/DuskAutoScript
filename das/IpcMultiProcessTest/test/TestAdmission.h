/**
 * @file TestAdmission.h
 * @brief 申报制测试准入基建（申报宏 + 注册表 + 准入闸门 + listener）
 *
 * 流控单元=测试本身：listener 在测试体开始前过闸（查申报表→余量
 * 判定→放行打点），测试结束打点并唤醒其它排队进程。listener 只装进
 * 含申报宏的测试 exe——宏注册器静态构造路径完成恰好一次安装
 * （main 零改动），纯单测 exe 零接入零开销。
 *
 * 闸门等待=事件+超时混合：余量不足时 Wait(命名 Event, 1s)，1s 到期
 * 重查兜底盖住无事件源释放；总超时到点具名失败（绝不挂死、绝不静默
 * 放行）；DAS_DEBUG=1 总超时归零=无限等待（沿既有调试约定）。
 */

#ifndef DAS_TEST_ADMISSION_H
#define DAS_TEST_ADMISSION_H

#include "CommitMemory.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef DAS_WINDOWS
#include <windows.h>
#endif

// ====== ASAN 编译期感知 ======
// __has_feature 双检必须分层嵌套：MSVC 传统预处理器不短路 &&，
// 未定义的函数式宏名进入常量表达式是硬错误（GCC 的 __SANITIZE_ADDRESS__
// 与 Clang 的 __has_feature 分开判，MSVC 自家 ASAN 定义前者）
#if defined(__SANITIZE_ADDRESS__)
#define DAS_TEST_ADMISSION_ASAN_ACTIVE 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DAS_TEST_ADMISSION_ASAN_ACTIVE 1
#else
#define DAS_TEST_ADMISSION_ASAN_ACTIVE 0
#endif
#else
#define DAS_TEST_ADMISSION_ASAN_ACTIVE 0
#endif

namespace TestAdmission
{
    // ====== 常量 ======

    /// 排队总超时；DAS_DEBUG=1 时解析为 0（无限等待）
    constexpr std::uint64_t DAS_TEST_ADMISSION_TIMEOUT_MS = 120000;

    /// 单 host 提交内存估算（ASAN 档：影子内存+红区把堆开销放 2-3 倍；
    /// 实测于 2026-10-08 Windows ember 树，轻载 host 稳态中位 96.8MiB、
    /// 重插件 host 峰 103MiB，97MiB 向上取整 × 1.3）
    constexpr std::uint64_t DAS_TEST_HOST_COMMIT_ESTIMATE_BYTES_ASAN =
        127ull * 1024 * 1024;

    /// 单 host 提交内存估算（非 ASAN 档；实测于 2026-10-08 Windows
    /// msvc-ember 树，host 稳态中位 8.4MiB / 峰值中位 9.1MiB，
    /// 10MiB 向上取整 × 1.3）
    constexpr std::uint64_t DAS_TEST_HOST_COMMIT_ESTIMATE_BYTES =
        13ull * 1024 * 1024;

    /// 测试基数预留：测试进程自身（不 host）运行所需的 commit 下限
    /// （实测于 2026-10-08 Windows ember 树 -j8 并发采样：ASAN 测试进程
    /// 稳态中位 467.8MiB、峰 498.7MiB——初始 256MiB 低估使全量并行下
    /// 系统 commit 突刺耗尽，host 启动的 SHM 池创建失败；499 × 1.3 取整）
    constexpr std::uint64_t DAS_TEST_BASE_COMMIT_RESERVE_BYTES =
        640ull * 1024 * 1024;

#if DAS_TEST_ADMISSION_ASAN_ACTIVE
    constexpr std::uint64_t DAS_TEST_HOST_COMMIT_ESTIMATE_BYTES_ACTIVE =
        DAS_TEST_HOST_COMMIT_ESTIMATE_BYTES_ASAN;
#else
    constexpr std::uint64_t DAS_TEST_HOST_COMMIT_ESTIMATE_BYTES_ACTIVE =
        DAS_TEST_HOST_COMMIT_ESTIMATE_BYTES;
#endif

    /// precise=false 口径（物理余量）与 commit 余量的偏差无上界，
    /// 估算乘 3/2 保守系数压低放行线
    constexpr std::uint64_t IMPRECISE_ESTIMATE_SAFETY_NUMERATOR = 3;
    constexpr std::uint64_t IMPRECISE_ESTIMATE_SAFETY_DENOMINATOR = 2;

    // ====== DAS_DEBUG 语义（自带 getenv，不依赖 IpcTestConfig） ======

    inline bool IsAdmissionDebugMode()
    {
        const char* debug = std::getenv("DAS_DEBUG");
        return debug != nullptr
               && (std::string(debug) == "1" || std::string(debug) == "true");
    }

    inline std::uint64_t GetAdmissionTimeoutMs()
    {
        return IsAdmissionDebugMode() ? 0 : DAS_TEST_ADMISSION_TIMEOUT_MS;
    }

    // ====== 申报注册表 ======

    namespace detail
    {
        /// inline 函数级静态承载注册表：ODR-safe 且恰好一次构造，
        /// 多 TU 的宏注册器并发抵达也不会重复初始化
        struct AdmissionRegistryStorage
        {
            std::map<std::pair<std::string, std::string>, std::int64_t>
                                                exact_entries;
            std::map<std::string, std::int64_t> suite_defaults;
        };

        inline AdmissionRegistryStorage& RegistryStorage()
        {
            static AdmissionRegistryStorage storage;
            return storage;
        }
    } // namespace detail

    inline void RegisterDeclaredHosts(
        const char*  suite_name,
        const char*  test_name,
        std::int64_t max_hosts)
    {
        detail::RegistryStorage().exact_entries[{suite_name, test_name}] =
            max_hosts;
    }

    inline void RegisterSuiteDefaultHosts(
        const char*  suite_name,
        std::int64_t max_hosts)
    {
        detail::RegistryStorage().suite_defaults[suite_name] = max_hosts;
    }

    /// 查找序：精确 suite.test → suite 默认 → 0（未申报直通）
    inline std::int64_t LookupDeclaredHosts(
        const std::string& suite_name,
        const std::string& test_name)
    {
        const auto& storage = detail::RegistryStorage();
        const auto  exact = storage.exact_entries.find({suite_name, test_name});
        if (exact != storage.exact_entries.end())
        {
            return exact->second;
        }
        const auto suite_default = storage.suite_defaults.find(suite_name);
        if (suite_default != storage.suite_defaults.end())
        {
            return suite_default->second;
        }
        return 0;
    }

    // ====== 日志契约（stdout 打点，ctest 按测试捕获天然隔离） ======

    inline std::uint64_t NowEpochMilliseconds()
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    inline std::string BuildAdmissionLogLine(
        const std::string&                                      suite_name,
        const std::string&                                      test_name,
        const char*                                             event_name,
        std::uint64_t                                           timestamp_ms,
        const std::vector<std::pair<std::string, std::string>>& fields)
    {
        std::string line = "[das-admission] suite=" + suite_name
                           + " test=" + test_name + " event=" + event_name
                           + " ts=" + std::to_string(timestamp_ms);
        for (const auto& [key, value] : fields)
        {
            line += " ";
            line += key;
            line += "=";
            line += value;
        }
        return line;
    }

    /// endl 强制冲刷：host 启动类测试若崩溃，最后的准入行是最有价值的
    /// 取证数据
    inline void EmitAdmissionLogLine(const std::string& line)
    {
        std::cout << line << std::endl;
    }

    // ====== 可注入 seam（fake 余量源 + fake 时钟） ======

    using HeadroomSource = CommitMemory::CommitHeadroom (*)();
    using ClockMilliseconds = std::uint64_t (*)();

    namespace detail
    {
        inline HeadroomSource& HeadroomSourceOverride()
        {
            static HeadroomSource source = nullptr;
            return source;
        }

        inline ClockMilliseconds& ClockOverride()
        {
            static ClockMilliseconds clock_function = nullptr;
            return clock_function;
        }
    } // namespace detail

    /// 注入 nullptr 恢复默认源
    inline void SetAdmissionHeadroomSourceForTest(HeadroomSource source)
    {
        detail::HeadroomSourceOverride() = source;
    }

    /// 注入 nullptr 恢复默认时钟
    inline void SetAdmissionClockForTest(ClockMilliseconds clock_function)
    {
        detail::ClockOverride() = clock_function;
    }

    inline CommitMemory::CommitHeadroom ReadCommitHeadroom()
    {
        if (detail::HeadroomSourceOverride() != nullptr)
        {
            return detail::HeadroomSourceOverride()();
        }
        return CommitMemory::QueryCommitHeadroom();
    }

    /// 闸门节奏时钟（超时判定与等待节奏）；日志时间戳另有真实
    /// epoch 时钟（NowEpochMilliseconds），不受 seam 控制
    inline std::uint64_t ClockNowMilliseconds()
    {
        if (detail::ClockOverride() != nullptr)
        {
            return detail::ClockOverride()();
        }
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    // ====== 跨进程唤醒事件（事件+超时混合等待的事件半边） ======

    namespace detail
    {
#ifdef DAS_WINDOWS
        /// 事件仅是唤醒提示，闸门放行判据永远是真实余量读数；伪造
        /// SetEvent 最多造成一次提前重查（无害）。Local 会话命名空间
        /// 即够，auto-reset 语义让多个等待者逐个被唤醒后各自重查。
        inline HANDLE AdmissionReleaseEvent()
        {
            static HANDLE release_event = ::CreateEventW(
                nullptr,
                FALSE,
                FALSE,
                L"Local\\das_test_admission_release");
            return release_event;
        }
#endif

        inline void WaitForReleaseSignalOrInterval()
        {
            if (ClockOverride() != nullptr)
            {
                // fake 时钟驱动节奏：等待时长由注入时钟的推进表达，
                // 单测不得真实睡眠
                return;
            }
#ifdef DAS_WINDOWS
            ::WaitForSingleObject(AdmissionReleaseEvent(), 1000);
#else
            // POSIX 无会话内命名事件，降级为 1s 重查循环：这是跨平台
            // 「Unix 尽力」的降级边界（唤醒及时性损失，正确性不受损，
            // 重查兜底盖住一切释放路径）
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
#endif
        }

        inline void SignalAdmissionRelease()
        {
#ifdef DAS_WINDOWS
            if (AdmissionReleaseEvent() != nullptr)
            {
                ::SetEvent(AdmissionReleaseEvent());
            }
#endif
        }
    } // namespace detail

    // ====== 准入闸门 ======

    enum class AdmissionOutcome
    {
        Admitted,
        TimedOut,
        ReadFailure,
    };

    namespace detail
    {
        /// 最近一条具名失败行（超时/读数失败）的查询口：具名失败必须
        /// 可编程取证，不能只依赖 stdout 语料
        inline std::string& LastNamedFailureLine()
        {
            static std::string line;
            return line;
        }

        inline std::uint64_t NeededCommitBytes(
            std::int64_t declared_host_count,
            bool         precise)
        {
            std::uint64_t host_estimate_bytes =
                DAS_TEST_HOST_COMMIT_ESTIMATE_BYTES_ACTIVE;
            if (!precise)
            {
                host_estimate_bytes = host_estimate_bytes
                                      * IMPRECISE_ESTIMATE_SAFETY_NUMERATOR
                                      / IMPRECISE_ESTIMATE_SAFETY_DENOMINATOR;
            }
            return static_cast<std::uint64_t>(declared_host_count)
                       * host_estimate_bytes
                   + DAS_TEST_BASE_COMMIT_RESERVE_BYTES;
        }

        inline void RecordNamedFailure(const std::string& failure_line)
        {
            LastNamedFailureLine() = failure_line;
            EmitAdmissionLogLine(failure_line);
        }
    } // namespace detail

    inline std::string GetLastNamedFailureLine()
    {
        return detail::LastNamedFailureLine();
    }

    /**
     * @brief 准入闸门：余量充足即放行，否则事件+1s 混合等待，
     *        总超时到点/读数失败走具名失败
     *
     * declared=0 时只需测试基数余量（正常机器上一次读数即放行，
     * 零额外等待）。读数失败（available_bytes==0）不放行不挂死，
     * 按具名失败立即判负（fail-loud）。
     */
    inline AdmissionOutcome WaitForAdmission(std::int64_t declared_host_count)
    {
        const std::uint64_t timeout_ms = GetAdmissionTimeoutMs();
        const std::uint64_t started_ms = ClockNowMilliseconds();
        const std::string declared_field = std::to_string(declared_host_count);
        while (true)
        {
            const CommitMemory::CommitHeadroom headroom = ReadCommitHeadroom();
            const std::uint64_t needed_bytes = detail::NeededCommitBytes(
                declared_host_count,
                headroom.precise);
            const std::string available_field =
                std::to_string(headroom.available_bytes);
            if (headroom.available_bytes == 0)
            {
                detail::RecordNamedFailure(BuildAdmissionLogLine(
                    "gate",
                    "WaitForAdmission",
                    "read-failure",
                    NowEpochMilliseconds(),
                    {{"declared", declared_field},
                     {"needed_bytes", std::to_string(needed_bytes)},
                     {"available_bytes", available_field}}));
                return AdmissionOutcome::ReadFailure;
            }
            if (headroom.available_bytes >= needed_bytes)
            {
                return AdmissionOutcome::Admitted;
            }
            const std::uint64_t waited_ms = ClockNowMilliseconds() - started_ms;
            if (timeout_ms != 0 && waited_ms >= timeout_ms)
            {
                detail::RecordNamedFailure(BuildAdmissionLogLine(
                    "gate",
                    "WaitForAdmission",
                    "denied",
                    NowEpochMilliseconds(),
                    {{"declared", declared_field},
                     {"needed_bytes", std::to_string(needed_bytes)},
                     {"available_bytes", available_field},
                     {"waited_ms", std::to_string(waited_ms)}}));
                return AdmissionOutcome::TimedOut;
            }
            detail::WaitForReleaseSignalOrInterval();
        }
    }

    // ====== host 启动计数（申报违约检测） ======

    namespace detail
    {
        inline std::atomic<std::int64_t>& HostStartCounter()
        {
            static std::atomic<std::int64_t> counter{0};
            return counter;
        }

        /// 当前测试的申报值（listener 在 OnTestStart 写入）
        inline std::int64_t& CurrentDeclaredHostCount()
        {
            static std::int64_t declared = 0;
            return declared;
        }
    } // namespace detail

    inline void CheckHostStartViolation(
        std::int64_t started_hosts,
        const char*  file,
        int          line)
    {
        if (started_hosts <= detail::CurrentDeclaredHostCount())
        {
            return;
        }
        // 超申报=响亮报错，不做静默兜底
        ADD_FAILURE_AT(file, line);
        EmitAdmissionLogLine(BuildAdmissionLogLine(
            "gate",
            "NoteHostStart",
            "violation",
            NowEpochMilliseconds(),
            {{"declared", std::to_string(detail::CurrentDeclaredHostCount())},
             {"started", std::to_string(started_hosts)}}));
    }

    /// 串行生命周期段作用域：前一个 host 退出后下一个才启动的形态
    /// （如 ColdStart 循环）在每轮生命周期结束时把计数恢复到本轮
    /// 开始前的值——「同时存活」语义下这类形态的峰值就是 1。
    class HostSessionScope
    {
    public:
        HostSessionScope() : baseline_hosts_(detail::HostStartCounter().load())
        {
        }

        HostSessionScope(const HostSessionScope&) = delete;
        HostSessionScope& operator=(const HostSessionScope&) = delete;

        ~HostSessionScope() { detail::HostStartCounter() = baseline_hosts_; }

    private:
        std::int64_t baseline_hosts_;
    };

    // ====== listener ①②③ 打点（全仓首个 TestEventListener 用户） ======

    struct ListenerEventCounts
    {
        std::uint64_t start_events{0};
        std::uint64_t admitted_events{0};
        std::uint64_t done_events{0};
    };

    namespace detail
    {
        inline std::map<std::string, ListenerEventCounts>&
        ListenerEventCountsTable()
        {
            static std::map<std::string, ListenerEventCounts> table;
            return table;
        }
    } // namespace detail

    /// A2 自观测查询口：本进程内指定用例的 ①②③ 事件计数（重复安装
    /// listener 会表现为 start_events==2）
    inline ListenerEventCounts GetListenerEventCounts(
        const std::string& test_name)
    {
        const auto& table = detail::ListenerEventCountsTable();
        const auto  found = table.find(test_name);
        if (found != table.end())
        {
            return found->second;
        }
        return {};
    }

    class AdmissionListener final : public ::testing::EmptyTestEventListener
    {
    public:
        void OnTestStart(const ::testing::TestInfo& test_info) override
        {
            const std::string  suite_name = test_info.test_suite_name();
            const std::string  test_name = test_info.name();
            const std::int64_t declared =
                LookupDeclaredHosts(suite_name, test_name);
            detail::CurrentDeclaredHostCount() = declared;
            detail::HostStartCounter() = 0;

            ListenerEventCounts& counts =
                detail::ListenerEventCountsTable()[test_name];
            ++counts.start_events;
            const std::uint64_t start_epoch_ms = NowEpochMilliseconds();
            EmitAdmissionLogLine(BuildAdmissionLogLine(
                suite_name,
                test_name,
                "start",
                start_epoch_ms,
                {{"declared", std::to_string(declared)}}));

            const AdmissionOutcome outcome = WaitForAdmission(declared);
            if (outcome != AdmissionOutcome::Admitted)
            {
                // 具名失败行已在闸门内打出；这里让测试判负，定位到
                // 测试定义处
                ADD_FAILURE_AT(test_info.file(), test_info.line());
                return;
            }
            const std::uint64_t admitted_epoch_ms = NowEpochMilliseconds();
            ++counts.admitted_events;
            EmitAdmissionLogLine(BuildAdmissionLogLine(
                suite_name,
                test_name,
                "admitted",
                admitted_epoch_ms,
                {{"declared", std::to_string(declared)},
                 {"queued",
                  std::to_string(admitted_epoch_ms - start_epoch_ms)}}));
        }

        void OnTestEnd(const ::testing::TestInfo& test_info) override
        {
            const std::string  suite_name = test_info.test_suite_name();
            const std::string  test_name = test_info.name();
            const std::int64_t declared = detail::CurrentDeclaredHostCount();
            ++detail::ListenerEventCountsTable()[test_name].done_events;
            EmitAdmissionLogLine(BuildAdmissionLogLine(
                suite_name,
                test_name,
                "done",
                NowEpochMilliseconds(),
                {{"declared", std::to_string(declared)}}));
            // 事件源=各测试结束点（唯一测试侧事件源）
            detail::SignalAdmissionRelease();
        }
    };

    /**
     * @brief 幂等安装准入 listener
     *
     * inline 函数级静态守卫：多 TU 的宏注册器并发抵达也恰好一次
     * Append（装进含申报宏的 exe，自定义 main 与 gtest_main 同机制）。
     */
    inline void InstallAdmissionListenerOnce()
    {
        static const bool installed = []()
        {
            ::testing::UnitTest::GetInstance()->listeners().Append(
                new AdmissionListener);
            return true;
        }();
        (void)installed;
    }
} // namespace TestAdmission

/**
 * @brief host 启动计数上报：每个测试实际 spawn host 的咽喉点各调一次
 *
 * 进程内 atomic 计数（listener 在每个测试开始时归零），超出当前测试
 * 申报值即 ADD_FAILURE 响亮报错。计数是「同时存活」语义——host 会
 * 中途退出再启动下一个的串行生命周期形态用 TestAdmission::
 * HostSessionScope 在每轮结束时恢复基线。
 */
inline void DasTestAdmissionNoteHostStart()
{
    const std::int64_t started_hosts =
        ++TestAdmission::detail::HostStartCounter();
    TestAdmission::CheckHostStartViolation(started_hosts, __FILE__, __LINE__);
}

/**
 * @brief host 停止/退出配对递减：串行生命周期形态（前一 host 确认死亡
 * 后下一个才启动）在旧 host 死亡确认处调用，维持计数「当前存活 host
 * 数」语义
 *
 * 只与 DasTestAdmissionNoteHostStart 精确配对使用，不触发违约判定；
 * 计数器按测试归零，误配对只影响本测试内的存活计数。
 */
inline void DasTestAdmissionNoteHostStop()
{
    --TestAdmission::detail::HostStartCounter();
}

// ====== 申报宏（文件作用域、TEST 定义旁使用） ======

/// 精确申报：suite_name.test_name 最大同时启动 host 数。注册器静态
/// 构造时顺带安装 listener（D-21：含申报宏的 TU 所在 exe 即装妥，
/// main 零改动）。
#define DAS_TEST_MAX_HOSTS(suite_name, test_name, max_hosts)                   \
    namespace                                                                  \
    {                                                                          \
        struct DasAdmissionEntry_##suite_name##_##test_name                    \
        {                                                                      \
            DasAdmissionEntry_##suite_name##_##test_name()                     \
            {                                                                  \
                ::TestAdmission::RegisterDeclaredHosts(                        \
                    #suite_name,                                               \
                    #test_name,                                                \
                    (max_hosts));                                              \
                ::TestAdmission::InstallAdmissionListenerOnce();               \
            }                                                                  \
        };                                                                     \
        [[maybe_unused]]                                                       \
        DasAdmissionEntry_##suite_name##_##test_name                           \
            DasAdmissionEntry_##suite_name##_##test_name##_instance;           \
    }

/// 套件级默认申报：suite 内未精确申报的用例全部适用
#define DAS_TEST_MAX_HOSTS_SUITE(suite_name, max_hosts)                        \
    namespace                                                                  \
    {                                                                          \
        struct DasAdmissionSuiteEntry_##suite_name                             \
        {                                                                      \
            DasAdmissionSuiteEntry_##suite_name()                              \
            {                                                                  \
                ::TestAdmission::RegisterSuiteDefaultHosts(                    \
                    #suite_name,                                               \
                    (max_hosts));                                              \
                ::TestAdmission::InstallAdmissionListenerOnce();               \
            }                                                                  \
        };                                                                     \
        [[maybe_unused]]                                                       \
        DasAdmissionSuiteEntry_##suite_name                                    \
            DasAdmissionSuiteEntry_##suite_name##_instance;                    \
    }

#endif // DAS_TEST_ADMISSION_H
