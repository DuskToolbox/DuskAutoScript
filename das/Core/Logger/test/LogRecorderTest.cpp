#include <das/Core/Logger/DasProcessLogRecorderImpl.h>
#include <das/Core/Logger/LogRecorderRuntime.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
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
        auto path = std::filesystem::current_path() / "logger-test-output"
                    / (std::string{test_name} + "-" + std::to_string(stamp));
        std::filesystem::remove_all(path);
        return path;
    }

    class LogRecorderFixture : public ::testing::Test
    {
    protected:
        void TearDown() override
        {
            Das::Core::LogRecorderRuntime::Shutdown();
            Das::Core::LogRecorderRuntime::ResetForTest();
        }
    };
} // namespace

TEST_F(LogRecorderFixture, LogRecorderTest_RecordWritesJsonlEntry)
{
    const auto dir = UniqueTempDir("RecordWritesJsonl");
    Das::Core::LogRecorderRuntime::ResetForTest();
    ASSERT_EQ(Das::Core::LogRecorderRuntime::Initialize(dir), DAS_S_OK);

    Das::DasPtr<Das::ExportInterface::IDasProcessLogRecorder> recorder;
    ASSERT_EQ(CreateIDasProcessLogRecorder(recorder.Put()), DAS_S_OK);

    auto entry = Das::DasPtr<Das::ExportInterface::IDasLogEntry>{
        new Das::Core::IDasLogEntryImpl{}};
    DasU8StringOnStack text{"hello"};
    ASSERT_EQ(
        entry->SetType(Das::ExportInterface::DAS_LOG_TYPE_INFO),
        DAS_S_OK);
    ASSERT_EQ(entry->AddText(&text), DAS_S_OK);
    EXPECT_EQ(recorder->Record(entry.Get()), DAS_S_OK);

    const auto jsonl_path = Das::Core::LogRecorderRuntime::UserJsonlPath();
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
    Das::Core::LogRecorderRuntime::ResetForTest();
    ASSERT_EQ(Das::Core::LogRecorderRuntime::Initialize(dir), DAS_S_OK);

    Das::DasPtr<Das::ExportInterface::IDasProcessLogRecorder> recorder;
    ASSERT_EQ(CreateIDasProcessLogRecorder(recorder.Put()), DAS_S_OK);

    DasU8StringOnStack first_text{"first"};
    auto first_entry = Das::DasPtr<Das::ExportInterface::IDasLogEntry>{
        new Das::Core::IDasLogEntryImpl{}};
    ASSERT_EQ(
        first_entry->SetType(Das::ExportInterface::DAS_LOG_TYPE_INFO),
        DAS_S_OK);
    ASSERT_EQ(first_entry->AddText(&first_text), DAS_S_OK);
    ASSERT_EQ(recorder->Record(first_entry.Get()), DAS_S_OK);

    DasU8StringOnStack second_text{"second"};
    auto second_entry = Das::DasPtr<Das::ExportInterface::IDasLogEntry>{
        new Das::Core::IDasLogEntryImpl{}};
    ASSERT_EQ(
        second_entry->SetType(Das::ExportInterface::DAS_LOG_TYPE_SUCCESS),
        DAS_S_OK);
    ASSERT_EQ(second_entry->AddText(&second_text), DAS_S_OK);
    ASSERT_EQ(recorder->Record(second_entry.Get()), DAS_S_OK);

    const auto    jsonl_path = Das::Core::LogRecorderRuntime::UserJsonlPath();
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

TEST_F(LogRecorderFixture, LogRecorderTest_RecordWithoutInitFails)
{
    Das::Core::LogRecorderRuntime::ResetForTest();

    Das::DasPtr<Das::ExportInterface::IDasProcessLogRecorder> recorder;
    ASSERT_EQ(CreateIDasProcessLogRecorder(recorder.Put()), DAS_S_OK);

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
