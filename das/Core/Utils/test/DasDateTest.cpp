#include <das/DasApi.h>
#include <das/_autogen/idl/abi/DasBasicTypes.h>
#include <gtest/gtest.h>

using Das::ExportInterface::DasDate;

namespace
{
    DasDate MakeDate(
        std::uint16_t year,
        std::uint8_t  month,
        std::uint8_t  day,
        std::uint8_t  hour = 0,
        std::uint8_t  minute = 0,
        std::uint8_t  second = 0)
    {
        return DasDate{year, month, day, hour, minute, second};
    }

    // 间隔装字段（D-B6：+1天={day:1}，其余字段 0=零偏移）。
    DasDate MakeInterval(
        std::uint16_t year = 0,
        std::uint8_t  month = 0,
        std::uint8_t  day = 0,
        std::uint8_t  hour = 0,
        std::uint8_t  minute = 0,
        std::uint8_t  second = 0)
    {
        return DasDate{year, month, day, hour, minute, second};
    }
} // namespace

// ============================================================================
// DasDateAdd — 归一化进位（D-B3：tm 字段相加 + timegm 链归一化，UTC 字段）
// ============================================================================

TEST(DasDateTest, AddMonthCarryNormalizesLikeTmMktime)
{
    // 1 月 31 日 + 1 月 = 2 月 31 日 → 归一化进位 → 3 月 3 日。
    const DasDate base = MakeDate(2026, 1, 31);
    const DasDate delta = MakeInterval(0, 1, 0);
    DasDate       result{};
    ASSERT_EQ(DasDateAdd(&base, &delta, &result), DAS_S_OK);
    EXPECT_EQ(result.year, 2026);
    EXPECT_EQ(result.month, 3);
    EXPECT_EQ(result.day, 3);
}

TEST(DasDateTest, AddYearLeapDayCarriesToMarchFirst)
{
    // 闰日 + 1 年 = 2025 年 2 月 29 日（不存在）→ 归一化 → 3 月 1 日。
    const DasDate base = MakeDate(2024, 2, 29);
    const DasDate delta = MakeInterval(1, 0, 0);
    DasDate       result{};
    ASSERT_EQ(DasDateAdd(&base, &delta, &result), DAS_S_OK);
    EXPECT_EQ(result.year, 2025);
    EXPECT_EQ(result.month, 3);
    EXPECT_EQ(result.day, 1);
}

TEST(DasDateTest, AddDayLeapYearFebruaryRoundtrip)
{
    // 2028 是闰年，2 月 28 + 1 天 = 2 月 29（不进位）。
    const DasDate base = MakeDate(2028, 2, 28);
    const DasDate delta = MakeInterval(0, 0, 1);
    DasDate       result{};
    ASSERT_EQ(DasDateAdd(&base, &delta, &result), DAS_S_OK);
    EXPECT_EQ(result.year, 2028);
    EXPECT_EQ(result.month, 2);
    EXPECT_EQ(result.day, 29);
}

TEST(DasDateTest, AddDayCarriesAcrossYearBoundary)
{
    // 跨年：2026-12-31 + 1 天 = 2027-01-01。
    const DasDate base = MakeDate(2026, 12, 31);
    const DasDate delta = MakeInterval(0, 0, 1);
    DasDate       result{};
    ASSERT_EQ(DasDateAdd(&base, &delta, &result), DAS_S_OK);
    EXPECT_EQ(result.year, 2027);
    EXPECT_EQ(result.month, 1);
    EXPECT_EQ(result.day, 1);
}

TEST(DasDateTest, AddCompositeOffsetsCoexist)
{
    // 复合偏移并存：+1 月 +1 天同施（D-B6「复合偏移可并存」）。
    // 2026-01-31 + {month:1, day:1} = 2 月 32 日 → 3 月 4 日。
    const DasDate base = MakeDate(2026, 1, 31);
    const DasDate delta = MakeInterval(0, 1, 1);
    DasDate       result{};
    ASSERT_EQ(DasDateAdd(&base, &delta, &result), DAS_S_OK);
    EXPECT_EQ(result.year, 2026);
    EXPECT_EQ(result.month, 3);
    EXPECT_EQ(result.day, 4);
}

TEST(DasDateTest, AddHourMinuteSecondCarryIntoDay)
{
    // 时间字段进位：23:59:59 + {second:1} = 次日 00:00:00。
    const DasDate base = MakeDate(2026, 1, 31, 23, 59, 59);
    const DasDate delta = MakeInterval(0, 0, 0, 0, 0, 1);
    DasDate       result{};
    ASSERT_EQ(DasDateAdd(&base, &delta, &result), DAS_S_OK);
    EXPECT_EQ(result.year, 2026);
    EXPECT_EQ(result.month, 2);
    EXPECT_EQ(result.day, 1);
    EXPECT_EQ(result.hour, 0);
    EXPECT_EQ(result.minute, 0);
    EXPECT_EQ(result.second, 0);
}

TEST(DasDateTest, AddZeroDeltaIsIdentity)
{
    // 零 delta 恒等（全字段 0 = 零偏移合法）。
    const DasDate base = MakeDate(2026, 5, 17, 12, 34, 56);
    const DasDate delta = MakeInterval();
    DasDate       result{};
    ASSERT_EQ(DasDateAdd(&base, &delta, &result), DAS_S_OK);
    EXPECT_EQ(result.year, base.year);
    EXPECT_EQ(result.month, base.month);
    EXPECT_EQ(result.day, base.day);
    EXPECT_EQ(result.hour, base.hour);
    EXPECT_EQ(result.minute, base.minute);
    EXPECT_EQ(result.second, base.second);
}

TEST(DasDateTest, AddNullPointersReturnInvalidPointer)
{
    const DasDate base = MakeDate(2026, 1, 31);
    const DasDate delta = MakeInterval(0, 1, 0);
    DasDate       result{};

    EXPECT_EQ(DasDateAdd(nullptr, &delta, &result), DAS_E_INVALID_POINTER);
    EXPECT_EQ(DasDateAdd(&base, nullptr, &result), DAS_E_INVALID_POINTER);
    EXPECT_EQ(DasDateAdd(&base, &delta, nullptr), DAS_E_INVALID_POINTER);
    EXPECT_EQ(DasDateAdd(nullptr, nullptr, nullptr), DAS_E_INVALID_POINTER);
}

TEST(DasDateTest, AddInvalidCalendarFieldsReturnInvalidArgument)
{
    const DasDate month_zero = MakeDate(2026, 0, 31);
    const DasDate month_thirteen = MakeDate(2026, 13, 31);
    const DasDate day_zero = MakeDate(2026, 1, 0);
    const DasDate delta = MakeInterval(0, 1, 0);
    const DasDate interval_month_thirteen = MakeInterval(0, 13, 0);
    const DasDate valid_base = MakeDate(2026, 1, 31);
    DasDate       result{};

    // 基线侧非法：month=0 / month=13 / day=0（不夹紧、拒绝非法）。
    EXPECT_EQ(DasDateAdd(&month_zero, &delta, &result), DAS_E_INVALID_ARGUMENT);
    EXPECT_EQ(
        DasDateAdd(&month_thirteen, &delta, &result),
        DAS_E_INVALID_ARGUMENT);
    EXPECT_EQ(DasDateAdd(&day_zero, &delta, &result), DAS_E_INVALID_ARGUMENT);
    // 间隔侧越界：month=13 超出名义域上界。
    EXPECT_EQ(
        DasDateAdd(&valid_base, &interval_month_thirteen, &result),
        DAS_E_INVALID_ARGUMENT);
}

// ============================================================================
// DasDateCompare — 三态比较（归一化后转 unix，UTC 字段语义）
// ============================================================================

TEST(DasDateTest, CompareLeftBeforeRightReturnsMinusOne)
{
    const DasDate left = MakeDate(2026, 1, 1);
    const DasDate right = MakeDate(2026, 1, 2);
    int32_t       out = 0;
    ASSERT_EQ(DasDateCompare(&left, &right, &out), DAS_S_OK);
    EXPECT_EQ(out, -1);
}

TEST(DasDateTest, CompareEqualDatesReturnZero)
{
    const DasDate left = MakeDate(2026, 6, 15, 8, 30, 0);
    const DasDate right = MakeDate(2026, 6, 15, 8, 30, 0);
    int32_t       out = -1;
    ASSERT_EQ(DasDateCompare(&left, &right, &out), DAS_S_OK);
    EXPECT_EQ(out, 0);
}

TEST(DasDateTest, CompareLeftAfterRightReturnsOne)
{
    const DasDate left = MakeDate(2027, 1, 1);
    const DasDate right = MakeDate(2026, 12, 31);
    int32_t       out = 0;
    ASSERT_EQ(DasDateCompare(&left, &right, &out), DAS_S_OK);
    EXPECT_EQ(out, 1);
}

TEST(DasDateTest, CompareCarriedFieldsCompareByNormalizedInstant)
{
    // 进位形字段（day=31 落在 2 月属名义域内合法输入）与归一化形同瞬间：
    // 2026-02-31 归一化 = 2026-03-03 → unix 相等 → 0。
    const DasDate left = MakeDate(2026, 2, 31, 0, 0, 0);
    const DasDate right = MakeDate(2026, 3, 3, 0, 0, 0);
    int32_t       out = 99;
    ASSERT_EQ(DasDateCompare(&left, &right, &out), DAS_S_OK);
    EXPECT_EQ(out, 0);
}

TEST(DasDateTest, CompareNullPointersReturnInvalidPointer)
{
    const DasDate left = MakeDate(2026, 1, 1);
    const DasDate right = MakeDate(2026, 1, 2);
    int32_t       out = 0;

    EXPECT_EQ(DasDateCompare(nullptr, &right, &out), DAS_E_INVALID_POINTER);
    EXPECT_EQ(DasDateCompare(&left, nullptr, &out), DAS_E_INVALID_POINTER);
    EXPECT_EQ(DasDateCompare(&left, &right, nullptr), DAS_E_INVALID_POINTER);
    EXPECT_EQ(DasDateCompare(nullptr, nullptr, nullptr), DAS_E_INVALID_POINTER);
}

TEST(DasDateTest, CompareInvalidFieldsReturnInvalidArgument)
{
    const DasDate month_zero = MakeDate(2026, 0, 1);
    const DasDate valid = MakeDate(2026, 1, 1);
    const DasDate month_thirteen = MakeDate(2026, 13, 2);
    const DasDate day_zero = MakeDate(2026, 1, 0);
    int32_t       out = 0;

    EXPECT_EQ(
        DasDateCompare(&month_zero, &valid, &out),
        DAS_E_INVALID_ARGUMENT);
    EXPECT_EQ(
        DasDateCompare(&valid, &month_thirteen, &out),
        DAS_E_INVALID_ARGUMENT);
    EXPECT_EQ(DasDateCompare(&valid, &day_zero, &out), DAS_E_INVALID_ARGUMENT);
}
