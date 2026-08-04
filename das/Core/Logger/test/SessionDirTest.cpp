#include <das/Core/Logger/DasLogSessionImpl.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
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

    std::filesystem::path ReadSessionDirectory(
        Das::ExportInterface::IDasLogSession* session)
    {
        Das::DasPtr<IDasReadOnlyString> directory_str;
        if (DAS::IsFailed(session->GetSessionDir(directory_str.Put()))
            || !directory_str)
        {
            return {};
        }
        const char* utf8 = nullptr;
        if (DAS::IsFailed(directory_str->GetUtf8(&utf8)) || utf8 == nullptr)
        {
            return {};
        }
        return std::filesystem::path{reinterpret_cast<const char8_t*>(utf8)};
    }

    std::filesystem::path ReadUserJsonlPath(
        Das::ExportInterface::IDasLogSession* session)
    {
        Das::DasPtr<IDasReadOnlyString> path_str;
        if (DAS::IsFailed(session->GetUserJsonlPath(path_str.Put()))
            || !path_str)
        {
            return {};
        }
        const char* utf8 = nullptr;
        if (DAS::IsFailed(path_str->GetUtf8(&utf8)) || utf8 == nullptr)
        {
            return {};
        }
        return std::filesystem::path{reinterpret_cast<const char8_t*>(utf8)};
    }

    class SessionDirFixture : public ::testing::Test
    {
    protected:
        Das::DasPtr<Das::ExportInterface::IDasLogSession> session_;

        DasResult CreateContext(const std::filesystem::path& debug_directory)
        {
            const auto directory_str = debug_directory.u8string();
            Das::DasPtr<IDasReadOnlyString> directory_param;
            const auto cr = CreateIDasReadOnlyStringFromUtf8(
                reinterpret_cast<const char*>(directory_str.c_str()),
                directory_param.Put());
            if (DAS::IsFailed(cr))
            {
                return cr;
            }
            return CreateLogSession(directory_param.Get(), session_.Put());
        }
    };
} // namespace

TEST_F(SessionDirFixture, SessionDirTest_NamedWithTimestampAndPid)
{
    const auto dir = UniqueTempDir("SessionDirNamed");
    ASSERT_EQ(CreateContext(dir), DAS_S_OK);

    const auto session_dir = ReadSessionDirectory(session_.Get());
    ASSERT_FALSE(session_dir.empty());
    EXPECT_TRUE(
        std::regex_match(
            session_dir.filename().string(),
            std::regex{R"(^\d{8}-\d{6}-\d+$)"}))
        << session_dir;

    EXPECT_EQ(ReadUserJsonlPath(session_.Get()), session_dir / "user.jsonl");
    EXPECT_TRUE(std::filesystem::exists(session_dir / "img"));
}

// RAII：session 释时目录被递归删除。
TEST_F(SessionDirFixture, SessionDirTest_DirectoryDeletedOnRelease)
{
    const auto dir = UniqueTempDir("SessionRaiiDelete");
    ASSERT_EQ(CreateContext(dir), DAS_S_OK);

    const auto session_dir = ReadSessionDirectory(session_.Get());
    ASSERT_FALSE(session_dir.empty());
    ASSERT_TRUE(std::filesystem::exists(session_dir));

    session_.Reset();
    EXPECT_FALSE(std::filesystem::exists(session_dir));
}

// 多次 NextImageSeq 单调递增（fetch_add + 1，从 1 开始）。
TEST_F(SessionDirFixture, SessionDirTest_NextImageSeqMonotonic)
{
    const auto dir = UniqueTempDir("SessionImageSeq");
    ASSERT_EQ(CreateContext(dir), DAS_S_OK);

    uint32_t first_seq = 0;
    uint32_t second_seq = 0;
    uint32_t third_seq = 0;
    ASSERT_EQ(session_->NextImageSeq(&first_seq), DAS_S_OK);
    ASSERT_EQ(session_->NextImageSeq(&second_seq), DAS_S_OK);
    ASSERT_EQ(session_->NextImageSeq(&third_seq), DAS_S_OK);
    EXPECT_EQ(first_seq, 1u);
    EXPECT_EQ(second_seq, 2u);
    EXPECT_EQ(third_seq, 3u);
}
