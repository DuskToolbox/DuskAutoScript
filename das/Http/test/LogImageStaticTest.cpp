#include "controller/DasImageController.hpp"

#include <das/Utils/PathText.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace
{
    TEST(LogImageStaticTest, SanitizeImagePath_RejectsTraversal)
    {
        const std::filesystem::path session_dir =
            DAS_PATH_LITERAL("E:/das/debug/20260801-123456-8841");
        EXPECT_FALSE(
            Das::Http::DasImageController::SanitizeImagePath(
                "../x",
                session_dir));
        EXPECT_FALSE(
            Das::Http::DasImageController::SanitizeImagePath(
                "a/../../b",
                session_dir));
        EXPECT_FALSE(
            Das::Http::DasImageController::SanitizeImagePath(
                "/etc/passwd",
                session_dir));
        EXPECT_FALSE(
            Das::Http::DasImageController::SanitizeImagePath(
                "C:evil.jpg",
                session_dir));
        EXPECT_FALSE(
            Das::Http::DasImageController::SanitizeImagePath(
                "..",
                session_dir));
        EXPECT_FALSE(
            Das::Http::DasImageController::SanitizeImagePath("", session_dir));
    }

    TEST(LogImageStaticTest, SanitizeImagePath_AcceptsNormalRef)
    {
        const std::filesystem::path session_dir =
            DAS_PATH_LITERAL("E:/das/debug/20260801-123456-8841");
        const auto result = Das::Http::DasImageController::SanitizeImagePath(
            "img/0001.jpg",
            session_dir);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(
            result->filename(),
            std::filesystem::path{DAS_PATH_LITERAL("0001.jpg")});
        const auto base = session_dir.lexically_normal();
        EXPECT_TRUE(
            Das::Utils::PathToUtf8(*result).starts_with(
                Das::Utils::PathToUtf8(base)));
    }
} // namespace
