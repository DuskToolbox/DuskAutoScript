// MaapiTask schedule brain: fake-clock 单测覆盖 Do-cache-GetNextExecutionTime
// seam / daily next-run / daily 跨午夜 / off→fail / envelope schedule key
// 解析。
#include "../src/MaapiTask.h"
#include "FakeMaaApiBoundary.h"

#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>

#include <gtest/gtest.h>

#include <ctime>
#include <string>
#include <string_view>

namespace
{
    using namespace Das::Plugins::DasMaaPi;
    using Das::Plugins::DasMaaPi::Test::FakeMaaApiBoundary;

    // 跨平台 timegm（_mkgmtime Win32 / timegm POSIX），镜像 Core 的
    // DasDateToUnix 语义（DasDate 字段按 UTC 解释）。
    std::time_t PortableTimegm(std::tm* tm)
    {
#ifdef _WIN32
        return _mkgmtime(tm);
#else
        return timegm(tm);
#endif
    }

    std::tm* PortableLocaltime(const std::time_t* timer, std::tm* buf)
    {
#ifdef _WIN32
        return localtime_s(buf, timer) == 0 ? buf : nullptr;
#else
        return localtime_r(timer, buf);
#endif
    }

    // DasDate(UTC 字段) → unix，复刻 Core 的 DasDateToUnix 以验证 round-trip
    // （核心断言：实现若误填本地 HH，round-trip 会偏一个时区）。
    std::time_t DasDateToUnix(const Das::ExportInterface::DasDate& d)
    {
        std::tm tm{};
        tm.tm_year = d.year - 1900;
        tm.tm_mon = d.month - 1;
        tm.tm_mday = d.day;
        tm.tm_hour = d.hour;
        tm.tm_min = d.minute;
        tm.tm_sec = d.second;
        tm.tm_isdst = -1;
        return PortableTimegm(&tm);
    }

    // 构造一个"本地 HH:MM:00 今天"的 unix 时间戳，用作 fake clock。
    std::time_t LocalTodayUnix(int hour, int minute)
    {
        std::time_t now = std::time(nullptr);
        std::tm     local{};
        PortableLocaltime(&now, &local);
        local.tm_hour = hour;
        local.tm_min = minute;
        local.tm_sec = 0;
        local.tm_isdst = -1;
        return std::mktime(&local);
    }

    // 构造一个"本地 HH:MM:00 相对今天偏移 day_offset 天"的 unix 时间戳。mktime
    // normalize tm_mday 进位，自动处理跨月/年与 DST。用作跨日 catch-up 测试的
    // fake clock（昨日/明日同时刻）。
    std::time_t LocalDaysOffsetTodayUnix(int day_offset, int hour, int minute)
    {
        std::time_t now = std::time(nullptr);
        std::tm     local{};
        PortableLocaltime(&now, &local);
        local.tm_mday += day_offset;
        local.tm_hour = hour;
        local.tm_min = minute;
        local.tm_sec = 0;
        local.tm_isdst = -1;
        return std::mktime(&local);
    }

    // 独立计算"宿主机本地下一次 HH:MM"的 unix 时间戳（与实现同算法），
    // 用作 daily round-trip 期望值。slot 已过则推到明日。
    std::time_t ExpectedNextDailyUnix(std::time_t now, int hour, int minute)
    {
        std::tm local{};
        PortableLocaltime(&now, &local);
        local.tm_hour = hour;
        local.tm_min = minute;
        local.tm_sec = 0;
        local.tm_isdst = -1;
        std::time_t slot = std::mktime(&local);
        if (slot <= now)
        {
            local.tm_mday += 1;
            local.tm_isdst = -1;
            slot = std::mktime(&local);
        }
        return slot;
    }

    // 构造完整 envelope JSON，schedule 由参数注入（非 optional，必须显式补
    // key）。
    std::string EnvelopeJsonWithSchedule(std::string_view schedule_json)
    {
        std::string json =
            R"({"version":1,"pluginGuid":")" + std::string(PLUGIN_GUID_TEXT)
            + R"(","taskTypeGuid":")" + std::string(TASK_GUID_TEXT)
            + R"(","maapi":{"interfaceDirectory":"C:/maa","controllerName":"Android",)"
              R"("controller":{"name":"Android","type":"Adb","readPath":"","address":"","adbPath":"adb","configJson":"{}","agentPath":""},)"
              R"("resourceName":"Official","resourcePaths":[],"attachResourcePaths":[],"resourceHash":"hash-expected",)"
              R"("failFast":true,"requiresAgentRuntime":false,"agent":[],)"
              R"("piEnv":{"interfaceVersion":"2","clientName":"DAS","clientVersion":"0.1","clientLanguage":"cpp","clientMaafwVersion":"test","projectVersion":"","controllerJson":"","resourceJson":""},)"
              R"("focusTranslationsPath":"","schedule":)"
            + std::string(schedule_json)
            + R"(,"controllerInitParams":{"controllerType":"Adb","win32":null,"gamepad":null,)"
              R"("adb":{"adbPath":"adb","address":"","screencapMethod":0,"inputMethod":0,"configJson":"{}","agentPath":""},)"
              R"("dbg":null,"displayShortSide":null,"displayLongSide":null,"displayRaw":null},)"
              R"("pretasks":[],"tasks":[{"taskName":"T","entry":"E","pipelineOverride":{}}]}})";
        return json;
    }

    Das::DasPtr<Das::ExportInterface::IDasJson> ParseDasJson(std::string json)
    {
        Das::DasPtr<Das::ExportInterface::IDasJson> result;
        EXPECT_EQ(ParseDasJsonFromString(json.c_str(), result.Put()), DAS_S_OK);
        return result;
    }

    // RAII boundary hook（照 MaapiRunTaskComponentTest 范式）。
    class ScopedBoundaryHook final
    {
    public:
        explicit ScopedBoundaryHook(FakeMaaApiBoundary& boundary)
            : boundary_(boundary)
        {
            SetMaaApiBoundaryForTest(&boundary_);
        }
        ~ScopedBoundaryHook() { SetMaaApiBoundaryForTest(nullptr); }

        ScopedBoundaryHook(const ScopedBoundaryHook&) = delete;
        ScopedBoundaryHook& operator=(const ScopedBoundaryHook&) = delete;

    private:
        FakeMaaApiBoundary& boundary_;
    };

    // 驱动 Do-cache seam：构造含 schedule 的 envelope → MaapiTask::Do（fake
    // boundary）→ schedule_ 经 ParseExecutionEnvelope 缓存。返回就绪的 task。
    void DriveDoWithSchedule(MaapiTask& task, std::string_view schedule_json)
    {
        FakeMaaApiBoundary       fake;
        const ScopedBoundaryHook hook(fake);
        auto json = ParseDasJson(EnvelopeJsonWithSchedule(schedule_json));
        ASSERT_EQ(task.Do(nullptr, nullptr, json.Get()), DAS_S_OK);
    }
} // namespace

// Do 解析 envelope 后缓存 schedule_，GetNextExecutionTime 读缓存返回
// daily（若未缓存 schedule_ 保持默认 off 则会返回 DAS_E_NO_IMPLEMENTATION）。
TEST(MaapiSchedule, DoCachesSchedule_FromEnvelope)
{
    MaapiTask task;
    DriveDoWithSchedule(
        task,
        R"({"kind":"daily","intervalMinutes":0,"dailyTime":"09:00","dailyCatchUp":false})");
    task.SetClockForTest([stamp = LocalTodayUnix(8, 0)] { return stamp; });

    Das::ExportInterface::DasDate date{};
    ASSERT_EQ(task.GetNextExecutionTime(&date), DAS_S_OK);
    EXPECT_EQ(
        DasDateToUnix(date),
        ExpectedNextDailyUnix(LocalTodayUnix(8, 0), 9, 0));
}

// fake clock 08:00 local，dailyTime="09:00" → 返回今日 09:00 local 的
// DasDate（UTC round-trip == 今日 09:00 local 的 mktime 值）。
TEST(MaapiSchedule, DailyNextRun_ReturnsNextLocalSlot)
{
    MaapiTask task;
    DriveDoWithSchedule(
        task,
        R"({"kind":"daily","intervalMinutes":0,"dailyTime":"09:00","dailyCatchUp":false})");
    const std::time_t now = LocalTodayUnix(8, 0);
    task.SetClockForTest([stamp = now] { return stamp; });

    Das::ExportInterface::DasDate date{};
    ASSERT_EQ(task.GetNextExecutionTime(&date), DAS_S_OK);
    EXPECT_EQ(DasDateToUnix(date), ExpectedNextDailyUnix(now, 9, 0));
}

// fake clock 23:00 local，dailyTime="01:30" → 返回明日 01:30 local
// （跨午夜核心断言：slot 已过推进到明日，UTC round-trip 正确）。
TEST(MaapiSchedule, DailyCrossMidnight_AdvancesToTomorrow)
{
    MaapiTask task;
    DriveDoWithSchedule(
        task,
        R"({"kind":"daily","intervalMinutes":0,"dailyTime":"01:30","dailyCatchUp":false})");
    const std::time_t now = LocalTodayUnix(23, 0);
    task.SetClockForTest([stamp = now] { return stamp; });

    Das::ExportInterface::DasDate date{};
    ASSERT_EQ(task.GetNextExecutionTime(&date), DAS_S_OK);
    // 明日 01:30 必严格晚于 now（23:00 今日）。
    EXPECT_GT(DasDateToUnix(date), now);
    EXPECT_EQ(DasDateToUnix(date), ExpectedNextDailyUnix(now, 1, 30));
}

// schedule kind=""(off) → GetNextExecutionTime 返回失败，Core sentinel 兜底
// （既有契约保持）。
TEST(MaapiSchedule, OffKind_ReturnsFailureForSentinel)
{
    MaapiTask task;
    DriveDoWithSchedule(
        task,
        R"({"kind":"","intervalMinutes":0,"dailyTime":"","dailyCatchUp":false})");

    Das::ExportInterface::DasDate date{};
    EXPECT_EQ(task.GetNextExecutionTime(&date), DAS_E_NO_IMPLEMENTATION);
}

// ParseExecutionEnvelope 接受 schedule key（同 focus_translations_path 非
// optional 范式，规避 caster 陷阱）。
TEST(MaapiSchedule, ParseExecutionEnvelope_AcceptsScheduleKey)
{
    auto value = Das::Utils::ParseYyjsonFromString(EnvelopeJsonWithSchedule(
        R"({"kind":"daily","intervalMinutes":45,"dailyTime":"09:00","dailyCatchUp":true})"));
    ASSERT_TRUE(value.has_value());

    auto parsed = ParseExecutionEnvelope(*value);
    ASSERT_EQ(parsed.result, DAS_S_OK);
    EXPECT_EQ(parsed.envelope.maapi.schedule.kind, "daily");
    EXPECT_EQ(parsed.envelope.maapi.schedule.interval_minutes, 45);
    EXPECT_EQ(parsed.envelope.maapi.schedule.daily_time, "09:00");
    EXPECT_TRUE(parsed.envelope.maapi.schedule.daily_catch_up);
}

// interval schedule：GetNextExecutionTime 返回 now + intervalMinutes（不补跑，
// 跳到下一未来档）。fake clock 固定 unix T，intervalMinutes=120 → round-trip
// DasDateToUnix == T + 7200s。
TEST(MaapiSchedule, IntervalNextRun_ReturnsNowPlusInterval)
{
    MaapiTask task;
    DriveDoWithSchedule(
        task,
        R"({"kind":"interval","intervalMinutes":120,"dailyTime":"","dailyCatchUp":false})");
    const std::time_t now = LocalTodayUnix(12, 0);
    task.SetClockForTest([stamp = now] { return stamp; });

    Das::ExportInterface::DasDate date{};
    ASSERT_EQ(task.GetNextExecutionTime(&date), DAS_S_OK);
    EXPECT_EQ(DasDateToUnix(date), now + 120 * 60);
}

// interval 固定不补跑：即便停机恢复跨越多档（now 已过若干档），也只返回
// now + interval（绝不连跑补 N 档）。断言结果严格等于 now + 单个 interval。
TEST(MaapiSchedule, IntervalNoBackfill_SkipsToNextFuture)
{
    MaapiTask task;
    DriveDoWithSchedule(
        task,
        R"({"kind":"interval","intervalMinutes":30,"dailyTime":"","dailyCatchUp":false})");
    const std::time_t now = LocalTodayUnix(12, 0);
    task.SetClockForTest([stamp = now] { return stamp; });

    Das::ExportInterface::DasDate date{};
    ASSERT_EQ(task.GetNextExecutionTime(&date), DAS_S_OK);
    // 严格等于 now + 30 分钟（不是 now + 60 / now + 90 等多档补跑）。
    EXPECT_EQ(DasDateToUnix(date), now + 30 * 60);
}

// interval intervalMinutes<=0 防热循环：返回 DAS_E_INVALID_ARGUMENT，不产出
// next-run=now 导致 Core 立即重跑的热循环。
TEST(MaapiSchedule, IntervalZeroOrNegative_ReturnsInvalidArgument)
{
    MaapiTask task_zero;
    DriveDoWithSchedule(
        task_zero,
        R"({"kind":"interval","intervalMinutes":0,"dailyTime":"","dailyCatchUp":false})");
    Das::ExportInterface::DasDate date_zero{};
    EXPECT_EQ(
        task_zero.GetNextExecutionTime(&date_zero),
        DAS_E_INVALID_ARGUMENT);

    MaapiTask task_neg;
    DriveDoWithSchedule(
        task_neg,
        R"({"kind":"interval","intervalMinutes":-5,"dailyTime":"","dailyCatchUp":false})");
    Das::ExportInterface::DasDate date_neg{};
    EXPECT_EQ(task_neg.GetNextExecutionTime(&date_neg), DAS_E_INVALID_ARGUMENT);
}

// daily catch-up 状态机：catchUp=true 且今日 slot 已过且今日未跑 → 第 1 次返回
// now（补跑一次），last_fired_ymd_=今日；第 2 次同日调用返回明日 slot（去重
// 生效，防 catch-up 热循环核心断言）。
TEST(MaapiSchedule, DailyCatchUp_ReturnsNowOnce_ThenTomorrow)
{
    MaapiTask task;
    DriveDoWithSchedule(
        task,
        R"({"kind":"daily","intervalMinutes":0,"dailyTime":"09:00","dailyCatchUp":true})");
    const std::time_t now =
        LocalTodayUnix(10, 0); // 今日 10:00，slot 09:00 已过
    task.SetClockForTest([stamp = now] { return stamp; });

    // 第 1 次：今日 slot 已过 + catchUp + 未跑 → 返回 now（补跑一次）。
    Das::ExportInterface::DasDate first_date{};
    ASSERT_EQ(task.GetNextExecutionTime(&first_date), DAS_S_OK);
    EXPECT_EQ(DasDateToUnix(first_date), now);

    // 第 2 次：同日再问 → 去重，返回明日 09:00 slot（不再返回
    // now，否则热循环）。
    Das::ExportInterface::DasDate second_date{};
    ASSERT_EQ(task.GetNextExecutionTime(&second_date), DAS_S_OK);
    EXPECT_EQ(DasDateToUnix(second_date), ExpectedNextDailyUnix(now, 9, 0));
    EXPECT_GT(DasDateToUnix(second_date), now);
}

// daily catchUp=false：今日 slot 已过 → 直接返回明日 slot（不补跑）。即便状态
// 机就位，catchUp=false 路径永不返回 now。
TEST(MaapiSchedule, DailyCatchUp_Disabled_SkipsToTomorrow)
{
    MaapiTask task;
    DriveDoWithSchedule(
        task,
        R"({"kind":"daily","intervalMinutes":0,"dailyTime":"09:00","dailyCatchUp":false})");
    const std::time_t now =
        LocalTodayUnix(10, 0); // 今日 10:00，slot 09:00 已过
    task.SetClockForTest([stamp = now] { return stamp; });

    Das::ExportInterface::DasDate date{};
    ASSERT_EQ(task.GetNextExecutionTime(&date), DAS_S_OK);
    EXPECT_EQ(DasDateToUnix(date), ExpectedNextDailyUnix(now, 9, 0));
    EXPECT_GT(DasDateToUnix(date), now); // 明日 slot，不是 now

    // 同日第二次仍返回明日 slot（catchUp=false 永不返回 now）。
    Das::ExportInterface::DasDate second_date{};
    ASSERT_EQ(task.GetNextExecutionTime(&second_date), DAS_S_OK);
    EXPECT_GT(DasDateToUnix(second_date), now);
}

// daily catch-up 跨日重置：昨日已补跑（last_fired_ymd_=昨日）不影响今日决策——
// 今日 slot 已过 + catchUp → 仍补跑一次返回 now（today_ymd != last_fired_ymd_
// 状态机去重按日边界自然失效）。
TEST(MaapiSchedule, DailyCatchUp_NewDay_ResetsLastFired)
{
    MaapiTask task;
    DriveDoWithSchedule(
        task,
        R"({"kind":"daily","intervalMinutes":0,"dailyTime":"09:00","dailyCatchUp":true})");

    // 昨日 10:00：slot 09:00 已过 + catchUp → 补跑，last_fired_ymd_ = 昨日
    // ymd。
    const std::time_t yesterday = LocalDaysOffsetTodayUnix(-1, 10, 0);
    task.SetClockForTest([stamp = yesterday] { return stamp; });
    Das::ExportInterface::DasDate yesterday_date{};
    ASSERT_EQ(task.GetNextExecutionTime(&yesterday_date), DAS_S_OK);
    EXPECT_EQ(DasDateToUnix(yesterday_date), yesterday);

    // 今日 10:00：跨日后 today_ymd != 昨日 last_fired_ymd_ → catchUp 再次生效。
    const std::time_t today = LocalTodayUnix(10, 0);
    task.SetClockForTest([stamp = today] { return stamp; });
    Das::ExportInterface::DasDate today_date{};
    ASSERT_EQ(task.GetNextExecutionTime(&today_date), DAS_S_OK);
    EXPECT_EQ(DasDateToUnix(today_date), today); // 补跑返回 now，非明日 slot
}
