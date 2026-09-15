#include "../src/MaaCallbackLogger.h"
#include "../src/PiHttpClient.h"

#include <das/Core/Logger/DasLogNotifierSink.h>
#include <das/Core/Logger/DasLogSessionImpl.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/PathText.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
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
                    / DAS_PATH_LITERAL("maapi-callback-test-output")
                    / Das::Utils::PathFromUtf8(
                        std::string{test_name} + "-" + std::to_string(stamp));
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
            const auto directory_str = Das::Utils::PathToUtf8(out_dir);
            Das::DasPtr<IDasReadOnlyString> directory_param;
            EXPECT_EQ(
                CreateIDasReadOnlyStringFromUtf8(
                    directory_str.c_str(),
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

    // 最小 PiHttpClient 测试桩：Get 返回预设 body+status；Head/DownloadStream
    // 不用于 focus content 解析，返 DAS_E_NO_IMPLEMENTATION。
    class FakeFocusHttpClient final : public PiHttpClient
    {
    public:
        FakeFocusHttpClient(
            std::string next_body,
            int         next_status,
            DasResult   next_result = DAS_S_OK)
            : next_body_(std::move(next_body)), next_status_(next_status),
              next_result_(next_result)
        {
        }

        DasResult Get(
            const std::string& /*url*/,
            std::string& out_body,
            int&         out_status,
            const std::string& /*proxy_url*/ = {}) override
        {
            ++get_call_count;
            out_body = next_body_;
            out_status = next_status_;
            return next_result_;
        }

        DasResult Head(
            const std::string& /*url*/,
            int& /*out_status*/,
            const std::string& /*proxy_url*/ = {}) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult DownloadStream(
            const std::string& /*url*/,
            const std::filesystem::path& /*out_path*/,
            const ProgressCallback& /*on_progress*/,
            int& /*out_status*/,
            const std::string& /*proxy_url*/ = {}) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        int get_call_count = 0;

    private:
        std::string next_body_;
        int         next_status_;
        DasResult   next_result_;
    };

    void WriteFile(const std::filesystem::path& path, std::string content)
    {
        std::ofstream out{path};
        out << content;
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

// ResolveFocusContent i18n `$key` 分支：命中 translations 时返回翻译值。
TEST_F(MaaCallbackLoggerFixture, ResolveFocusContent_ResolvesI18nDollarKey)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    context.translations["greeting"] = "Hello";
    EXPECT_EQ(ResolveFocusContent("$greeting", context), "Hello");
}

// ResolveFocusContent i18n miss：translations 不含 key 时返回去 `$` 前缀的
// key 文本（与 ResolveI18n 既有 fallback 语义一致）。
TEST_F(
    MaaCallbackLoggerFixture,
    ResolveFocusContent_I18nMissReturnsKeyWithoutDollar)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    EXPECT_EQ(ResolveFocusContent("$missing", context), "missing");
}

// ResolveFocusContent literal 分支：未匹配任何源（无 `$` 前缀）→ 原文返回。
TEST_F(MaaCallbackLoggerFixture, ResolveFocusContent_LiteralTextPassesThrough)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    EXPECT_EQ(ResolveFocusContent("plain text", context), "plain text");
}

// LogFocusMessage 调用序：先 ResolveFocusContent 解析 `$key` 模板，
// 再 ReplaceFocusPlaceholders 替换 `{name}` 等占位符。验证两步组合后
// user.jsonl 文本含解析+占位符替换后的最终结果。
TEST_F(
    MaaCallbackLoggerFixture,
    LogFocusMessage_ResolvesContentBeforePlaceholders)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    context.translations["greeting"] = "Hello {name}";
    auto details = FocusDetails(R"("Node.Action.Starting":"$greeting")");

    LogFocusMessage(
        context,
        "Node.Action.Starting",
        SerializeDetails(details).c_str());

    auto obj = ReadLastEntry().as_object();
    ASSERT_TRUE(obj.has_value());
    auto text = (*obj)[std::string_view("text")].as_string();
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, "Hello NodeA");
}

// ResolveFocusContent URL 分支：http(s):// + http_client 注入时，
// 调 Get 拉取响应体。成功 + status=200 → 返回 body。
TEST_F(MaaCallbackLoggerFixture, ResolveFocusContent_FetchesUrlReturnsBody)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    FakeFocusHttpClient   http{"from server", 200};
    context.http_client = &http;
    EXPECT_EQ(
        ResolveFocusContent("http://example.com/note", context),
        "from server");
    EXPECT_EQ(http.get_call_count, 1);
}

// URL 加载失败（Get 返失败或 status != 200）→ 降级字面 URL 文本，不阻塞回调。
TEST_F(
    MaaCallbackLoggerFixture,
    ResolveFocusContent_UrlFailureReturnsLiteralText)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    // Get 返 DAS_E_FAIL 模拟网络失败。
    FakeFocusHttpClient failed_http{"", 0, DAS_E_FAIL};
    context.http_client = &failed_http;
    EXPECT_EQ(
        ResolveFocusContent("https://unreachable.example.com", context),
        "https://unreachable.example.com");
}

// https:// 前缀同走 URL 分支（http + https 都触发 GET）。
TEST_F(MaaCallbackLoggerFixture, ResolveFocusContent_HttpsPrefixAlsoFetched)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    FakeFocusHttpClient   http{"secure body", 200};
    context.http_client = &http;
    EXPECT_EQ(
        ResolveFocusContent("https://example.com/secure", context),
        "secure body");
}

// URL 响应体上限：body 超 1 MiB（MAX_FOCUS_CONTENT_BYTES）截断至首 1 MiB，
// SSRF 内存耗尽防御（恶意 PI 资源指向流式 GB 端点）。
TEST_F(MaaCallbackLoggerFixture, ResolveFocusContent_UrlBodyTruncatedAtOneMib)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    constexpr std::size_t one_mib = 1U << 20;
    std::string           huge_body(2 * one_mib, 'A');
    FakeFocusHttpClient   http{std::move(huge_body), 200};
    context.http_client = &http;
    const auto result = ResolveFocusContent("http://example.com/big", context);
    EXPECT_EQ(result.size(), one_mib);
    EXPECT_TRUE(
        std::all_of(
            result.begin(),
            result.end(),
            [](char ch) { return ch == 'A'; }));
}

// file 分支：./ 前缀的相对路径，从 context.interface_directory 拼接读文件。
TEST_F(MaaCallbackLoggerFixture, ResolveFocusContent_LoadsFileFromInterfaceDir)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    context.interface_directory = UniqueTempDir("FileBranch");
    std::filesystem::create_directories(context.interface_directory);
    WriteFile(
        context.interface_directory / DAS_PATH_LITERAL("intro.md"),
        "intro body");
    EXPECT_EQ(ResolveFocusContent("./intro.md", context), "intro body");
}

// file 分支降级：文件不存在 → 返回原文字面（不阻塞）。
TEST_F(
    MaaCallbackLoggerFixture,
    ResolveFocusContent_FileMissingReturnsLiteralText)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    context.interface_directory = UniqueTempDir("FileMissing");
    std::filesystem::create_directories(context.interface_directory);
    EXPECT_EQ(ResolveFocusContent("./missing.md", context), "./missing.md");
}

// file 分支扩展名启发式：无 ./ 前缀但含 .txt 扩展名 → 视为文件路径。
TEST_F(
    MaaCallbackLoggerFixture,
    ResolveFocusContent_DocExtensionAlsoTreatedAsFile)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    context.interface_directory = UniqueTempDir("DocExt");
    std::filesystem::create_directories(context.interface_directory);
    WriteFile(
        context.interface_directory / DAS_PATH_LITERAL("notes.txt"),
        "notes content");
    EXPECT_EQ(ResolveFocusContent("notes.txt", context), "notes content");
}

// 绝对路径越界：content 是 interface_directory 之外的绝对路径 .md 文件时，
// std::filesystem::path::operator/ 对绝对 RHS 丢弃 LHS 导致读取任意文件。
// 范围守卫必须拒绝越界路径，降级字面文本。
TEST_F(
    MaaCallbackLoggerFixture,
    ResolveFocusContent_AbsolutePathOutsideInterfaceDirReturnsLiteral)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    context.interface_directory = UniqueTempDir("AbsGuardInside");
    std::filesystem::create_directories(context.interface_directory);
    // 在 interface_directory 外的独立临时目录建靶文件（绝对路径含 .md）。
    const auto external_dir = UniqueTempDir("AbsGuardOutside");
    std::filesystem::create_directories(external_dir);
    WriteFile(external_dir / DAS_PATH_LITERAL("secret.md"), "secret data");
    const std::string content =
        Das::Utils::PathToUtf8(external_dir / DAS_PATH_LITERAL("secret.md"));
    // 期望降级字面文本（不读外部文件），实现缺陷下会读到 "secret data"。
    EXPECT_EQ(ResolveFocusContent(content, context), content);
}

// 父遍历越界：content = "../escape.md" 经 interface_directory/../ 解析后
// 落到 interface_directory 父目录之外。范围守卫必须拒绝，降级字面文本。
TEST_F(
    MaaCallbackLoggerFixture,
    ResolveFocusContent_ParentTraversalOutsideDirReturnsLiteral)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    context.interface_directory = UniqueTempDir("TraversalInside");
    std::filesystem::create_directories(context.interface_directory);
    // 在父目录建靶文件（../ 从 interface_directory 跳出到父目录命中它）。
    const auto parent = context.interface_directory.parent_path();
    WriteFile(parent / DAS_PATH_LITERAL("escape.md"), "escaped content");
    // 期望降级字面文本（不读父目录文件），实现缺陷下会读到 "escaped content"。
    EXPECT_EQ(ResolveFocusContent("../escape.md", context), "../escape.md");
}

// i18n 空翻译值 fallback：translations[key] 是空串（falsy）时按 MXU
// `translations?.[key] || key` 语义 fallback 到 key（去 $ 前缀）。
TEST_F(
    MaaCallbackLoggerFixture,
    ResolveFocusContent_EmptyTranslationFallsBackToKey)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    context.translations["empty_key"] = "";
    // 空翻译值应 fallback 到 "empty_key"，实现缺陷下返回空串。
    EXPECT_EQ(ResolveFocusContent("$empty_key", context), "empty_key");
}

// RecordEntryOrLog：recorder 为空时走 fallback 路径，返回
// DAS_E_OBJECT_NOT_INIT（不吞失败），并按 fallback_level 映射打 DAS_LOG_*
// （[focus-degraded] 前缀落 logs/DasCore.log，不在单测断言范围）。
TEST_F(
    MaaCallbackLoggerFixture,
    RecordEntryOrLog_FallsBackToLogWhenRecorderNull)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    Das::DasPtr<Das::ExportInterface::IDasLogEntry> entry;
    ASSERT_EQ(CreateIDasLogEntry(entry.Put()), DAS_S_OK);
    DasU8StringOnStack text{"hello"};
    ASSERT_EQ(entry->AddText(&text), DAS_S_OK);
    const auto result = RecordEntryOrLog(
        nullptr,
        entry.Get(),
        Das::ExportInterface::DAS_LOG_TYPE_STRUCTURAL,
        "hello");
    EXPECT_EQ(result, DAS_E_OBJECT_NOT_INIT);
}

// RecordEntryOrLog：recorder 非空且 Record 成功时透明转发返回 S_OK，
// 不调 fallback（成功路径零介入）。
TEST_F(MaaCallbackLoggerFixture, RecordEntryOrLog_PassesThroughOnSuccess)
{
    std::filesystem::path dir;
    auto                  context = MakeRecorderContext(dir);
    Das::DasPtr<Das::ExportInterface::IDasLogEntry> entry;
    ASSERT_EQ(CreateIDasLogEntry(entry.Put()), DAS_S_OK);
    DasU8StringOnStack text{"ok entry"};
    ASSERT_EQ(entry->AddText(&text), DAS_S_OK);
    const auto result = RecordEntryOrLog(
        context.recorder.Get(),
        entry.Get(),
        Das::ExportInterface::DAS_LOG_TYPE_STRUCTURAL,
        "ok entry");
    EXPECT_EQ(result, DAS_S_OK);
    // 验证 user.jsonl 写入了 entry（证明走 Record 成功路径，非 fallback）。
    auto obj = ReadLastEntry().as_object();
    ASSERT_TRUE(obj.has_value());
    auto text_field = (*obj)[std::string_view("text")].as_string();
    ASSERT_TRUE(text_field.has_value());
    EXPECT_EQ(*text_field, "ok entry");
}
