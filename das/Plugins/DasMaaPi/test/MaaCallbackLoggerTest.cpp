#include "../src/MaaCallbackLogger.h"

#include <das/Core/Logger/DasLogNotifierSink.h>
#include <das/Core/Logger/DasLogSessionImpl.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using namespace Das;
    using namespace Das::Plugins::DasMaaPi;

    std::filesystem::path UniqueTempDir(const char* test_name)
    {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        auto path = std::filesystem::current_path()
                    / "maapi-callback-test-output"
                    / (std::string{test_name} + "-" + std::to_string(stamp));
        std::filesystem::remove_all(path);
        return path;
    }

    class MaaCallbackLoggerFixture : public ::testing::Test
    {
    protected:
        Das::DasPtr<Das::ExportInterface::IDasLogNotifier> notifier_;
        Das::DasPtr<Das::ExportInterface::IDasLogSession>  session_;

        void SetUp() override { GetDasLogNotifier(notifier_.Put()); }

        // 测试用 recorder context 工厂：创建独立 session + recorder 注入到
        // fixture 持有的 notifier_/session_。每用例独立目录，RAII 自动清理。
        MaaCallbackLoggerContext MakeRecorderContext(
            std::filesystem::path& out_dir,
            FocusImageProvider     image_provider = {})
        {
            out_dir = UniqueTempDir("CallbackLogger");
            const auto                      directory_str = out_dir.u8string();
            Das::DasPtr<IDasReadOnlyString> directory_param;
            EXPECT_EQ(
                CreateIDasReadOnlyStringFromUtf8(
                    reinterpret_cast<const char*>(directory_str.c_str()),
                    directory_param.Put()),
                DAS_S_OK);
            EXPECT_EQ(
                CreateLogSession(directory_param.Get(), session_.Put()),
                DAS_S_OK);

            MaaCallbackLoggerContext context;
            EXPECT_EQ(
                CreateIDasProcessLogRecorder(
                    notifier_.Get(),
                    session_.Get(),
                    context.recorder.Put()),
                DAS_S_OK);
            context.image_provider = std::move(image_provider);
            return context;
        }

        std::filesystem::path ReadUserJsonlPath()
        {
            if (!session_)
            {
                return {};
            }
            Das::DasPtr<IDasReadOnlyString> path_str;
            if (DAS::IsFailed(session_->GetUserJsonlPath(path_str.Put()))
                || !path_str)
            {
                return {};
            }
            const char* utf8 = nullptr;
            if (DAS::IsFailed(path_str->GetUtf8(&utf8)) || utf8 == nullptr)
            {
                return {};
            }
            return std::filesystem::path{
                reinterpret_cast<const char8_t*>(utf8)};
        }

        // 读 user.jsonl 最后一行并解析为 JSON 对象。
        yyjson::value ReadLastEntry()
        {
            const auto jsonl_path = ReadUserJsonlPath();
            EXPECT_FALSE(jsonl_path.empty());
            EXPECT_TRUE(std::filesystem::exists(jsonl_path));
            std::ifstream input{jsonl_path};
            std::string   line;
            std::string   last;
            while (std::getline(input, line))
            {
                last = line;
            }
            auto parsed = Das::Utils::ParseYyjsonFromString(last);
            EXPECT_TRUE(parsed.has_value());
            return parsed ? std::move(*parsed) : yyjson::value{};
        }
    };

    yyjson::value FocusDetails(const std::string& message_value)
    {
        const std::string json = R"({"task_id":12345,"name":"NodeA","focus":{)"
                                 + message_value + R"(}})";
        auto              parsed = Das::Utils::ParseYyjsonFromString(json);
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : yyjson::value{};
    }

    Das::DasPtr<Das::ExportInterface::IDasImage> MakeTestImage()
    {
        constexpr int32_t width = 8;
        constexpr int32_t height = 8;
        std::vector<char> pixels(
            static_cast<size_t>(width) * height * 3,
            static_cast<char>(0x80));
        DasImageDesc desc{
            pixels.data(),
            pixels.size(),
            Das::ExportInterface::DAS_IMAGE_FORMAT_RGB_888};
        Das::ExportInterface::DasSize                size{width, height};
        Das::DasPtr<Das::ExportInterface::IDasImage> image;
        EXPECT_EQ(
            CreateIDasImageFromDecodedData(&desc, &size, image.Put()),
            DAS_S_OK);
        return image;
    }

    std::string SerializeDetails(const yyjson::value& details)
    {
        return Das::Utils::SerializeYyjsonValue(details).value_or("{}");
    }
} // namespace

TEST_F(MaaCallbackLoggerFixture, ParseFocusTemplate_StringShorthand)
{
    auto details = FocusDetails(R"("Node.Action.Starting":"{name} 开始执行")");
    auto result = ParseFocusTemplate(details, "Node.Action.Starting");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->content, "{name} 开始执行");
    ASSERT_EQ(result->channels.size(), 1u);
    EXPECT_EQ(result->channels[0], "log");
}

TEST_F(MaaCallbackLoggerFixture, ParseFocusTemplate_ObjectWithDisplayString)
{
    auto details = FocusDetails(
        R"("Node.Action.Starting":{"content":"{name} 开始执行","display":"toast"})");
    auto result = ParseFocusTemplate(details, "Node.Action.Starting");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->content, "{name} 开始执行");
    ASSERT_EQ(result->channels.size(), 1u);
    EXPECT_EQ(result->channels[0], "toast");
}

TEST_F(MaaCallbackLoggerFixture, ParseFocusTemplate_ObjectWithDisplayArray)
{
    auto details = FocusDetails(
        R"("Node.Action.Succeeded":{"content":"✅ {name} 执行成功","display":["log","toast"]})");
    auto result = ParseFocusTemplate(details, "Node.Action.Succeeded");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->content, "✅ {name} 执行成功");
    ASSERT_EQ(result->channels.size(), 2u);
    EXPECT_EQ(result->channels[0], "log");
    EXPECT_EQ(result->channels[1], "toast");
}

TEST_F(MaaCallbackLoggerFixture, ParseFocusTemplate_NoFocusReturnsNull)
{
    auto parsed = Das::Utils::ParseYyjsonFromString(R"({"task_id":1})");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(
        ParseFocusTemplate(*parsed, "Node.Action.Starting").has_value());
}

TEST_F(MaaCallbackLoggerFixture, ParseFocusTemplate_MessageNotInFocus)
{
    auto details = FocusDetails(R"("Node.Action.Starting":"开始")");
    EXPECT_FALSE(ParseFocusTemplate(details, "Node.Action.Failed").has_value());
}

TEST_F(
    MaaCallbackLoggerFixture,
    ReplaceFocusPlaceholders_ReplacesFieldsKeepsImage)
{
    auto details = FocusDetails(R"("k":"{name} {task_id} {image}")");
    const std::string result =
        ReplaceFocusPlaceholders("{name} 任务 {task_id} {image}", details);
    // {name}/{task_id} 从 details 顶层取 String 值，{image}
    // 保留由截图通路处理。
    EXPECT_EQ(result, "NodeA 任务 12345 {image}");
}

TEST_F(MaaCallbackLoggerFixture, LogFocusMessage_WritesFocusEntryToJsonl)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    auto details = FocusDetails(R"("Node.Action.Starting":"{name} 开始执行")");

    LogFocusMessage(
        context,
        "Node.Action.Starting",
        SerializeDetails(details).c_str());

    auto obj = ReadLastEntry().as_object();
    ASSERT_TRUE(obj.has_value());
    auto type = (*obj)[std::string_view("type")].as_string();
    ASSERT_TRUE(type.has_value());
    EXPECT_EQ(*type, "structural");
    auto text = (*obj)[std::string_view("text")].as_string();
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, "NodeA 开始执行");
    auto fields = (*obj)[std::string_view("fields")].as_object();
    ASSERT_TRUE(fields.has_value());
    auto channels = (*fields)[std::string_view("displayChannels")].as_array();
    ASSERT_TRUE(channels.has_value());
    ASSERT_EQ(channels->size(), 1u);
    auto channel = (*channels)[0].as_string();
    ASSERT_TRUE(channel.has_value());
    EXPECT_EQ(*channel, "log");
}

TEST_F(MaaCallbackLoggerFixture, LogFocusMessage_ObjectDisplayArrayChannels)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    auto                  details = FocusDetails(
        R"("Node.Action.Succeeded":{"content":"成功","display":["log","toast"]})");

    LogFocusMessage(
        context,
        "Node.Action.Succeeded",
        SerializeDetails(details).c_str());

    auto obj = ReadLastEntry().as_object();
    ASSERT_TRUE(obj.has_value());
    auto fields = (*obj)[std::string_view("fields")].as_object();
    ASSERT_TRUE(fields.has_value());
    auto channels = (*fields)[std::string_view("displayChannels")].as_array();
    ASSERT_TRUE(channels.has_value());
    ASSERT_EQ(channels->size(), 2u);
    auto first = (*channels)[0].as_string();
    auto second = (*channels)[1].as_string();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, "log");
    EXPECT_EQ(*second, "toast");
}

TEST_F(
    MaaCallbackLoggerFixture,
    LogFocusMessage_ImagePlaceholderWithoutProvider)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    auto details = FocusDetails(R"("Node.Action.Starting":"{name} {image}")");

    LogFocusMessage(
        context,
        "Node.Action.Starting",
        SerializeDetails(details).c_str());

    auto obj = ReadLastEntry().as_object();
    ASSERT_TRUE(obj.has_value());
    // 无截图 seam → imageRefs 留空（Pitfall 4），文本不含字面 {image}。
    auto text = (*obj)[std::string_view("text")].as_string();
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, "NodeA ");
    auto image_refs = (*obj)[std::string_view("imageRefs")].as_array();
    ASSERT_TRUE(image_refs.has_value());
    EXPECT_TRUE(image_refs->empty());
}

TEST_F(MaaCallbackLoggerFixture, LogFocusMessage_ImagePlaceholderWithProvider)
{
    std::filesystem::path dir;
    auto context = MakeRecorderContext(dir, [] { return MakeTestImage(); });
    auto details = FocusDetails(R"("Node.Action.Starting":"{name} {image}")");

    LogFocusMessage(
        context,
        "Node.Action.Starting",
        SerializeDetails(details).c_str());

    auto obj = ReadLastEntry().as_object();
    ASSERT_TRUE(obj.has_value());
    auto image_refs = (*obj)[std::string_view("imageRefs")].as_array();
    ASSERT_TRUE(image_refs.has_value());
    ASSERT_EQ(image_refs->size(), 1u);
    auto ref = (*image_refs)[0].as_string();
    ASSERT_TRUE(ref.has_value());
    EXPECT_EQ(*ref, "img/0001.jpg");
}

TEST_F(MaaCallbackLoggerFixture, LogFocusMessage_NonFocusMessageIgnored)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    auto details = FocusDetails(R"("Node.Action.Starting":"开始")");

    LogFocusMessage(
        context,
        "Tasker.Task.Starting",
        SerializeDetails(details).c_str());

    const auto jsonl_path = ReadUserJsonlPath();
    EXPECT_FALSE(std::filesystem::exists(jsonl_path));
}

TEST_F(MaaCallbackLoggerFixture, MaaCallbackLoggerEntry_ForwardsToRecorder)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    auto details = FocusDetails(R"("Node.Action.Failed":"{name} 执行失败")");
    const std::string details_json =
        Das::Utils::SerializeYyjsonValue(details).value_or("{}");

    MaaCallbackLoggerEntry(
        nullptr,
        "Node.Action.Failed",
        details_json.c_str(),
        &context);

    auto obj = ReadLastEntry().as_object();
    ASSERT_TRUE(obj.has_value());
    auto text = (*obj)[std::string_view("text")].as_string();
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, "NodeA 执行失败");
}
