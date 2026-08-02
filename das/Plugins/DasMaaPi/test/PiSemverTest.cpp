#include "../src/PiSemver.h"

#include <gtest/gtest.h>

#include <string>

namespace
{
    using namespace Das::Plugins::DasMaaPi;

    TEST(PiSemver, StripsVPrefixAndComparesEqual)
    {
        EXPECT_EQ(CompareVersions("v1.6.0", "1.6.0"), 0);
        EXPECT_EQ(CompareVersions("V1.6.0", "1.6.0"), 0);
    }

    TEST(PiSemver, ReleaseBeatsPrerelease)
    {
        // Formal release > prerelease (1.6.0 > 1.6.0-beta.1)
        EXPECT_GT(CompareVersions("1.6.0", "1.6.0-beta.1"), 0);
        EXPECT_LT(CompareVersions("1.6.0-beta.1", "1.6.0"), 0);
    }

    TEST(PiSemver, PrereleaseComparesLexically)
    {
        EXPECT_GT(CompareVersions("1.6.0-beta.2", "1.6.0-beta.1"), 0);
        EXPECT_LT(CompareVersions("1.6.0-beta.1", "1.6.0-beta.2"), 0);
        EXPECT_EQ(CompareVersions("v1.6.0-beta.1", "1.6.0-beta.1"), 0);
    }

    TEST(PiSemver, NumericComparisonNotStringComparison)
    {
        // "10.0" > "9.0" must hold numerically (string compare would invert
        // it).
        EXPECT_GT(CompareVersions("10.0", "9.0"), 0);
        EXPECT_GT(CompareVersions("10.0", "9.9.9"), 0);
    }

    TEST(PiSemver, CoercesShortVersions)
    {
        // "2.0" coerces to "2.0.0"
        EXPECT_EQ(CompareVersions("2.0", "2.0.0"), 0);
        EXPECT_GT(CompareVersions("2.0", "1.9.9"), 0);
    }

    TEST(PiSemver, FallsBackToStringComparison)
    {
        // Neither parses nor coerces -> normalized string order.
        EXPECT_GT(CompareVersions("beta", "alpha"), 0);
        EXPECT_LT(CompareVersions("alpha", "beta"), 0);
        EXPECT_EQ(CompareVersions("abc", "abc"), 0);
    }

    TEST(PiSemver, ParseSemVerSeparatesPrerelease)
    {
        PiSemVer ver;
        ASSERT_TRUE(ParseSemVer("1.6.0-beta.1", ver));
        EXPECT_EQ(ver.major, 1);
        EXPECT_EQ(ver.minor, 6);
        EXPECT_EQ(ver.patch, 0);
        EXPECT_EQ(ver.prerelease, "beta.1");
    }

    TEST(PiSemver, ParseSemVerRejectsNonFullVersions)
    {
        PiSemVer ver;
        EXPECT_FALSE(ParseSemVer("2.0", ver));
        EXPECT_FALSE(ParseSemVer("1.6", ver));
        EXPECT_FALSE(ParseSemVer("not-a-version", ver));
        EXPECT_FALSE(ParseSemVer("1.x.0", ver));
    }
} // namespace
