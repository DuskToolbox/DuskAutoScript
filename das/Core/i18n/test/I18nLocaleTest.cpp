#include <das/Core/i18n/IDasI18nImpl.h>
#include <das/Core/i18n/i18n.hpp>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    // 写 UTF-8 字节内容到文件（std::ofstream 经 std::filesystem::path 打开）。
    void WriteFile(const std::filesystem::path& path, std::string_view content)
    {
        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(ofs.is_open()) << "failed to open " << path.string();
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        ofs.flush();
        ASSERT_TRUE(ofs.good()) << "write failed " << path.string();
    }

    // 经 C API 工厂构造只读 IDasReadOnlyString（AddRef 后移交 DasPtr）。
    DAS::DasPtr<IDasReadOnlyString> MakeLocaleName(const char* utf8)
    {
        DAS::DasPtr<IDasReadOnlyString> p;
        ::CreateIDasReadOnlyStringFromUtf8(utf8, p.Put());
        return p;
    }

    // 把进程级 GlobalLocale 切到指定 locale（Translate 默认路径读它）。
    void SetGlobalLocale(const char* utf8)
    {
        auto locale = MakeLocaleName(utf8);
        ASSERT_NE(locale.Get(), nullptr);
        ASSERT_EQ(::DasSetDefaultLocale(locale.Get()), DAS_S_OK);
    }

    // 从 IDasReadOnlyString 取 UTF-8 std::string。
    std::string ToUtf8(IDasReadOnlyString* p)
    {
        const char* out = nullptr;
        EXPECT_EQ(p->GetUtf8(&out), DAS_S_OK);
        return out ? std::string(out) : std::string{};
    }

    // 本测试套件的临时 JSON 夹具：{cwd}/das_i18n_test_fixtures/{en,zh-CN}.json
    class I18nLocaleTest : public ::testing::Test
    {
    protected:
        inline static const std::filesystem::path kFixtureDir =
            std::filesystem::current_path() / "das_i18n_test_fixtures";

        void SetUp() override
        {
            std::error_code ec;
            std::filesystem::remove_all(kFixtureDir, ec);
            std::filesystem::create_directories(kFixtureDir, ec);
            ASSERT_FALSE(ec) << "create_directories failed: " << ec.message();

            // en.json — base locale，含占位符模板
            WriteFile(
                kFixtureDir / "en.json",
                R"({"progress.downloading": "Downloading...",
)" // placeholder line preserved below
                R"(    "progress.downloading_with_size": "Downloading ({1} / {2}){3}%",
    "label.confirm": "Confirm"})");

            // zh-CN.json — 非基础 locale
            WriteFile(
                kFixtureDir / "zh-CN.json",
                R"({"progress.downloading": "正在下载…",
    "label.confirm": "确认"})");
        }

        void TearDown() override
        {
            std::error_code ec;
            std::filesystem::remove_all(kFixtureDir, ec);
        }

        // 经工厂装载夹具目录，返回只读 IDasI18n 视图。
        DAS::DasPtr<Das::ExportInterface::IDasI18n> LoadFromFixtureDir()
        {
            const auto dir_str = kFixtureDir.string();
            DAS::DasPtr<Das::ExportInterface::IDasI18n> p_i18n;
            DAS::DasPtr<IDasReadOnlyString>             p_dir;
            ::CreateIDasReadOnlyStringFromUtf8(dir_str.c_str(), p_dir.Put());
            EXPECT_EQ(
                ::CreateIDasI18nFromDirectory(p_dir.Get(), p_i18n.Put()),
                DAS_S_OK);
            return p_i18n;
        }
    };
} // namespace

TEST_F(I18nLocaleTest, TranslateHitsCurrentGlobalLocale)
{
    auto i18n = LoadFromFixtureDir();
    ASSERT_NE(i18n.Get(), nullptr);
    SetGlobalLocale("zh-CN");

    auto key = MakeLocaleName("progress.downloading");
    DAS::DasPtr<IDasReadOnlyString> p_text;
    ASSERT_EQ(i18n->Translate(key.Get(), p_text.Put()), DAS_S_OK);
    ASSERT_NE(p_text.Get(), nullptr);
    EXPECT_EQ(
        ToUtf8(p_text.Get()),
        "\xE6\xAD\xA3\xE5\x9C\xA8\xE4\xB8\x8B\xE8\xBD\xBD\xE2\x80\xA6");
}

TEST_F(I18nLocaleTest, TranslatePassesThroughPositionalPlaceholders)
{
    auto i18n = LoadFromFixtureDir();
    ASSERT_NE(i18n.Get(), nullptr);
    SetGlobalLocale("en");

    auto key = MakeLocaleName("progress.downloading_with_size");
    DAS::DasPtr<IDasReadOnlyString> p_text;
    ASSERT_EQ(i18n->Translate(key.Get(), p_text.Put()), DAS_S_OK);
    ASSERT_NE(p_text.Get(), nullptr);
    const auto text = ToUtf8(p_text.Get());
    EXPECT_NE(text.find("{1}"), std::string::npos);
    EXPECT_NE(text.find("{2}"), std::string::npos);
    EXPECT_NE(text.find("{3}"), std::string::npos);
}

TEST_F(I18nLocaleTest, TranslateWithLocaleReturnsExplicitLocale)
{
    auto i18n = LoadFromFixtureDir();
    ASSERT_NE(i18n.Get(), nullptr);
    SetGlobalLocale("zh-CN"); // 全局是 zh-CN，但显式请求 en

    auto                            locale = MakeLocaleName("en");
    auto                            key = MakeLocaleName("label.confirm");
    DAS::DasPtr<IDasReadOnlyString> p_text;
    ASSERT_EQ(
        i18n->TranslateWithLocale(locale.Get(), key.Get(), p_text.Put()),
        DAS_S_OK);
    ASSERT_NE(p_text.Get(), nullptr);
    EXPECT_EQ(ToUtf8(p_text.Get()), "Confirm");
}

TEST_F(I18nLocaleTest, TranslateWithLocaleFallsBackToBaseWhenLocaleMissing)
{
    auto i18n = LoadFromFixtureDir();
    ASSERT_NE(i18n.Get(), nullptr);
    // ja 不在表中，应回退 en base 命中 label.confirm
    auto                            locale = MakeLocaleName("ja");
    auto                            key = MakeLocaleName("label.confirm");
    DAS::DasPtr<IDasReadOnlyString> p_text;
    ASSERT_EQ(
        i18n->TranslateWithLocale(locale.Get(), key.Get(), p_text.Put()),
        DAS_S_OK);
    ASSERT_NE(p_text.Get(), nullptr);
    EXPECT_EQ(ToUtf8(p_text.Get()), "Confirm");
}

TEST_F(I18nLocaleTest, TranslateMissEverywhereReturnsOutOfRange)
{
    auto i18n = LoadFromFixtureDir();
    ASSERT_NE(i18n.Get(), nullptr);
    SetGlobalLocale("en");

    auto                            key = MakeLocaleName("nonexistent.key");
    DAS::DasPtr<IDasReadOnlyString> p_text;
    EXPECT_EQ(i18n->Translate(key.Get(), p_text.Put()), DAS_E_OUT_OF_RANGE);
    EXPECT_EQ(p_text.Get(), nullptr);
}

TEST_F(I18nLocaleTest, GetAvailableLocalesReturnsAllLoadedLocales)
{
    auto i18n = LoadFromFixtureDir();
    ASSERT_NE(i18n.Get(), nullptr);

    DAS::DasPtr<Das::ExportInterface::IDasReadOnlyStringVector> p_locales;
    ASSERT_EQ(i18n->GetAvailableLocales(p_locales.Put()), DAS_S_OK);
    ASSERT_NE(p_locales.Get(), nullptr);

    uint32_t count = 0;
    ASSERT_EQ(p_locales->GetCount(&count), DAS_S_OK);
    ASSERT_EQ(count, 2u);

    std::vector<std::string> names;
    for (uint32_t i = 0; i < count; ++i)
    {
        DAS::DasPtr<IDasReadOnlyString> p_name;
        ASSERT_EQ(p_locales->GetAt(i, p_name.Put()), DAS_S_OK);
        names.push_back(ToUtf8(p_name.Get()));
    }
    EXPECT_NE(std::find(names.begin(), names.end(), "en"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "zh-CN"), names.end());
}

TEST_F(I18nLocaleTest, GetKeysReturnsLocaleKeys)
{
    auto i18n = LoadFromFixtureDir();
    ASSERT_NE(i18n.Get(), nullptr);

    auto locale = MakeLocaleName("en");
    DAS::DasPtr<Das::ExportInterface::IDasReadOnlyStringVector> p_keys;
    ASSERT_EQ(i18n->GetKeys(locale.Get(), p_keys.Put()), DAS_S_OK);
    ASSERT_NE(p_keys.Get(), nullptr);

    uint32_t count = 0;
    ASSERT_EQ(p_keys->GetCount(&count), DAS_S_OK);
    ASSERT_EQ(count, 3u);

    std::vector<std::string> keys;
    for (uint32_t i = 0; i < count; ++i)
    {
        DAS::DasPtr<IDasReadOnlyString> p_key;
        ASSERT_EQ(p_keys->GetAt(i, p_key.Put()), DAS_S_OK);
        keys.push_back(ToUtf8(p_key.Get()));
    }
    EXPECT_NE(
        std::find(keys.begin(), keys.end(), "progress.downloading"),
        keys.end());
    EXPECT_NE(
        std::find(keys.begin(), keys.end(), "progress.downloading_with_size"),
        keys.end());
    EXPECT_NE(std::find(keys.begin(), keys.end(), "label.confirm"), keys.end());
}
