#include <das/Core/i18n/IDasI18nImpl.h>
#include <das/Core/i18n/i18n.hpp>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/PathText.h>
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
        ASSERT_TRUE(ofs.is_open())
            << "failed to open " << Das::Utils::PathToUtf8(path);
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        ofs.flush();
        ASSERT_TRUE(ofs.good())
            << "write failed " << Das::Utils::PathToUtf8(path);
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

    // 本测试套件的临时 JSON 夹具。ctest 经 gtest_discover_tests
    // 把每个用例注册为 独立 ctest 项并以独立进程运行（-j8
    // 并行），故每个用例须用各自的夹具目录， 否则并行进程会在共享目录上
    // remove_all/写入互踩（曾导致 fallback/GetKeys 在 -j8
    // 下随机失败、单独运行却通过）。
    class I18nLocaleTest : public ::testing::Test
    {
    protected:
        std::filesystem::path fixture_dir_;

        void SetUp() override
        {
            const auto* info =
                ::testing::UnitTest::GetInstance()->current_test_info();
            const std::string leaf = std::string("das_i18n_fixtures_")
                                     + info->test_suite_name() + "_"
                                     + info->name();
            fixture_dir_ = std::filesystem::current_path()
                           / Das::Utils::PathFromUtf8(leaf);

            std::error_code ec;
            std::filesystem::remove_all(fixture_dir_, ec);
            std::filesystem::create_directories(fixture_dir_, ec);
            ASSERT_FALSE(ec) << "create_directories failed: " << ec.message();

            // en.json — base locale，含占位符模板
            WriteFile(
                fixture_dir_ / DAS_PATH_LITERAL("en.json"),
                R"({"progress.downloading": "Downloading...",
    "progress.downloading_with_size": "Downloading ({1} / {2}){3}%",
    "label.confirm": "Confirm"})");

            // zh-cn.json — 非基础 locale（文件名小写；装载时 stem 也会
            // toLower）
            WriteFile(
                fixture_dir_ / DAS_PATH_LITERAL("zh-cn.json"),
                R"({"progress.downloading": "正在下载…",
    "label.confirm": "确认"})");
        }

        void TearDown() override
        {
            std::error_code ec;
            std::filesystem::remove_all(fixture_dir_, ec);
        }

        // 经工厂装载夹具目录，返回只读 IDasI18n 视图。
        DAS::DasPtr<Das::ExportInterface::IDasI18n> LoadFromFixtureDir()
        {
            const auto dir_str = Das::Utils::PathToUtf8(fixture_dir_);
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
    EXPECT_NE(std::find(names.begin(), names.end(), "zh-cn"), names.end());
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

// ---------------------------------------------------------------------------
// IDasMutableI18n — AddTranslation / MergeFromFile / bad-JSON 守卫 / 回退链
// ---------------------------------------------------------------------------

namespace
{
    // 构造空可变 i18n 表。
    DAS::DasPtr<Das::ExportInterface::IDasMutableI18n> MakeMutableI18n()
    {
        DAS::DasPtr<Das::ExportInterface::IDasMutableI18n> p;
        ::CreateIDasMutableI18n(p.Put());
        return p;
    }

    // 给当前用例生成唯一临时目录（同样避免并行进程互踩）。
    std::filesystem::path UniqueDirForCurrentTest()
    {
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string leaf = std::string("das_i18n_mutable_")
                                 + info->test_suite_name() + "_" + info->name();
        auto              dir =
            std::filesystem::current_path() / Das::Utils::PathFromUtf8(leaf);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir;
    }
} // namespace

TEST(I18nMutable, AddTranslationAddsEntryTranslatableAfterwards)
{
    auto i18n = MakeMutableI18n();
    ASSERT_NE(i18n.Get(), nullptr);

    auto locale = MakeLocaleName("zh-CN");
    auto key = MakeLocaleName("new.key");
    auto text = MakeLocaleName("\xE6\x96\xB0\xE5\x80\xBC"); // "新值" UTF-8
    ASSERT_EQ(
        i18n->AddTranslation(locale.Get(), key.Get(), text.Get()),
        DAS_S_OK);

    DAS::DasPtr<IDasReadOnlyString> p_out;
    ASSERT_EQ(
        i18n->TranslateWithLocale(locale.Get(), key.Get(), p_out.Put()),
        DAS_S_OK);
    ASSERT_NE(p_out.Get(), nullptr);
    EXPECT_EQ(ToUtf8(p_out.Get()), "\xE6\x96\xB0\xE5\x80\xBC");
}

TEST(I18nMutable, AddTranslationOverwritesExistingKey)
{
    auto i18n = MakeMutableI18n();
    ASSERT_NE(i18n.Get(), nullptr);

    auto locale = MakeLocaleName("en");
    auto key = MakeLocaleName("dup.key");
    auto first = MakeLocaleName("first");
    auto second = MakeLocaleName("second");
    ASSERT_EQ(
        i18n->AddTranslation(locale.Get(), key.Get(), first.Get()),
        DAS_S_OK);
    ASSERT_EQ(
        i18n->AddTranslation(locale.Get(), key.Get(), second.Get()),
        DAS_S_OK);

    DAS::DasPtr<IDasReadOnlyString> p_out;
    ASSERT_EQ(
        i18n->TranslateWithLocale(locale.Get(), key.Get(), p_out.Put()),
        DAS_S_OK);
    ASSERT_NE(p_out.Get(), nullptr);
    EXPECT_EQ(ToUtf8(p_out.Get()), "second");
}

TEST(I18nMutable, AddTranslationEmptyLocaleReturnsInvalidArgument)
{
    auto i18n = MakeMutableI18n();
    ASSERT_NE(i18n.Get(), nullptr);

    auto locale = MakeLocaleName("");
    auto key = MakeLocaleName("any.key");
    auto text = MakeLocaleName("v");
    EXPECT_EQ(
        i18n->AddTranslation(locale.Get(), key.Get(), text.Get()),
        DAS_E_INVALID_ARGUMENT);
}

TEST(I18nMutable, MergeFromFileMergesPerLocaleFlatJson)
{
    auto i18n = MakeMutableI18n();
    ASSERT_NE(i18n.Get(), nullptr);

    const auto dir = UniqueDirForCurrentTest();
    const auto file = dir / DAS_PATH_LITERAL("en.json");
    WriteFile(file, R"({"a.b": "x", "c.d": "y"})");

    auto path = MakeLocaleName(Das::Utils::PathToUtf8(file).c_str());
    ASSERT_EQ(i18n->MergeFromFile(path.Get()), DAS_S_OK);

    auto                            locale = MakeLocaleName("en");
    DAS::DasPtr<IDasReadOnlyString> p_ab;
    ASSERT_EQ(
        i18n->TranslateWithLocale(
            locale.Get(),
            MakeLocaleName("a.b").Get(),
            p_ab.Put()),
        DAS_S_OK);
    EXPECT_EQ(ToUtf8(p_ab.Get()), "x");
    DAS::DasPtr<IDasReadOnlyString> p_cd;
    ASSERT_EQ(
        i18n->TranslateWithLocale(
            locale.Get(),
            MakeLocaleName("c.d").Get(),
            p_cd.Put()),
        DAS_S_OK);
    EXPECT_EQ(ToUtf8(p_cd.Get()), "y");
}

TEST(I18nMutable, MergeFromFileOverwritesExistingKey)
{
    auto i18n = MakeMutableI18n();
    ASSERT_NE(i18n.Get(), nullptr);

    // 先 AddTranslation 写入旧值
    auto locale = MakeLocaleName("en");
    auto key = MakeLocaleName("overlap.key");
    ASSERT_EQ(
        i18n->AddTranslation(
            locale.Get(),
            key.Get(),
            MakeLocaleName("old").Get()),
        DAS_S_OK);

    const auto dir = UniqueDirForCurrentTest();
    const auto file = dir / DAS_PATH_LITERAL("en.json");
    WriteFile(file, R"({"overlap.key": "new"})");
    auto path = MakeLocaleName(Das::Utils::PathToUtf8(file).c_str());
    ASSERT_EQ(i18n->MergeFromFile(path.Get()), DAS_S_OK);

    DAS::DasPtr<IDasReadOnlyString> p_out;
    ASSERT_EQ(
        i18n->TranslateWithLocale(locale.Get(), key.Get(), p_out.Put()),
        DAS_S_OK);
    EXPECT_EQ(ToUtf8(p_out.Get()), "new");
}

TEST(I18nBadJson, MissingBraceReturnsInvalidJsonAndLeavesTableUnchanged)
{
    auto i18n = MakeMutableI18n();
    ASSERT_NE(i18n.Get(), nullptr);

    // 先建一个有效条目，畸形合并后该条目必须仍可命中（事务性：不半合并）
    auto locale = MakeLocaleName("en");
    auto key = MakeLocaleName("keep.me");
    ASSERT_EQ(
        i18n->AddTranslation(
            locale.Get(),
            key.Get(),
            MakeLocaleName("kept").Get()),
        DAS_S_OK);

    const auto dir = UniqueDirForCurrentTest();
    const auto file = dir / DAS_PATH_LITERAL("en.json");
    WriteFile(file, R"({"broken": )"); // 缺值 + 缺花括号
    auto path = MakeLocaleName(Das::Utils::PathToUtf8(file).c_str());
    EXPECT_EQ(i18n->MergeFromFile(path.Get()), DAS_E_INVALID_JSON);

    // 原条目仍在
    DAS::DasPtr<IDasReadOnlyString> p_keep;
    ASSERT_EQ(
        i18n->TranslateWithLocale(locale.Get(), key.Get(), p_keep.Put()),
        DAS_S_OK);
    EXPECT_EQ(ToUtf8(p_keep.Get()), "kept");
    // 畸形文件的 key 未污染表
    DAS::DasPtr<IDasReadOnlyString> p_broken;
    EXPECT_EQ(
        i18n->TranslateWithLocale(
            locale.Get(),
            MakeLocaleName("broken").Get(),
            p_broken.Put()),
        DAS_E_OUT_OF_RANGE);
}

TEST(I18nBadJson, NonObjectRootReturnsInvalidJson)
{
    auto i18n = MakeMutableI18n();
    ASSERT_NE(i18n.Get(), nullptr);

    const auto dir = UniqueDirForCurrentTest();
    const auto file = dir / DAS_PATH_LITERAL("en.json");
    WriteFile(file, R"(["not", "an", "object"])");
    auto path = MakeLocaleName(Das::Utils::PathToUtf8(file).c_str());
    EXPECT_EQ(i18n->MergeFromFile(path.Get()), DAS_E_INVALID_JSON);
}

TEST(I18nFallback, AddedLocaleHitsBeforeBaseMissesViaOtherLocale)
{
    auto i18n = MakeMutableI18n();
    ASSERT_NE(i18n.Get(), nullptr);

    // 给 ja 加 key，en base 无此 key
    auto ja = MakeLocaleName("ja");
    auto key = MakeLocaleName("only.in.ja");
    ASSERT_EQ(
        i18n->AddTranslation(
            ja.Get(),
            key.Get(),
            MakeLocaleName("ja-val").Get()),
        DAS_S_OK);

    // TranslateWithLocale("ja") 直接命中 ja
    DAS::DasPtr<IDasReadOnlyString> p_ja;
    ASSERT_EQ(
        i18n->TranslateWithLocale(ja.Get(), key.Get(), p_ja.Put()),
        DAS_S_OK);
    EXPECT_EQ(ToUtf8(p_ja.Get()), "ja-val");

    // TranslateWithLocale("de") 该 key：de 不在表 → 回退 en base → 仍 miss →
    // OUT_OF_RANGE
    auto                            de = MakeLocaleName("de");
    DAS::DasPtr<IDasReadOnlyString> p_de;
    EXPECT_EQ(
        i18n->TranslateWithLocale(de.Get(), key.Get(), p_de.Put()),
        DAS_E_OUT_OF_RANGE);
    EXPECT_EQ(p_de.Get(), nullptr);
}
