#include "TestAdmission.h"

#include <das/Utils/ScopedEnvironmentVariable.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    constexpr std::uint64_t HEADROOM_ABUNDANT_BYTES =
        16ull * 1024 * 1024 * 1024;
    constexpr std::uint64_t HEADROOM_INSUFFICIENT_BYTES = 1024;

    /// RAII 恢复 seam：本 exe 的 listener 对全部用例生效，泄漏 fake
    /// 会污染后续用例的闸门读数
    class AdmissionSeamGuard
    {
    public:
        AdmissionSeamGuard() = default;

        ~AdmissionSeamGuard()
        {
            TestAdmission::SetAdmissionHeadroomSourceForTest(nullptr);
            TestAdmission::SetAdmissionClockForTest(nullptr);
        }

        AdmissionSeamGuard(const AdmissionSeamGuard&) = delete;
        AdmissionSeamGuard& operator=(const AdmissionSeamGuard&) = delete;
    };

    /// 可编程 fake 余量源：按调用次序回放读数序列，越界重复最后一个值
    class ProgrammableHeadroomSource
    {
    public:
        explicit ProgrammableHeadroomSource(
            std::vector<CommitMemory::CommitHeadroom> readings)
            : readings_(std::move(readings))
        {
        }

        CommitMemory::CommitHeadroom Next()
        {
            const size_t index = call_count_ < readings_.size()
                                     ? call_count_
                                     : readings_.size() - 1;
            ++call_count_;
            return readings_[index];
        }

        size_t call_count() const { return call_count_; }

    private:
        std::vector<CommitMemory::CommitHeadroom> readings_;
        size_t                                    call_count_{0};
    };

    /// 步进 fake 时钟：每次调用前进 step_milliseconds
    class SteppingClock
    {
    public:
        explicit SteppingClock(std::uint64_t step_milliseconds)
            : step_milliseconds_(step_milliseconds)
        {
        }

        std::uint64_t Next()
        {
            current_milliseconds_ += step_milliseconds_;
            return current_milliseconds_;
        }

    private:
        std::uint64_t step_milliseconds_;
        std::uint64_t current_milliseconds_{0};
    };

    /// seam 是裸函数指针（无捕获），带状态的 fake 经进程内指针转发
    ProgrammableHeadroomSource* active_headroom_source = nullptr;
    SteppingClock*              active_clock = nullptr;

    CommitMemory::CommitHeadroom ActiveHeadroomSourceNext()
    {
        return active_headroom_source->Next();
    }

    std::uint64_t ActiveClockNext() { return active_clock->Next(); }

    CommitMemory::CommitHeadroom Headroom(
        std::uint64_t available_bytes,
        bool          precise)
    {
        return {available_bytes, precise};
    }

    void ActivateAdmissionSeams(
        ProgrammableHeadroomSource& source,
        SteppingClock&              clock)
    {
        active_headroom_source = &source;
        active_clock = &clock;
        TestAdmission::SetAdmissionHeadroomSourceForTest(
            &ActiveHeadroomSourceNext);
        TestAdmission::SetAdmissionClockForTest(&ActiveClockNext);
    }
} // namespace

// 注册表测试专用申报：落在独立套件名上，不改变 TestAdmissionTest
// 套件用例自身的闸门申报值
DAS_TEST_MAX_HOSTS_SUITE(AdmissionRegistrySuite, 1)
DAS_TEST_MAX_HOSTS(AdmissionRegistrySuite, ExactEntryOverridesSuiteDefault, 2)

// 查找序：精确 suite.test → suite 默认 → 0（未申报直通）
TEST(TestAdmissionTest, AdmissionRegistry)
{
    EXPECT_EQ(
        TestAdmission::LookupDeclaredHosts(
            "AdmissionRegistrySuite",
            "ExactEntryOverridesSuiteDefault"),
        2);
    EXPECT_EQ(
        TestAdmission::LookupDeclaredHosts(
            "AdmissionRegistrySuite",
            "NeverDeclaredTest"),
        1);
    EXPECT_EQ(
        TestAdmission::LookupDeclaredHosts("NoSuchSuite", "NoSuchTest"),
        0);
}

TEST(TestAdmissionTest, AdmissionGate_AdmitsImmediatelyWhenHeadroomSufficient)
{
    AdmissionSeamGuard         seam_guard;
    ProgrammableHeadroomSource source(
        {Headroom(HEADROOM_ABUNDANT_BYTES, true)});
    SteppingClock clock(40000);
    ActivateAdmissionSeams(source, clock);

    EXPECT_EQ(
        TestAdmission::WaitForAdmission(1),
        TestAdmission::AdmissionOutcome::Admitted);
    // 零等待：一次读数即放行
    EXPECT_EQ(source.call_count(), static_cast<size_t>(1));
}

TEST(TestAdmissionTest, AdmissionGate_WaitsWhenInsufficient)
{
    AdmissionSeamGuard seam_guard;
    // 前 2 轮不足、第 3 轮起充足；fake 时钟每调用步进 40000ms
    ProgrammableHeadroomSource source(
        {Headroom(HEADROOM_INSUFFICIENT_BYTES, true),
         Headroom(HEADROOM_INSUFFICIENT_BYTES, true),
         Headroom(HEADROOM_ABUNDANT_BYTES, true)});
    SteppingClock clock(40000);
    ActivateAdmissionSeams(source, clock);

    EXPECT_EQ(
        TestAdmission::WaitForAdmission(1),
        TestAdmission::AdmissionOutcome::Admitted);
    // 恰在余量转足的第 3 轮放行（等待 2 轮）
    EXPECT_EQ(source.call_count(), static_cast<size_t>(3));
}

TEST(TestAdmissionTest, AdmissionGate_TimesOutWithNamedFailure)
{
    AdmissionSeamGuard seam_guard;
    // 余量恒不足；时钟步进 40000ms，3 次推进后越过 120000ms 总超时
    ProgrammableHeadroomSource source(
        {Headroom(HEADROOM_INSUFFICIENT_BYTES, true)});
    SteppingClock clock(40000);
    ActivateAdmissionSeams(source, clock);

    EXPECT_EQ(
        TestAdmission::WaitForAdmission(1),
        TestAdmission::AdmissionOutcome::TimedOut);
    const std::string failure_line = TestAdmission::GetLastNamedFailureLine();
    // 具名失败四字段齐全（不静默、可取证）
    EXPECT_NE(failure_line.find("event=denied"), std::string::npos);
    EXPECT_NE(failure_line.find("declared=1"), std::string::npos);
    EXPECT_NE(failure_line.find("needed_bytes="), std::string::npos);
    EXPECT_NE(failure_line.find("available_bytes=1024"), std::string::npos);
    EXPECT_NE(failure_line.find("waited_ms=120000"), std::string::npos);
}

TEST(TestAdmissionTest, AdmissionGate_DebugModeDisablesTimeout)
{
    AdmissionSeamGuard seam_guard;
    // DAS_DEBUG=1：总超时归零（无限等待）
    const Das::Utils::ScopedEnvironmentVariable debug_mode(
        "DAS_DEBUG",
        std::optional<std::string>{"1"});
    EXPECT_EQ(
        TestAdmission::GetAdmissionTimeoutMs(),
        static_cast<std::uint64_t>(0));

    // 时钟推过 120000ms 后余量才转足：无限等待语义下仍被放行
    ProgrammableHeadroomSource source(
        {Headroom(HEADROOM_INSUFFICIENT_BYTES, true),
         Headroom(HEADROOM_INSUFFICIENT_BYTES, true),
         Headroom(HEADROOM_INSUFFICIENT_BYTES, true),
         Headroom(HEADROOM_ABUNDANT_BYTES, true)});
    SteppingClock clock(60000);
    ActivateAdmissionSeams(source, clock);

    EXPECT_EQ(
        TestAdmission::WaitForAdmission(1),
        TestAdmission::AdmissionOutcome::Admitted);
}

TEST(TestAdmissionTest, AdmissionGate_ReadFailureFailsLoud)
{
    AdmissionSeamGuard seam_guard;
    // 读数失败（available_bytes==0）：不放行、不挂死，立即具名失败
    ProgrammableHeadroomSource source({Headroom(0, false)});
    SteppingClock              clock(40000);
    ActivateAdmissionSeams(source, clock);

    EXPECT_EQ(
        TestAdmission::WaitForAdmission(1),
        TestAdmission::AdmissionOutcome::ReadFailure);
    const std::string failure_line = TestAdmission::GetLastNamedFailureLine();
    EXPECT_NE(failure_line.find("event=read-failure"), std::string::npos);
    EXPECT_EQ(source.call_count(), static_cast<size_t>(1));
}

// 本用例未申报（默认 0）：一次 host 启动即违约，违约的 ADD_FAILURE
// 被捕获断言（EXPECT_NONFATAL_FAILURE 不让预期失败外泄为用例失败）
TEST(TestAdmissionTest, NoteHostStart_ViolationFailsLoud)
{
    EXPECT_NONFATAL_FAILURE({ DasTestAdmissionNoteHostStart(); }, "");
}

// 申报 2 内两次启动不报违约
DAS_TEST_MAX_HOSTS(
    TestAdmissionTest,
    NoteHostStart_WithinDeclaredLimitIsSilent,
    2)

TEST(TestAdmissionTest, NoteHostStart_WithinDeclaredLimitIsSilent)
{
    DasTestAdmissionNoteHostStart();
    DasTestAdmissionNoteHostStart();
    EXPECT_FALSE(::testing::Test::HasFailure());
}

// 本 exe 含申报 TU，listener 已随宏注册器恰好一次安装；观测自身：
// 重复安装会表现为 start_events==2
TEST(TestAdmissionTest, AdmissionListener_ExactlyOneEventSetPerTest)
{
    const TestAdmission::ListenerEventCounts counts =
        TestAdmission::GetListenerEventCounts(
            "AdmissionListener_ExactlyOneEventSetPerTest");
    EXPECT_EQ(counts.start_events, static_cast<std::uint64_t>(1));
    EXPECT_EQ(counts.admitted_events, static_cast<std::uint64_t>(1));
    // done 事件在本测试结束时才发生（体内恒 0）；其恰好一次由 ctest
    // 输出层每用例恰一行 event=done 的验收锚定
    EXPECT_EQ(counts.done_events, static_cast<std::uint64_t>(0));
}

TEST(TestAdmissionTest, AdmissionListener_LogLineFormat)
{
    const std::string line = TestAdmission::BuildAdmissionLogLine(
        "SuiteName",
        "TestName",
        "admitted",
        1791430673505,
        {{"declared", "2"}, {"queued", "137"}});
    // ② 行可 grep 还原排队时长：前缀 + 字段 + queued= 毫秒
    EXPECT_NE(
        line.find(
            "[das-admission] suite=SuiteName test=TestName event=admitted"),
        std::string::npos);
    EXPECT_NE(line.find("ts=1791430673505"), std::string::npos);
    EXPECT_NE(line.find("queued=137"), std::string::npos);
}
