#include <das/Core/Logger/DasLogNotifierSink.h>
#include <das/Core/Logger/DasLogSessionImpl.h>
#include <das/Core/Logger/DasProcessLogRecorderImpl.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/PathText.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    std::filesystem::path UniqueTempDir(const char* test_name)
    {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        auto path = std::filesystem::current_path()
                    / DAS_PATH_LITERAL("logger-test-output")
                    / Das::Utils::PathFromUtf8(
                        std::string{test_name} + "-" + std::to_string(stamp));
        std::filesystem::remove_all(path);
        return path;
    }

    // 从 IDasLogSession 取 session_dir（std::filesystem::path）。
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

    // 测试夹具：每个用例建独立 session + recorder，析构时 RAII 自动清目录。
    // recorder 注入 notifier（来自 g_logger 固定 sink）+ session。
    class LogRecorderFixture : public ::testing::Test
    {
    protected:
        Das::DasPtr<Das::ExportInterface::IDasLogNotifier>        notifier_;
        Das::DasPtr<Das::ExportInterface::IDasLogSession>         session_;
        Das::DasPtr<Das::ExportInterface::IDasProcessLogRecorder> recorder_;
        std::filesystem::path debug_directory_;

        void SetUp() override
        {
            GetDasLogNotifier(notifier_.Put());
            // notifier 可能为空（早期单测环境无 g_logger sink）；recorder
            // 容忍空。
        }

        // 创建 session + recorder：debug_directory 由调用方提供。
        DasResult CreateContext(const std::filesystem::path& debug_directory)
        {
            debug_directory_ = debug_directory;
            const std::string directory_str =
                Das::Utils::PathToUtf8(debug_directory);
            Das::DasPtr<IDasReadOnlyString> directory_param;
            const auto cr = CreateIDasReadOnlyStringFromUtf8(
                directory_str.c_str(),
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
} // namespace

TEST_F(LogRecorderFixture, LogRecorderTest_RecordWritesJsonlEntry)
{
    const auto dir = UniqueTempDir("RecordWritesJsonl");
    ASSERT_EQ(CreateContext(dir), DAS_S_OK);
    ASSERT_TRUE(session_);
    ASSERT_TRUE(recorder_);

    auto entry = Das::DasPtr<Das::ExportInterface::IDasLogEntry>{
        new Das::Core::IDasLogEntryImpl{}};
    DasU8StringOnStack text{"hello"};
    ASSERT_EQ(
        entry->SetType(Das::ExportInterface::DAS_LOG_TYPE_INFO),
        DAS_S_OK);
    ASSERT_EQ(entry->AddText(&text), DAS_S_OK);
    EXPECT_EQ(recorder_->Record(entry.Get()), DAS_S_OK);

    const auto jsonl_path = ReadUserJsonlPath(session_.Get());
    ASSERT_FALSE(jsonl_path.empty());
    ASSERT_TRUE(std::filesystem::exists(jsonl_path));

    std::ifstream input{jsonl_path};
    std::string   first_line;
    ASSERT_TRUE(std::getline(input, first_line));

    auto parsed = DAS::Utils::ParseYyjsonFromString(first_line);
    ASSERT_TRUE(parsed.has_value());
    auto obj = parsed->as_object();
    ASSERT_TRUE(obj.has_value());

    auto type_value = (*obj)[std::string_view("type")].as_string();
    ASSERT_TRUE(type_value.has_value());
    EXPECT_EQ(*type_value, "info");

    auto text_value = (*obj)[std::string_view("text")].as_string();
    ASSERT_TRUE(text_value.has_value());
    EXPECT_EQ(*text_value, "hello");

    auto image_refs = (*obj)[std::string_view("imageRefs")].as_array();
    ASSERT_TRUE(image_refs.has_value());
    EXPECT_TRUE(image_refs->empty());

    auto fields = (*obj)[std::string_view("fields")].as_object();
    ASSERT_TRUE(fields.has_value());
    EXPECT_TRUE(fields->empty());
}

TEST_F(LogRecorderFixture, LogRecorderTest_RecordAppendsMultipleLines)
{
    const auto dir = UniqueTempDir("RecordAppends");
    ASSERT_EQ(CreateContext(dir), DAS_S_OK);

    DasU8StringOnStack first_text{"first"};
    auto first_entry = Das::DasPtr<Das::ExportInterface::IDasLogEntry>{
        new Das::Core::IDasLogEntryImpl{}};
    ASSERT_EQ(
        first_entry->SetType(Das::ExportInterface::DAS_LOG_TYPE_INFO),
        DAS_S_OK);
    ASSERT_EQ(first_entry->AddText(&first_text), DAS_S_OK);
    ASSERT_EQ(recorder_->Record(first_entry.Get()), DAS_S_OK);

    DasU8StringOnStack second_text{"second"};
    auto second_entry = Das::DasPtr<Das::ExportInterface::IDasLogEntry>{
        new Das::Core::IDasLogEntryImpl{}};
    ASSERT_EQ(
        second_entry->SetType(Das::ExportInterface::DAS_LOG_TYPE_SUCCESS),
        DAS_S_OK);
    ASSERT_EQ(second_entry->AddText(&second_text), DAS_S_OK);
    ASSERT_EQ(recorder_->Record(second_entry.Get()), DAS_S_OK);

    const auto    jsonl_path = ReadUserJsonlPath(session_.Get());
    std::ifstream input{jsonl_path};
    std::string   line;
    int           line_count = 0;
    while (std::getline(input, line))
    {
        ++line_count;
        auto parsed = DAS::Utils::ParseYyjsonFromString(line);
        ASSERT_TRUE(parsed.has_value());
    }
    EXPECT_EQ(line_count, 2);
}

// 统一通知：recorder Record 写盘后调 notifier->Signal(UserJsonl) 置 dirty
// flag。 Http 16ms 定时器轮询 Consume 拿到 true 时 hub->Broadcast
// api/v1/logs/get tick。
TEST_F(LogRecorderFixture, LogRecorderTest_RecordSignalsUserJsonlNotifier)
{
    const auto dir = UniqueTempDir("RecordSignalsNotifier");
    ASSERT_EQ(CreateContext(dir), DAS_S_OK);

    if (!notifier_)
    {
        GTEST_SKIP() << "DasLogNotifierSink not present in g_logger";
    }

    // 先 Consume 排空残留 dirty（其他用例可能已 Signal）。
    bool was_dirty = false;
    notifier_->Consume(
        Das::ExportInterface::DAS_LOG_CHANNEL_USER_JSONL,
        &was_dirty);
    was_dirty = false;

    DasU8StringOnStack text{"notify-me"};
    auto               entry = Das::DasPtr<Das::ExportInterface::IDasLogEntry>{
        new Das::Core::IDasLogEntryImpl{}};
    ASSERT_EQ(
        entry->SetType(Das::ExportInterface::DAS_LOG_TYPE_WARNING),
        DAS_S_OK);
    ASSERT_EQ(entry->AddText(&text), DAS_S_OK);
    EXPECT_EQ(recorder_->Record(entry.Get()), DAS_S_OK);

    EXPECT_EQ(
        notifier_->Consume(
            Das::ExportInterface::DAS_LOG_CHANNEL_USER_JSONL,
            &was_dirty),
        DAS_S_OK);
    EXPECT_TRUE(was_dirty);
}

// 无 session 时（factory 传 null）Record 返 OBJECT_NOT_INIT（recorder 容忍空
// notifier 但不容忍空 session——session 是写盘的硬前置）。
TEST_F(LogRecorderFixture, LogRecorderTest_RecordWithoutSessionFails)
{
    Das::DasPtr<Das::ExportInterface::IDasProcessLogRecorder> recorder;
    ASSERT_EQ(
        CreateIDasProcessLogRecorder(notifier_.Get(), nullptr, recorder.Put()),
        DAS_S_OK);

    auto entry = Das::DasPtr<Das::ExportInterface::IDasLogEntry>{
        new Das::Core::IDasLogEntryImpl{}};
    DasU8StringOnStack text{"hello"};
    ASSERT_EQ(
        entry->SetType(Das::ExportInterface::DAS_LOG_TYPE_INFO),
        DAS_S_OK);
    ASSERT_EQ(entry->AddText(&text), DAS_S_OK);
    EXPECT_EQ(recorder->Record(entry.Get()), DAS_E_OBJECT_NOT_INIT);
}

TEST_F(LogRecorderFixture, LogRecorderTest_InvalidEnumRejected)
{
    auto entry = Das::DasPtr<Das::ExportInterface::IDasLogEntry>{
        new Das::Core::IDasLogEntryImpl{}};
    EXPECT_EQ(
        entry->SetType(static_cast<Das::ExportInterface::DasLogType>(99)),
        DAS_E_INVALID_ENUM);
}

// IDasLogEntry getter 经接口读字段（替代 dynamic_cast 到 IDasLogEntryImpl）：
// 本地 impl 路径返真值；跨进程 proxy 路径在 ProcessLogRecorderIpcTest 覆盖。
TEST_F(LogRecorderFixture, LogRecorderTest_EntryGetterReturnsFieldValues)
{
    auto entry = Das::DasPtr<Das::ExportInterface::IDasLogEntry>{
        new Das::Core::IDasLogEntryImpl{}};
    DasU8StringOnStack first_text{"first"};
    DasU8StringOnStack second_text{"second"};
    ASSERT_EQ(
        entry->SetType(Das::ExportInterface::DAS_LOG_TYPE_STRUCTURAL),
        DAS_S_OK);
    ASSERT_EQ(entry->AddText(&first_text), DAS_S_OK);
    ASSERT_EQ(entry->AddText(&second_text), DAS_S_OK);

    Das::ExportInterface::DasLogType type =
        Das::ExportInterface::DAS_LOG_TYPE_INFO;
    EXPECT_EQ(entry->GetType(&type), DAS_S_OK);
    EXPECT_EQ(type, Das::ExportInterface::DAS_LOG_TYPE_STRUCTURAL);

    size_t count = 0;
    EXPECT_EQ(entry->GetTextCount(&count), DAS_S_OK);
    EXPECT_EQ(count, 2u);

    Das::DasPtr<IDasReadOnlyString> text0;
    EXPECT_EQ(entry->GetText(0, text0.Put()), DAS_S_OK);
    const char* text0_utf8 = nullptr;
    ASSERT_TRUE(text0);
    ASSERT_EQ(text0->GetUtf8(&text0_utf8), DAS_S_OK);
    EXPECT_STREQ(text0_utf8, "first");

    // 越界返 OUT_OF_RANGE。
    Das::DasPtr<IDasReadOnlyString> text_out_of_range;
    EXPECT_EQ(entry->GetText(99, text_out_of_range.Put()), DAS_E_OUT_OF_RANGE);
}

// IDasLogSession RAII：session 释时目录被删。
TEST_F(LogRecorderFixture, LogRecorderTest_SessionDirectoryDeletedOnRelease)
{
    const auto dir = UniqueTempDir("SessionRaiiDelete");
    ASSERT_EQ(CreateContext(dir), DAS_S_OK);
    const auto session_dir = ReadSessionDirectory(session_.Get());
    ASSERT_FALSE(session_dir.empty());
    ASSERT_TRUE(std::filesystem::exists(session_dir));

    session_.Reset();
    recorder_.Reset(); // recorder 也持 session 引用，Reset 后才真正释放。
    EXPECT_FALSE(std::filesystem::exists(session_dir));
}
