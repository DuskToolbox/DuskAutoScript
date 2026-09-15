#include "../src/DasProgressReporter.h"
#include "../src/PiUpdateService.h"

#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/PathText.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProgressService.Implements.hpp>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using namespace Das;
    using namespace Das::Plugins::DasMaaPi;

    // 测试缝：本地 IDasProgressService，记录 Report 收到的 entry（无需真
    // IPC）。 镜像 MaapiProgressReporterTest 的 FakeProgressService 范式。
    class FakeProgressService final
        : public Das::ExportInterface::DasProgressServiceImplBase<
              FakeProgressService>
    {
    public:
        std::vector<DasPtr<Das::ExportInterface::IDasProgressUpdate>> entries;

        DasResult AddCallback(
            Das::ExportInterface::IDasProgressCallback* cb) override
        {
            (void)cb;
            return DAS_S_OK;
        }

        DasResult RemoveCallback(
            Das::ExportInterface::IDasProgressCallback* cb) override
        {
            (void)cb;
            return DAS_S_OK;
        }

        DasResult Report(
            Das::ExportInterface::IDasProgressUpdate* entry) override
        {
            entries.emplace_back(entry);
            return DAS_S_OK;
        }
    };

    // 读 entry 第 index 个字段的字符串值（fieldName → value）。
    // IDasJson::ToString 返回 JSON 序列化形式（字符串值带包围双引号
    // "\"text\""），这里剥掉首尾双引号还原裸文本。本测试值均为无内嵌引号
    // 的简单字符串，安全。
    std::string ReadFieldValue(
        Das::ExportInterface::IDasProgressUpdate* entry,
        size_t                                    index)
    {
        DasPtr<IDasReadOnlyString>             name;
        DasPtr<Das::ExportInterface::IDasJson> value;
        EXPECT_EQ(
            entry->GetFieldByIndex(index, name.Put(), value.Put()),
            DAS_S_OK);
        DasPtr<IDasReadOnlyString> text;
        EXPECT_EQ(value->ToString(0, text.Put()), DAS_S_OK);
        const char* raw = nullptr;
        EXPECT_EQ(text->GetUtf8(&raw), DAS_S_OK);
        if (raw == nullptr)
        {
            return {};
        }
        std::string s(raw);
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        {
            return s.substr(1, s.size() - 2);
        }
        return s;
    }

    // 写 UTF-8 字节内容到文件。
    void WriteFile(const std::filesystem::path& path, std::string_view content)
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(ofs.is_open())
            << "failed to open " << Das::Utils::PathToUtf8(path);
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        ofs.flush();
        ASSERT_TRUE(ofs.good())
            << "write failed " << Das::Utils::PathToUtf8(path);
    }

    // 经 C API 工厂构造只读 IDasReadOnlyString。
    DasPtr<IDasReadOnlyString> MakeString(const char* utf8)
    {
        DasPtr<IDasReadOnlyString> p;
        ::CreateIDasReadOnlyStringFromUtf8(utf8, p.Put());
        return p;
    }

    // 把进程级 GlobalLocale 切到指定 locale（Translate 默认路径读它）。
    void SetGlobalLocale(const char* utf8)
    {
        auto locale = MakeString(utf8);
        ASSERT_NE(locale.Get(), nullptr);
        ASSERT_EQ(::DasSetDefaultLocale(locale.Get()), DAS_S_OK);
    }

    // DasMaaPi 进度文本 i18n fixture：每用例独立 i18n 目录（避免 -j8
    // 并行进程共享目录互踩），装载 en base + zh-CN 翻译表。
    class DasMaaPiProgressI18nTest : public ::testing::Test
    {
    protected:
        std::filesystem::path fixture_dir_;

        void SetUp() override
        {
            const auto* info =
                ::testing::UnitTest::GetInstance()->current_test_info();
            const std::string leaf = std::string("das_maapi_i18n_fixtures_")
                                     + info->test_suite_name() + "_"
                                     + info->name();
            fixture_dir_ = std::filesystem::current_path()
                           / Das::Utils::PathFromUtf8(leaf);

            std::error_code ec;
            std::filesystem::remove_all(fixture_dir_, ec);
            std::filesystem::create_directories(fixture_dir_, ec);
            ASSERT_FALSE(ec) << "create_directories failed: " << ec.message();

            WriteFile(
                fixture_dir_ / DAS_PATH_LITERAL("en.json"),
                R"({"progress.extracting": "Extracting…",)"
                R"("progress.replacing": "Replacing…",)"
                R"("progress.downloading": "Downloading…",)"
                R"("progress.downloading_with_size": "Downloading ({0} / {1}){2}%"})");

            WriteFile(
                fixture_dir_ / DAS_PATH_LITERAL("zh-CN.json"),
                R"({"progress.extracting": "解压中…",)"
                R"("progress.replacing": "替换中…",)"
                R"("progress.downloading": "正在下载…",)"
                R"("progress.downloading_with_size": "正在下载（{0} / {1}）{2}%"})");
        }

        void TearDown() override
        {
            // GlobalLocale 是进程级单例：测试改了 locale 必须复位默认 en，
            // 避免污染同进程后续测试（gtest_discover_tests 每用例独立进程，
            // 但稳妥起见仍复位）。
            SetGlobalLocale("en");

            std::error_code ec;
            std::filesystem::remove_all(fixture_dir_, ec);
        }

        // 装载夹具目录翻译表，返回只读 IDasI18n 视图。
        DasPtr<Das::ExportInterface::IDasI18n> LoadI18n()
        {
            const auto dir_utf8 = Das::Utils::PathToUtf8(fixture_dir_);
            DasPtr<IDasReadOnlyString> dir_str;
            ::CreateIDasReadOnlyStringFromUtf8(dir_utf8.c_str(), dir_str.Put());
            DasPtr<Das::ExportInterface::IDasI18n> i18n;
            EXPECT_EQ(
                ::CreateIDasI18nFromDirectory(dir_str.Get(), i18n.Put()),
                DAS_S_OK);
            return i18n;
        }

        // 构造 reporter + 注入 i18n + Start。返回 reporter 与 fake service。
        std::shared_ptr<DasProgressReporter> MakeReporter(
            DasPtr<Das::ExportInterface::IDasProgressService> service,
            DasPtr<Das::ExportInterface::IDasI18n>            i18n)
        {
            auto reporter =
                std::make_shared<DasProgressReporter>("k1", service);
            reporter->SetI18n(std::move(i18n));
            reporter->Start();
            return reporter;
        }

        // 取最近一次 Report 的 updateProgress 字段值。
        std::string LastUpdateProgressText(FakeProgressService* fake)
        {
            EXPECT_FALSE(fake->entries.empty());
            return ReadFieldValue(fake->entries.back().Get(), 0);
        }
    };
} // namespace

// en locale：Extract 阶段返回英文模板。
TEST_F(DasMaaPiProgressI18nTest, ExtractStageReturnsEnglishTemplate)
{
    SetGlobalLocale("en");
    auto  service = FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());
    auto  reporter = MakeReporter(service, LoadI18n());

    UpdateProgress progress{};
    progress.stage = UpdateStage::Extract;
    reporter->Report(progress);

    EXPECT_EQ(LastUpdateProgressText(fake), "Extracting…");
}

// en locale：有 total 的下载阶段，{0}/{1}/{2} 正确填充。
TEST_F(DasMaaPiProgressI18nTest, DownloadWithSizeFillsPlaceholdersEnglish)
{
    SetGlobalLocale("en");
    auto  service = FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());
    auto  reporter = MakeReporter(service, LoadI18n());

    UpdateProgress progress{};
    progress.stage = UpdateStage::Download;
    progress.bytes_received = 1572864; // 1.5 MB
    progress.total_bytes = 10485760;   // 10.0 MB
    reporter->Report(progress);

    // 1572864/10485760 = 0.15 → 15%；FormatBytes 1024 进制 setprecision(1)。
    EXPECT_EQ(
        LastUpdateProgressText(fake),
        "Downloading (1.5 MB / 10.0 MB)15%");
}

// zh-CN locale：Extract 阶段返回中文模板。
TEST_F(DasMaaPiProgressI18nTest, ExtractStageReturnsChineseTemplate)
{
    SetGlobalLocale("zh-CN");
    auto  service = FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());
    auto  reporter = MakeReporter(service, LoadI18n());

    UpdateProgress progress{};
    progress.stage = UpdateStage::Extract;
    reporter->Report(progress);

    EXPECT_EQ(LastUpdateProgressText(fake), "解压中…");
}

// zh-CN locale：有 total 的下载阶段，{0}/{1}/{2} 正确填充（含全角括号）。
TEST_F(DasMaaPiProgressI18nTest, DownloadWithSizeFillsPlaceholdersChinese)
{
    SetGlobalLocale("zh-CN");
    auto  service = FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());
    auto  reporter = MakeReporter(service, LoadI18n());

    UpdateProgress progress{};
    progress.stage = UpdateStage::Download;
    progress.bytes_received = 1572864;
    progress.total_bytes = 10485760;
    reporter->Report(progress);

    EXPECT_EQ(LastUpdateProgressText(fake), "正在下载（1.5 MB / 10.0 MB）15%");
}

// 同 reporter 实例切换 GlobalLocale 后，Translate 即时命中新 locale。
TEST_F(DasMaaPiProgressI18nTest, LocaleSwitchReflectsImmediately)
{
    auto service = FakeProgressService::Make();
    auto fake = static_cast<FakeProgressService*>(service.Get());
    auto reporter = MakeReporter(service, LoadI18n());

    UpdateProgress progress{};
    progress.stage = UpdateStage::Replace;

    SetGlobalLocale("en");
    reporter->Report(progress);
    EXPECT_EQ(LastUpdateProgressText(fake), "Replacing…");

    // 同实例切 zh-CN，不重建 reporter/i18n。
    SetGlobalLocale("zh-CN");
    reporter->Report(progress);
    EXPECT_EQ(LastUpdateProgressText(fake), "替换中…");
}

// 无 total 的下载阶段返回阶段态文本（en / zh-CN 各一）。
TEST_F(DasMaaPiProgressI18nTest, DownloadWithoutTotalReturnsStageText)
{
    auto service = FakeProgressService::Make();
    auto fake = static_cast<FakeProgressService*>(service.Get());
    auto reporter = MakeReporter(service, LoadI18n());

    UpdateProgress progress{};
    progress.stage = UpdateStage::Download;
    progress.total_bytes = std::nullopt;

    SetGlobalLocale("en");
    reporter->Report(progress);
    EXPECT_EQ(LastUpdateProgressText(fake), "Downloading…");

    SetGlobalLocale("zh-CN");
    reporter->Report(progress);
    EXPECT_EQ(LastUpdateProgressText(fake), "正在下载…");
}
