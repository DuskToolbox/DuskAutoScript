#include <das/Core/Logger/DasLogNotifierSink.h>
#include <das/Core/Logger/DasLogSessionImpl.h>
#include <das/Core/Logger/DasProcessLogRecorderImpl.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

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

    class LogArchiveJsonlFixture : public ::testing::Test
    {
    protected:
        Das::DasPtr<Das::ExportInterface::IDasLogNotifier>        notifier_;
        Das::DasPtr<Das::ExportInterface::IDasLogSession>         session_;
        Das::DasPtr<Das::ExportInterface::IDasProcessLogRecorder> recorder_;
        std::filesystem::path debug_directory_;

        void SetUp() override { GetDasLogNotifier(notifier_.Put()); }

        DasResult CreateContext(const std::filesystem::path& debug_directory)
        {
            debug_directory_ = debug_directory;
            const auto directory_str = debug_directory.u8string();
            Das::DasPtr<IDasReadOnlyString> directory_param;
            const auto cr = CreateIDasReadOnlyStringFromUtf8(
                reinterpret_cast<const char*>(directory_str.c_str()),
                directory_param.Put());
            if (DAS::IsFailed(cr))
            {
                return cr;
            }
            const auto session_cr =
                CreateLogSession(directory_param.Get(), session_.Put());
            if (DAS::IsFailed(session_cr))
            {
                return session_cr;
            }
            return CreateIDasProcessLogRecorder(
                notifier_.Get(),
                session_.Get(),
                recorder_.Put());
        }
    };

    void RecordTextEntry(
        Das::ExportInterface::IDasProcessLogRecorder* recorder,
        const char*                                   text,
        Das::ExportInterface::DasLogType              type)
    {
        auto entry = Das::DasPtr<Das::ExportInterface::IDasLogEntry>{
            new Das::Core::IDasLogEntryImpl{}};
        DasU8StringOnStack text_str{text};
        ASSERT_EQ(entry->SetType(type), DAS_S_OK);
        ASSERT_EQ(entry->AddText(&text_str), DAS_S_OK);
        ASSERT_EQ(recorder->Record(entry.Get()), DAS_S_OK);
    }

    std::vector<std::string> SplitLines(const std::string& joined)
    {
        std::vector<std::string> lines;
        std::string_view         s(joined);
        std::size_t              pos = 0;
        while (pos < s.size())
        {
            const auto nl = s.find('\n', pos);
            const auto len = nl == std::string_view::npos
                                 ? std::string_view::npos
                                 : nl - pos;
            auto       line = s.substr(pos, len);
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            if (!line.empty())
            {
                lines.emplace_back(line);
            }
            if (nl == std::string_view::npos)
            {
                break;
            }
            pos = nl + 1;
        }
        return lines;
    }
} // namespace

TEST_F(LogArchiveJsonlFixture, LogArchiveJsonlTest_ReadTailReadsJsonLines)
{
    const auto dir = UniqueTempDir("ArchiveReadTail");
    ASSERT_EQ(CreateContext(dir), DAS_S_OK);

    RecordTextEntry(
        recorder_.Get(),
        "first",
        Das::ExportInterface::DAS_LOG_TYPE_INFO);
    RecordTextEntry(
        recorder_.Get(),
        "second",
        Das::ExportInterface::DAS_LOG_TYPE_WARNING);

    Das::DasPtr<Das::ExportInterface::IDasLogArchive> archive;
    ASSERT_EQ(
        CreateIDasLogArchive(
            Das::ExportInterface::DAS_LOG_ARCHIVE_SOURCE_USER_JSONL,
            session_.Get(),
            archive.Put()),
        DAS_S_OK);

    Das::DasPtr<IDasReadOnlyString> lines_str;
    uint64_t                        next = 0;
    uint64_t                        earliest = 0;
    ASSERT_EQ(
        archive->ReadTail(10, lines_str.Put(), &next, &earliest),
        DAS_S_OK);
    ASSERT_TRUE(lines_str);
    const char* utf8 = nullptr;
    ASSERT_EQ(lines_str->GetUtf8(&utf8), DAS_S_OK);
    ASSERT_TRUE(utf8 != nullptr);

    const auto lines = SplitLines(utf8);
    ASSERT_EQ(lines.size(), 2u);
    for (const auto& line : lines)
    {
        auto parsed = DAS::Utils::ParseYyjsonFromString(line);
        ASSERT_TRUE(parsed.has_value());
    }
}

TEST_F(LogArchiveJsonlFixture, LogArchiveJsonlTest_ReadAfterAdvancesCursor)
{
    const auto dir = UniqueTempDir("ArchiveReadAfter");
    ASSERT_EQ(CreateContext(dir), DAS_S_OK);

    RecordTextEntry(
        recorder_.Get(),
        "first",
        Das::ExportInterface::DAS_LOG_TYPE_INFO);

    Das::DasPtr<Das::ExportInterface::IDasLogArchive> archive;
    ASSERT_EQ(
        CreateIDasLogArchive(
            Das::ExportInterface::DAS_LOG_ARCHIVE_SOURCE_USER_JSONL,
            session_.Get(),
            archive.Put()),
        DAS_S_OK);

    Das::DasPtr<IDasReadOnlyString> lines_str;
    uint64_t                        next = 0;
    ASSERT_EQ(archive->ReadAfter(0, lines_str.Put(), &next), DAS_S_OK);
    ASSERT_TRUE(lines_str);
    ASSERT_GT(next, 0u);

    Das::DasPtr<IDasReadOnlyString> lines_again;
    uint64_t                        next_again = 0;
    ASSERT_EQ(
        archive->ReadAfter(next, lines_again.Put(), &next_again),
        DAS_S_OK);
    ASSERT_TRUE(lines_again);
    const char* utf8 = nullptr;
    ASSERT_EQ(lines_again->GetUtf8(&utf8), DAS_S_OK);
    EXPECT_TRUE(utf8 == nullptr || std::string_view(utf8).empty());
}

TEST_F(LogArchiveJsonlFixture, LogArchiveJsonlTest_ReadAfterDropsHalfLine)
{
    const auto dir = UniqueTempDir("ArchiveHalfLine");
    ASSERT_EQ(CreateContext(dir), DAS_S_OK);

    RecordTextEntry(
        recorder_.Get(),
        "first",
        Das::ExportInterface::DAS_LOG_TYPE_INFO);

    // 在 user.jsonl 末尾手写半行 JSON（不换行）：读方应丢弃该半行。
    const auto jsonl_path = ReadUserJsonlPath(session_.Get());
    ASSERT_FALSE(jsonl_path.empty());
    {
        std::ofstream output{jsonl_path, std::ios::app | std::ios::binary};
        output << "{\"time\":123,\"type\":\"info\",\"text\":\"half\"";
    }

    Das::DasPtr<Das::ExportInterface::IDasLogArchive> archive;
    ASSERT_EQ(
        CreateIDasLogArchive(
            Das::ExportInterface::DAS_LOG_ARCHIVE_SOURCE_USER_JSONL,
            session_.Get(),
            archive.Put()),
        DAS_S_OK);

    Das::DasPtr<IDasReadOnlyString> lines_str;
    uint64_t                        next = 0;
    ASSERT_EQ(archive->ReadAfter(0, lines_str.Put(), &next), DAS_S_OK);
    ASSERT_TRUE(lines_str);
    const char* utf8 = nullptr;
    ASSERT_EQ(lines_str->GetUtf8(&utf8), DAS_S_OK);
    ASSERT_TRUE(utf8 != nullptr);

    const auto lines = SplitLines(utf8);
    ASSERT_EQ(lines.size(), 1u);
    auto parsed = DAS::Utils::ParseYyjsonFromString(lines.front());
    ASSERT_TRUE(parsed.has_value());
}

// archive 不持 session 时（factory 传 null）也能构造，读时返回空结果
// （不崩溃）。
TEST_F(LogArchiveJsonlFixture, LogArchiveJsonlTest_NullSessionReturnsEmpty)
{
    Das::DasPtr<Das::ExportInterface::IDasLogArchive> archive;
    ASSERT_EQ(
        CreateIDasLogArchive(
            Das::ExportInterface::DAS_LOG_ARCHIVE_SOURCE_USER_JSONL,
            nullptr,
            archive.Put()),
        DAS_S_OK);

    Das::DasPtr<IDasReadOnlyString> lines_str;
    uint64_t                        next = 0;
    uint64_t                        earliest = 0;
    ASSERT_EQ(
        archive->ReadTail(10, lines_str.Put(), &next, &earliest),
        DAS_S_OK);
    ASSERT_TRUE(lines_str);
    const char* utf8 = nullptr;
    ASSERT_EQ(lines_str->GetUtf8(&utf8), DAS_S_OK);
    EXPECT_TRUE(utf8 == nullptr || std::string_view(utf8).empty());
}
