#include "controller/DasImageController.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace
{
    TEST(LogImageStaticTest, SanitizeImagePath_RejectsTraversal)
    {
        const std::filesystem::path session_dir =
            "E:/das/debug/20260801-123456-8841";
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
            "E:/das/debug/20260801-123456-8841";
        const auto result = Das::Http::DasImageController::SanitizeImagePath(
            "img/0001.jpg",
            session_dir);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->filename(), std::filesystem::path{"0001.jpg"});
        const auto base = session_dir.lexically_normal();
        EXPECT_TRUE(
            result->string().starts_with(base.string()));
    }
} // namespace
