#include <das/Core/Logger/LogRecorderRuntime.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <regex>
#include <string>

namespace
{
    std::filesystem::path UniqueTempDir(const char* test_name)
    {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        auto path = std::filesystem::current_path() / "logger-test-output"
                    / (std::string{test_name} + "-" + std::to_string(stamp));
        std::filesystem::remove_all(path);
        return path;
    }

    class SessionDirFixture : public ::testing::Test
    {
    protected:
        void TearDown() override
        {
            Das::Core::LogRecorderRuntime::Shutdown();
            Das::Core::LogRecorderRuntime::ResetForTest();
        }
    };
} // namespace

TEST_F(SessionDirFixture, SessionDirTest_NamedWithTimestampAndPid)
{
    const auto dir = UniqueTempDir("SessionDirNamed");
    Das::Core::LogRecorderRuntime::ResetForTest();
    ASSERT_EQ(Das::Core::LogRecorderRuntime::Initialize(dir), DAS_S_OK);

    const auto session_dir = Das::Core::LogRecorderRuntime::SessionDir();
    ASSERT_FALSE(session_dir.empty());
    EXPECT_TRUE(
        std::regex_match(
            session_dir.filename().string(),
            std::regex{R"(^\d{8}-\d{6}-\d+$)"}))
        << session_dir;

    EXPECT_EQ(
        Das::Core::LogRecorderRuntime::UserJsonlPath(),
        session_dir / "user.jsonl");
    EXPECT_TRUE(std::filesystem::exists(session_dir / "img"));

    Das::Core::LogRecorderRuntime::Shutdown();
    EXPECT_FALSE(std::filesystem::exists(session_dir));
}

TEST_F(SessionDirFixture, SessionDirTest_ShutdownIdempotent)
{
    const auto dir = UniqueTempDir("ShutdownIdempotent");
    Das::Core::LogRecorderRuntime::ResetForTest();
    ASSERT_EQ(Das::Core::LogRecorderRuntime::Initialize(dir), DAS_S_OK);

    Das::Core::LogRecorderRuntime::Shutdown();
    EXPECT_NO_THROW(Das::Core::LogRecorderRuntime::Shutdown());
    EXPECT_TRUE(Das::Core::LogRecorderRuntime::SessionDir().empty());
}

TEST_F(SessionDirFixture, SessionDirTest_UninitializedReturnsEmpty)
{
    Das::Core::LogRecorderRuntime::ResetForTest();
    EXPECT_TRUE(Das::Core::LogRecorderRuntime::SessionDir().empty());
    EXPECT_TRUE(Das::Core::LogRecorderRuntime::UserJsonlPath().empty());
}
