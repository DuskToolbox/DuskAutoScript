// LogArchiveSpdlogTest：验证 IDasLogArchiveSpdlogFileImpl（恢复原版设计）
// 的 ReadTail / ReadAfter 行为，通过 g_logger 触发真实 spdlog
// rotating_file_sink_mt 写盘 + flush，然后用 archive 读回来。轮转分支因 50MB×2
// 阈值难自动触发，仅覆盖 常规增量与 offset>size 重置分支。
#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace
{
    // 唯一 marker，避免与其他测试 / 全局初始化日志混淆断言。
    std::string UniqueMarker(const char* tag)
    {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        return std::string{"spdlog-archive-marker-"} + tag + "-"
               + std::to_string(stamp);
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

// ReadTail 返回 spdlog 文件末尾 N 行，最新写入的 marker 必在末尾。
TEST(LogArchiveSpdlogTest, ReadTailReturnsLastLines)
{
    const auto marker = UniqueMarker("ReadTail");
    DAS_CORE_LOG_INFO("LogArchiveSpdlogTest trigger {}", marker);
    Das::Core::g_logger->flush();

    Das::DasPtr<Das::ExportInterface::IDasLogArchive> archive;
    ASSERT_EQ(
        CreateIDasLogArchive(
            Das::ExportInterface::DAS_LOG_ARCHIVE_SOURCE_SPDLOG_FILE,
            nullptr,
            archive.Put()),
        DAS_S_OK);
    ASSERT_TRUE(archive);

    Das::DasPtr<IDasReadOnlyString> lines_str;
    uint64_t                        next = 0;
    uint64_t                        earliest = 0;
    ASSERT_EQ(
        archive->ReadTail(50, lines_str.Put(), &next, &earliest),
        DAS_S_OK);
    ASSERT_TRUE(lines_str);
    ASSERT_GT(next, 0u);

    const char* utf8 = nullptr;
    ASSERT_EQ(lines_str->GetUtf8(&utf8), DAS_S_OK);
    ASSERT_NE(utf8, nullptr);
    const auto lines = SplitLines(utf8);
    ASSERT_FALSE(lines.empty());
    // 末尾若干行中必有一行包含 marker（spdlog pattern 含时间戳/线程前缀）
    bool found = false;
    for (const auto& line : lines)
    {
        if (line.find(marker) != std::string::npos)
        {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

// ReadAfter 从指定 offset 拉取增量；先 ReadTail 拿当前 next，再写新 marker，
// ReadTail 后 ReadAfter(next) 应包含新 marker。
TEST(LogArchiveSpdlogTest, ReadAfterAdvancesCursor)
{
    Das::DasPtr<Das::ExportInterface::IDasLogArchive> archive;
    ASSERT_EQ(
        CreateIDasLogArchive(
            Das::ExportInterface::DAS_LOG_ARCHIVE_SOURCE_SPDLOG_FILE,
            nullptr,
            archive.Put()),
        DAS_S_OK);

    Das::DasPtr<IDasReadOnlyString> lines_str;
    uint64_t                        offset_before = 0;
    uint64_t                        earliest = 0;
    ASSERT_EQ(
        archive->ReadTail(10, lines_str.Put(), &offset_before, &earliest),
        DAS_S_OK);
    ASSERT_GT(offset_before, 0u);

    const auto marker = UniqueMarker("ReadAfter");
    DAS_CORE_LOG_INFO("LogArchiveSpdlogTest increment {}", marker);
    Das::Core::g_logger->flush();

    Das::DasPtr<IDasReadOnlyString> after_str;
    uint64_t                        offset_after = 0;
    ASSERT_EQ(
        archive->ReadAfter(offset_before, after_str.Put(), &offset_after),
        DAS_S_OK);
    ASSERT_GE(offset_after, offset_before);

    const char* utf8 = nullptr;
    ASSERT_EQ(after_str->GetUtf8(&utf8), DAS_S_OK);
    ASSERT_NE(utf8, nullptr);
    const auto lines = SplitLines(utf8);
    bool       found = false;
    for (const auto& line : lines)
    {
        if (line.find(marker) != std::string::npos)
        {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

// offset 超出当前文件大小（含轮转语义）：next_offset 复位为 0，提示 UI 重发
// tail。
TEST(LogArchiveSpdlogTest, ReadAfterBeyondSizeResetsCursor)
{
    Das::DasPtr<Das::ExportInterface::IDasLogArchive> archive;
    ASSERT_EQ(
        CreateIDasLogArchive(
            Das::ExportInterface::DAS_LOG_ARCHIVE_SOURCE_SPDLOG_FILE,
            nullptr,
            archive.Put()),
        DAS_S_OK);

    // 偏一个明显超出实际文件大小的 offset（rotating_file 单文件上限 50MB，
    // 这里取一个比当前文件大的值）
    const uint64_t huge_offset = static_cast<uint64_t>(1) << 40; // 1 TiB

    Das::DasPtr<IDasReadOnlyString> lines_str;
    uint64_t                        next = 1;
    ASSERT_EQ(
        archive->ReadAfter(huge_offset, lines_str.Put(), &next),
        DAS_S_OK);
    EXPECT_EQ(next, 0u);
}

// CreateIDasLogArchive 对未知 source（FORCE_DWORD 哨兵）返
// DAS_E_INVALID_ARGUMENT。
TEST(LogArchiveSpdlogTest, FactoryRejectsUnknownSource)
{
    Das::DasPtr<Das::ExportInterface::IDasLogArchive> archive;
    const auto                                        rc = CreateIDasLogArchive(
        Das::ExportInterface::DAS_LOG_ARCHIVE_SOURCE_FORCE_DWORD,
        nullptr,
        archive.Put());
    EXPECT_EQ(rc, DAS_E_INVALID_ARGUMENT);
    EXPECT_FALSE(archive);
}
