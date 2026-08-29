#include "../src/DasProgressReporter.h"
#include "../src/PiUpdateService.h"

#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Plugins/DasMaaPi/AcceptedSettings.h>
#include <das/Plugins/DasMaaPi/AuthoringProjector.h>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/StringUtils.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProgressService.Implements.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using namespace Das;
    using namespace Das::Plugins::DasMaaPi;

    // RAII：构造时设进程级 GlobalLocale，析构时复位 en（进程单例，测试间
    // 隔离）。DasProgressReporter / AuthoringProjector 的 i18n Translate 经
    // GlobalLocale 解析；测试若不设，默认 en 让进度/releaseNote 文案显示英文，
    // 与中文断言不符。
    class GlobalLocaleGuard
    {
    public:
        explicit GlobalLocaleGuard(const char* locale)
        {
            SetGlobalLocale(locale);
        }

        ~GlobalLocaleGuard() { SetGlobalLocale("en"); }

    private:
        static void SetGlobalLocale(const char* utf8)
        {
            DasPtr<IDasReadOnlyString> locale_name;
            ::CreateIDasReadOnlyStringFromUtf8(utf8, locale_name.Put());
            if (locale_name != nullptr)
            {
                ::DasSetDefaultLocale(locale_name.Get());
            }
        }
    };

    // 测试缝：本地 IDasProgressService 实现。记录 AddCallback /
    // RemoveCallback / Report 收到的 entry + 可读回字段（无需真 IPC）。
    class FakeProgressService final
        : public Das::ExportInterface::DasProgressServiceImplBase<
              FakeProgressService>
    {
    public:
        int                                                add_count = 0;
        int                                                remove_count = 0;
        DasPtr<Das::ExportInterface::IDasProgressCallback> callback;
        std::vector<DasPtr<Das::ExportInterface::IDasProgressUpdate>> entries;

        DasResult AddCallback(
            Das::ExportInterface::IDasProgressCallback* cb) override
        {
            callback = cb;
            ++add_count;
            return DAS_S_OK;
        }

        DasResult RemoveCallback(
            Das::ExportInterface::IDasProgressCallback* cb) override
        {
            if (cb != nullptr && cb == callback.Get())
            {
                callback.Reset();
            }
            ++remove_count;
            return DAS_S_OK;
        }

        DasResult Report(
            Das::ExportInterface::IDasProgressUpdate* entry) override
        {
            entries.emplace_back(entry);
            return DAS_S_OK;
        }
    };

    std::string ReadEntryKey(Das::ExportInterface::IDasProgressUpdate* entry)
    {
        DasPtr<IDasReadOnlyString> key;
        EXPECT_EQ(entry->GetKey(key.Put()), DAS_S_OK);
        const char* raw = nullptr;
        EXPECT_EQ(key->GetUtf8(&raw), DAS_S_OK);
        return raw != nullptr ? std::string(raw) : std::string{};
    }

    std::pair<std::string, std::string> ReadEntryField(
        Das::ExportInterface::IDasProgressUpdate* entry,
        size_t                                    index)
    {
        DasPtr<IDasReadOnlyString>             name;
        DasPtr<Das::ExportInterface::IDasJson> value;
        EXPECT_EQ(
            entry->GetFieldByIndex(index, name.Put(), value.Put()),
            DAS_S_OK);
        const char* name_raw = nullptr;
        EXPECT_EQ(name->GetUtf8(&name_raw), DAS_S_OK);
        DasPtr<IDasReadOnlyString> text;
        EXPECT_EQ(value->ToString(0, text.Put()), DAS_S_OK);
        const char* text_raw = nullptr;
        EXPECT_EQ(text->GetUtf8(&text_raw), DAS_S_OK);
        return {
            name_raw != nullptr ? std::string(name_raw) : std::string{},
            text_raw != nullptr ? std::string(text_raw) : std::string{}};
    }

    // entry 全字段读回（name → JSON 序列化值；字符串值带引号）。
    std::map<std::string, std::string> ReadEntryFields(
        Das::ExportInterface::IDasProgressUpdate* entry)
    {
        std::map<std::string, std::string> result;
        size_t                             count = 0;
        EXPECT_EQ(entry->GetFieldCount(&count), DAS_S_OK);
        for (size_t index = 0; index < count; ++index)
        {
            auto [name, value] = ReadEntryField(entry, index);
            result[name] = value;
        }
        return result;
    }

    // 为 ReportCheckOutcome 用例构造注入式 i18n 表：临时目录写 en/zh-cn
    // JSON 后经工厂装载（经 SetI18n 注入缝，绕开 this_line_location 懒加
    // 载）。with_check_keys=false 时写不含 progress.updateFound/upToDate/
    // checkFailed 的表（缺译回退用例）。ctest 每用例独立进程，目录以用例
    // 名区分防并行互踩。
    DasPtr<Das::ExportInterface::IDasI18n> MakeInjectedCheckOutcomeI18n(
        const char* test_name,
        bool        with_check_keys)
    {
        const auto dir =
            std::filesystem::temp_directory_path()
            / (std::string("maapi_reporter_check_outcome_") + test_name);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);

        const auto write_json = [&dir](const char* name, std::string_view body)
        {
            std::ofstream ofs(dir / name, std::ios::binary | std::ios::trunc);
            ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
        };
        if (with_check_keys)
        {
            write_json(
                "en.json",
                R"json({"progress.updateFound": "New version available ({0})", "progress.upToDate": "Already up to date ({0})", "progress.checkFailed": "Check failed: {0}"})json");
            write_json(
                "zh-cn.json",
                R"json({"progress.updateFound": "发现新版本（{0}）", "progress.upToDate": "已是最新（{0}）", "progress.checkFailed": "检查失败：{0}"})json");
        }
        else
        {
            write_json("en.json", R"json({"label.confirm": "Confirm"})json");
            write_json("zh-cn.json", R"json({"label.confirm": "确认"})json");
        }

        const std::string dir_utf8{DAS::Utils::U8AsString(dir.u8string())};
        DasPtr<IDasReadOnlyString> dir_str;
        ::CreateIDasReadOnlyStringFromUtf8(dir_utf8.c_str(), dir_str.Put());
        DasPtr<Das::ExportInterface::IDasI18n> table;
        if (dir_str != nullptr)
        {
            EXPECT_EQ(
                ::CreateIDasI18nFromDirectory(dir_str.Get(), table.Put()),
                DAS_S_OK);
        }
        else
        {
            ADD_FAILURE() << "CreateIDasReadOnlyStringFromUtf8 failed";
        }
        std::filesystem::remove_all(dir, ec);
        return table;
    }
} // namespace

// ============================================================================
// Task 1: DasProgressReporter（entry 装配 + callback 生命周期 + Report）
// ============================================================================

// Behavior 1+2: Report(entry) → fake service 收到 entry，字段增量逐字段可读回
// （GetKey / GetFieldCount / GetFieldByIndex）。
TEST(MaapiProgressReporterTest, ReportAssemblesFieldIncrementReadableByService)
{
    GlobalLocaleGuard                                 locale_guard("zh-cn");
    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());

    auto reporter = std::make_shared<DasProgressReporter>("key1", service);
    ASSERT_TRUE(reporter->Start());
    ASSERT_EQ(fake->add_count, 1);

    UpdateProgress progress{};
    progress.stage = UpdateStage::Download;
    progress.bytes_received = 100;
    progress.total_bytes = 200;
    progress.rate_mbps = 2.5;
    reporter->Report(progress);

    ASSERT_EQ(fake->entries.size(), 1u);
    auto entry = fake->entries.front();
    EXPECT_EQ(ReadEntryKey(entry.Get()), "key1");

    size_t count = 0;
    ASSERT_EQ(entry->GetFieldCount(&count), DAS_S_OK);
    ASSERT_GE(count, 3u);

    auto [name0, value0] = ReadEntryField(entry.Get(), 0);
    EXPECT_EQ(name0, "updateProgress");
    EXPECT_NE(value0.find("正在下载"), std::string::npos);
    EXPECT_NE(value0.find("50%"), std::string::npos);

    auto [name1, value1] = ReadEntryField(entry.Get(), 1);
    EXPECT_EQ(name1, "downloadRate");
    EXPECT_NE(value1.find("2.5 MB/s"), std::string::npos);

    auto [name2, value2] = ReadEntryField(entry.Get(), 2);
    EXPECT_EQ(name2, "downloadElapsed");

    reporter->Stop();
    EXPECT_EQ(fake->remove_count, 1);
}

// Behavior 3: OnAllSubscribersDisconnected 按 key 过滤——own key 命中停止上报，
// 其它 key 忽略。
TEST(MaapiProgressReporterTest, OnAllSubscribersDisconnectedFiltersByOwnKey)
{
    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());

    auto reporter = std::make_shared<DasProgressReporter>("key1", service);
    ASSERT_TRUE(reporter->Start());
    ASSERT_TRUE(fake->callback);

    UpdateProgress progress{};
    progress.stage = UpdateStage::Download;
    progress.bytes_received = 10;
    progress.total_bytes = 100;

    // 其它 key 的断连通知 → 忽略，仍上报。
    {
        DasPtr<IDasReadOnlyString> other_key;
        ASSERT_EQ(
            CreateIDasReadOnlyStringFromUtf8("other", other_key.Put()),
            DAS_S_OK);
        ASSERT_EQ(
            fake->callback->OnAllSubscribersDisconnected(other_key.Get()),
            DAS_S_OK);
    }
    reporter->Report(progress);
    ASSERT_EQ(fake->entries.size(), 1u);

    // own key 的断连通知 → 停止上报标志 + 不再上报。
    {
        DasPtr<IDasReadOnlyString> own_key;
        ASSERT_EQ(
            CreateIDasReadOnlyStringFromUtf8("key1", own_key.Put()),
            DAS_S_OK);
        ASSERT_EQ(
            fake->callback->OnAllSubscribersDisconnected(own_key.Get()),
            DAS_S_OK);
    }
    EXPECT_TRUE(reporter->IsDisconnected());
    reporter->Report(progress);
    ASSERT_EQ(fake->entries.size(), 1u);

    reporter->Stop();
}

// Behavior 4: RemoveCallback 成对（service 侧 callback 释放）+ 停止后不再上报。
TEST(MaapiProgressReporterTest, RemoveCallbackStopsReporting)
{
    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());

    auto reporter = std::make_shared<DasProgressReporter>("key1", service);
    ASSERT_TRUE(reporter->Start());
    ASSERT_EQ(fake->add_count, 1);
    ASSERT_TRUE(fake->callback);

    reporter->Stop();
    EXPECT_EQ(fake->remove_count, 1);
    EXPECT_FALSE(fake->callback);

    UpdateProgress progress{};
    progress.stage = UpdateStage::Extract;
    reporter->Report(progress);
    EXPECT_EQ(fake->entries.size(), 0u);
}

// ============================================================================
// ReportFinal：后台更新 job 终态字段增量（三字段恒推：文案 + 空速率 + 空
// 耗时；i18n 模板 0 基 {0} 填 detail）。i18n 表部署在测试 exe 目录（懒加载
// 经 this_line_location 解析），locale 经 GlobalLocaleGuard 切换覆盖两语言。
// ============================================================================

// 成功终态：updateProgress 填入版本号（zh 模板「更新完成（{0}）」），
// downloadRate / downloadElapsed 为空串值。
TEST(MaapiProgressReporterTest, ReportFinalSuccessFillsVersionTemplate)
{
    GlobalLocaleGuard                                 locale_guard("zh-cn");
    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());

    auto reporter = std::make_shared<DasProgressReporter>("key1", service);
    ASSERT_TRUE(reporter->Start());

    reporter->ReportFinal(true, "v1.2.0");

    ASSERT_EQ(fake->entries.size(), 1u);
    auto entry = fake->entries.front();
    EXPECT_EQ(ReadEntryKey(entry.Get()), "key1");

    size_t count = 0;
    ASSERT_EQ(entry->GetFieldCount(&count), DAS_S_OK);
    ASSERT_EQ(count, 3u);

    const auto [name0, value0] = ReadEntryField(entry.Get(), 0);
    EXPECT_EQ(name0, "updateProgress");
    EXPECT_NE(value0.find("更新完成"), std::string::npos);
    EXPECT_NE(value0.find("v1.2.0"), std::string::npos);

    // IDasJson::ToString(0) 返回紧凑 JSON 序列化——空串值是两字符 ""。
    const auto [name1, value1] = ReadEntryField(entry.Get(), 1);
    EXPECT_EQ(name1, "downloadRate");
    EXPECT_EQ(value1, "\"\"");

    const auto [name2, value2] = ReadEntryField(entry.Get(), 2);
    EXPECT_EQ(name2, "downloadElapsed");
    EXPECT_EQ(value2, "\"\"");

    reporter->Stop();
}

// 成功终态（默认 en locale）：模板 "Update complete ({0})"。
TEST(MaapiProgressReporterTest, ReportFinalSuccessEnglishTemplate)
{
    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());

    auto reporter = std::make_shared<DasProgressReporter>("key1", service);
    ASSERT_TRUE(reporter->Start());

    reporter->ReportFinal(true, "v2.0.1");

    ASSERT_EQ(fake->entries.size(), 1u);
    const auto [name0, value0] = ReadEntryField(fake->entries.front().Get(), 0);
    EXPECT_EQ(name0, "updateProgress");
    EXPECT_NE(value0.find("Update complete"), std::string::npos);
    EXPECT_NE(value0.find("v2.0.1"), std::string::npos);
}

// 失败终态：updateProgress 填入失败概要（detail 为 DasResult 十进制串）。
TEST(MaapiProgressReporterTest, ReportFinalFailureFillsDetailTemplate)
{
    GlobalLocaleGuard                                 locale_guard("zh-cn");
    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());

    auto reporter = std::make_shared<DasProgressReporter>("key1", service);
    ASSERT_TRUE(reporter->Start());

    reporter->ReportFinal(false, "result=-10008");

    ASSERT_EQ(fake->entries.size(), 1u);
    const auto [name0, value0] = ReadEntryField(fake->entries.front().Get(), 0);
    EXPECT_EQ(name0, "updateProgress");
    EXPECT_NE(value0.find("更新失败"), std::string::npos);
    EXPECT_NE(value0.find("result=-10008"), std::string::npos);
}

// 未 Start → ReportFinal 无推送（add_count 不变、entries 空）。
TEST(MaapiProgressReporterTest, ReportFinalWithoutStartDoesNotPush)
{
    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());

    auto reporter = std::make_shared<DasProgressReporter>("key1", service);
    reporter->ReportFinal(true, "v1.2.0");

    EXPECT_EQ(fake->add_count, 0);
    EXPECT_EQ(fake->entries.size(), 0u);
}

// 已 Stop（终态后）→ ReportFinal 无推送。
TEST(MaapiProgressReporterTest, ReportFinalAfterStopDoesNotPush)
{
    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());

    auto reporter = std::make_shared<DasProgressReporter>("key1", service);
    ASSERT_TRUE(reporter->Start());
    reporter->Stop();
    const int remove_count_before = fake->remove_count;

    reporter->ReportFinal(false, "result=-10008");

    EXPECT_EQ(fake->entries.size(), 0u);
    EXPECT_EQ(fake->remove_count, remove_count_before);
}

// ============================================================================
// ReportCheckOutcome：checkUpdate 三终局多字段推送（FoundUpdate 三字段：
// updateVersion「cur → next」三元退化 + releaseNote 原文 + updateProgress
// 模板文案；UpToDate/Failed 两字段：updateVersion 空串 + updateProgress。
// 不推 downloadRate/downloadElapsed。i18n 表经 SetI18n 注入缝（临时目录
// 工厂装载），缺译回退硬编码中文。
// ============================================================================

// FoundUpdate：恰 3 字段，{0}=目标版本，版本为「cur → next」。
TEST(MaapiProgressReporterTest, ReportCheckOutcomeFoundUpdatePushesThreeFields)
{
    GlobalLocaleGuard                      locale_guard("zh-cn");
    DasPtr<Das::ExportInterface::IDasI18n> i18n = MakeInjectedCheckOutcomeI18n(
        "ReportCheckOutcomeFoundUpdatePushesThreeFields",
        /*with_check_keys=*/true);
    ASSERT_NE(i18n.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());

    auto reporter = std::make_shared<DasProgressReporter>("key1", service);
    reporter->SetI18n(i18n);
    ASSERT_TRUE(reporter->Start());

    PiUpdateInfo info;
    info.has_update = true;
    info.version_name = "v2.23.0";
    info.release_note = "日志";
    reporter->ReportCheckOutcome(
        DasProgressReporter::CheckOutcomeKind::FoundUpdate,
        info,
        "v2.22.0",
        "");

    ASSERT_EQ(fake->entries.size(), 1u);
    auto entry = fake->entries.front();
    EXPECT_EQ(ReadEntryKey(entry.Get()), "key1");

    const auto fields = ReadEntryFields(entry.Get());
    ASSERT_EQ(fields.size(), 3u);
    EXPECT_EQ(fields.at("updateVersion"), "\"v2.22.0 → v2.23.0\"");
    EXPECT_EQ(fields.at("releaseNote"), "\"日志\"");
    EXPECT_EQ(fields.at("updateProgress"), "\"发现新版本（v2.23.0）\"");
    EXPECT_EQ(fields.count("downloadRate"), 0u);
    EXPECT_EQ(fields.count("downloadElapsed"), 0u);

    reporter->Stop();
}

// UpToDate：恰 2 字段，updateVersion 空串，{0}=当前版本。
TEST(MaapiProgressReporterTest, ReportCheckOutcomeUpToDatePushesTwoFields)
{
    GlobalLocaleGuard                      locale_guard("zh-cn");
    DasPtr<Das::ExportInterface::IDasI18n> i18n = MakeInjectedCheckOutcomeI18n(
        "ReportCheckOutcomeUpToDatePushesTwoFields",
        /*with_check_keys=*/true);
    ASSERT_NE(i18n.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());

    auto reporter = std::make_shared<DasProgressReporter>("key1", service);
    reporter->SetI18n(i18n);
    ASSERT_TRUE(reporter->Start());

    PiUpdateInfo info;
    reporter->ReportCheckOutcome(
        DasProgressReporter::CheckOutcomeKind::UpToDate,
        info,
        "v2.22.0",
        "");

    ASSERT_EQ(fake->entries.size(), 1u);
    const auto fields = ReadEntryFields(fake->entries.front().Get());
    ASSERT_EQ(fields.size(), 2u);
    EXPECT_EQ(fields.at("updateVersion"), "\"\"");
    EXPECT_EQ(fields.at("updateProgress"), "\"已是最新（v2.22.0）\"");
    EXPECT_EQ(fields.count("releaseNote"), 0u);
    EXPECT_EQ(fields.count("downloadRate"), 0u);

    reporter->Stop();
}

// Failed：恰 2 字段，updateVersion 空串，{0}=失败分类文案。
TEST(MaapiProgressReporterTest, ReportCheckOutcomeFailedPushesTwoFields)
{
    GlobalLocaleGuard                      locale_guard("zh-cn");
    DasPtr<Das::ExportInterface::IDasI18n> i18n = MakeInjectedCheckOutcomeI18n(
        "ReportCheckOutcomeFailedPushesTwoFields",
        /*with_check_keys=*/true);
    ASSERT_NE(i18n.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());

    auto reporter = std::make_shared<DasProgressReporter>("key1", service);
    reporter->SetI18n(i18n);
    ASSERT_TRUE(reporter->Start());

    PiUpdateInfo info;
    reporter->ReportCheckOutcome(
        DasProgressReporter::CheckOutcomeKind::Failed,
        info,
        "",
        "分类文案");

    ASSERT_EQ(fake->entries.size(), 1u);
    const auto fields = ReadEntryFields(fake->entries.front().Get());
    ASSERT_EQ(fields.size(), 2u);
    EXPECT_EQ(fields.at("updateVersion"), "\"\"");
    EXPECT_EQ(fields.at("updateProgress"), "\"检查失败：分类文案\"");

    reporter->Stop();
}

// version_text 三元退化：current_version 空且 version_name 非空 → 单值
// next；反向 → 单值 cur（照投影五字段构造逻辑）。
TEST(
    MaapiProgressReporterTest,
    ReportCheckOutcomeVersionTextDegradesWhenSideEmpty)
{
    GlobalLocaleGuard                      locale_guard("zh-cn");
    DasPtr<Das::ExportInterface::IDasI18n> i18n = MakeInjectedCheckOutcomeI18n(
        "ReportCheckOutcomeVersionTextDegradesWhenSideEmpty",
        /*with_check_keys=*/true);
    ASSERT_NE(i18n.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());

    auto reporter = std::make_shared<DasProgressReporter>("key1", service);
    reporter->SetI18n(i18n);
    ASSERT_TRUE(reporter->Start());

    {
        PiUpdateInfo info;
        info.has_update = true;
        info.version_name = "v2.23.0";
        reporter->ReportCheckOutcome(
            DasProgressReporter::CheckOutcomeKind::FoundUpdate,
            info,
            "",
            "");
        ASSERT_EQ(fake->entries.size(), 1u);
        const auto fields = ReadEntryFields(fake->entries.back().Get());
        EXPECT_EQ(fields.at("updateVersion"), "\"v2.23.0\"");
    }
    {
        PiUpdateInfo info;
        info.has_update = true;
        reporter->ReportCheckOutcome(
            DasProgressReporter::CheckOutcomeKind::FoundUpdate,
            info,
            "v2.22.0",
            "");
        ASSERT_EQ(fake->entries.size(), 2u);
        const auto fields = ReadEntryFields(fake->entries.back().Get());
        EXPECT_EQ(fields.at("updateVersion"), "\"v2.22.0\"");
    }

    reporter->Stop();
}

// 缺译回退：注入不含 progress.updateFound/upToDate/checkFailed 的表 →
// updateProgress 落硬编码中文（发现新版本（…）/已是最新（…）/检查失败：…）。
TEST(MaapiProgressReporterTest, ReportCheckOutcomeFallsBackWhenKeyMissing)
{
    GlobalLocaleGuard                      locale_guard("zh-cn");
    DasPtr<Das::ExportInterface::IDasI18n> i18n = MakeInjectedCheckOutcomeI18n(
        "ReportCheckOutcomeFallsBackWhenKeyMissing",
        /*with_check_keys=*/false);
    ASSERT_NE(i18n.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());

    auto reporter = std::make_shared<DasProgressReporter>("key1", service);
    reporter->SetI18n(i18n);
    ASSERT_TRUE(reporter->Start());

    {
        PiUpdateInfo info;
        info.has_update = true;
        info.version_name = "v2.23.0";
        reporter->ReportCheckOutcome(
            DasProgressReporter::CheckOutcomeKind::FoundUpdate,
            info,
            "v2.22.0",
            "");
    }
    {
        PiUpdateInfo info;
        reporter->ReportCheckOutcome(
            DasProgressReporter::CheckOutcomeKind::UpToDate,
            info,
            "v2.22.0",
            "");
    }
    {
        PiUpdateInfo info;
        reporter->ReportCheckOutcome(
            DasProgressReporter::CheckOutcomeKind::Failed,
            info,
            "",
            "分类文案");
    }

    ASSERT_EQ(fake->entries.size(), 3u);
    EXPECT_EQ(
        ReadEntryFields(fake->entries[0].Get()).at("updateProgress"),
        "\"发现新版本（v2.23.0）\"");
    EXPECT_EQ(
        ReadEntryFields(fake->entries[1].Get()).at("updateProgress"),
        "\"已是最新（v2.22.0）\"");
    EXPECT_EQ(
        ReadEntryFields(fake->entries[2].Get()).at("updateProgress"),
        "\"检查失败：分类文案\"");

    reporter->Stop();
}

// 早退链：未 Start 的 reporter 调用 → service 零 entry（零 AddCallback）。
TEST(MaapiProgressReporterTest, ReportCheckOutcomeWithoutStartDoesNotPush)
{
    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());

    auto reporter = std::make_shared<DasProgressReporter>("key1", service);
    PiUpdateInfo info;
    reporter->ReportCheckOutcome(
        DasProgressReporter::CheckOutcomeKind::FoundUpdate,
        info,
        "v2.22.0",
        "");

    EXPECT_EQ(fake->add_count, 0);
    EXPECT_EQ(fake->entries.size(), 0u);
}

// ============================================================================
// Task 3: 更新 section 字段投影（五字段无 updateKey + fieldLayout 置顶）
// ============================================================================

std::optional<yyjson::value> FindRootField(
    const yyjson::value& document,
    std::string_view     name)
{
    auto obj = document.as_object();
    if (!obj || !obj->contains(std::string_view("fields")))
    {
        return std::nullopt;
    }
    auto fields = (*obj)[std::string_view("fields")].as_array();
    if (!fields)
    {
        return std::nullopt;
    }
    for (auto it = fields->begin(); it != fields->end(); ++it)
    {
        auto field = it->as_object();
        if (!field)
        {
            continue;
        }
        if ((*field)[std::string_view("name")].as_string().value_or("") == name)
        {
            return Das::Utils::CloneYyjsonValue(*it);
        }
    }
    return std::nullopt;
}

std::optional<yyjson::value> FindLayoutSection(
    const yyjson::value& document,
    std::string_view     id)
{
    auto obj = document.as_object();
    if (!obj || !obj->contains(std::string_view("fieldLayout")))
    {
        return std::nullopt;
    }
    auto layout = (*obj)[std::string_view("fieldLayout")].as_array();
    if (!layout)
    {
        return std::nullopt;
    }
    for (auto it = layout->begin(); it != layout->end(); ++it)
    {
        auto section = it->as_object();
        if (!section)
        {
            continue;
        }
        if ((*section)[std::string_view("id")].as_string().value_or("") == id)
        {
            return Das::Utils::CloneYyjsonValue(*it);
        }
    }
    return std::nullopt;
}

std::vector<std::string> LayoutSectionFieldNames(const yyjson::value& section)
{
    std::vector<std::string> names;
    auto                     section_obj = section.as_object();
    if (!section_obj || !section_obj->contains(std::string_view("entries")))
    {
        return names;
    }
    auto entries = (*section_obj)[std::string_view("entries")].as_array();
    if (!entries)
    {
        return names;
    }
    for (auto it = entries->begin(); it != entries->end(); ++it)
    {
        auto leaf = it->as_object();
        // 跳过非 fieldName 叶子（{commandId} 叶子、{row:[...]} 容器）—
        // cpp_yyjson operator[] 对缺失 key 抛异常。
        if (!leaf || !leaf->contains(std::string_view("fieldName")))
        {
            continue;
        }
        names.emplace_back(
            (*leaf)[std::string_view("fieldName")].as_string().value_or(""));
    }
    return names;
}

// Behavior 1: pending_update engaged → document.fields[] 含五字段（无
// updateKey）；releaseNote inputType==7 && readonly==true。
TEST(MaapiProgressReporterTest, UpdateSectionProjectedWhenPending)
{
    using namespace Das::Plugins::DasMaaPi;

    AcceptedSettingsDto         settings;
    std::optional<PiUpdateInfo> pending;
    pending.emplace();
    pending->has_update = true;
    pending->version_name = "v1.2.3";
    pending->release_note = "# 更新\n- 新功能";
    pending->download_url = "https://example.com/package.zip";

    auto document =
        ProjectAuthoringDocument(settings, nullptr, {}, 0, nullptr, &pending);

    auto version = FindRootField(document, "updateVersion");
    ASSERT_TRUE(version.has_value());
    EXPECT_EQ(
        std::string((*version->as_object())[std::string_view("value")]
                        .as_string()
                        .value_or("")),
        "v1.2.3");

    auto release = FindRootField(document, "releaseNote");
    ASSERT_TRUE(release.has_value());
    auto release_obj = release->as_object();
    ASSERT_TRUE(release_obj.has_value());
    EXPECT_EQ(
        (*release_obj)[std::string_view("inputType")].as_sint().value_or(-1),
        7);
    EXPECT_TRUE(
        (*release_obj)[std::string_view("readonly")].as_bool().value_or(false));

    EXPECT_TRUE(FindRootField(document, "updateProgress").has_value());
    EXPECT_TRUE(FindRootField(document, "downloadRate").has_value());
    EXPECT_TRUE(FindRootField(document, "downloadElapsed").has_value());
    // key 传递方案 B：SSE key 经 commandResult 返回，不投影 document。
    EXPECT_FALSE(FindRootField(document, "updateKey").has_value());
    EXPECT_FALSE(FindRootField(document, "progressKey").has_value());
}

// Behavior 2: 无 pending_update → 五字段不出现（更新 panel 不常驻）；既有更新
// section 字段（CDK/代理/通道/版本）仍存在。
TEST(MaapiProgressReporterTest, UpdateSectionAbsentWhenNoPending)
{
    using namespace Das::Plugins::DasMaaPi;

    AcceptedSettingsDto settings;
    auto document = ProjectAuthoringDocument(settings, nullptr, {}, 0);

    EXPECT_FALSE(FindRootField(document, "updateVersion").has_value());
    EXPECT_FALSE(FindRootField(document, "releaseNote").has_value());
    EXPECT_FALSE(FindRootField(document, "updateProgress").has_value());
    EXPECT_FALSE(FindRootField(document, "downloadRate").has_value());
    EXPECT_FALSE(FindRootField(document, "downloadElapsed").has_value());

    EXPECT_TRUE(FindRootField(document, "adapter.proxyUrl").has_value());
    EXPECT_TRUE(FindRootField(document, "resourceVersion").has_value());
}

// Behavior 3: releaseNote 值为 markdown 文本（来自
// PiUpdateInfo.release_note）； 无内容 → 「暂无更新日志」。
TEST(MaapiProgressReporterTest, ReleaseNoteValueFromPendingAndEmptyFallback)
{
    GlobalLocaleGuard locale_guard("zh-cn");
    using namespace Das::Plugins::DasMaaPi;

    AcceptedSettingsDto settings;

    {
        std::optional<PiUpdateInfo> pending;
        pending.emplace();
        pending->has_update = true;
        pending->release_note = "**加粗** release note";
        auto document = ProjectAuthoringDocument(
            settings,
            nullptr,
            {},
            0,
            nullptr,
            &pending);
        auto release = FindRootField(document, "releaseNote");
        ASSERT_TRUE(release.has_value());
        EXPECT_EQ(
            std::string((*release->as_object())[std::string_view("value")]
                            .as_string()
                            .value_or("")),
            "**加粗** release note");
    }

    {
        std::optional<PiUpdateInfo> pending;
        pending.emplace();
        pending->has_update = true;
        auto document = ProjectAuthoringDocument(
            settings,
            nullptr,
            {},
            0,
            nullptr,
            &pending);
        auto release = FindRootField(document, "releaseNote");
        ASSERT_TRUE(release.has_value());
        EXPECT_EQ(
            std::string((*release->as_object())[std::string_view("value")]
                            .as_string()
                            .value_or("")),
            "暂无更新日志");
    }
}

// Behavior 4: pending 时 fieldLayout 首位是更新 section，且五字段在更新 section
// 内。
TEST(MaapiProgressReporterTest, UpdateSectionFirstInLayoutWhenPending)
{
    using namespace Das::Plugins::DasMaaPi;

    AcceptedSettingsDto         settings;
    std::optional<PiUpdateInfo> pending;
    pending.emplace();
    pending->has_update = true;
    pending->version_name = "v1.2.3";
    pending->release_note = "note";
    pending->download_url = "https://example.com/package.zip";

    auto document =
        ProjectAuthoringDocument(settings, nullptr, {}, 0, nullptr, &pending);

    auto doc_obj = document.as_object();
    ASSERT_TRUE(doc_obj.has_value());
    auto layout = (*doc_obj)[std::string_view("fieldLayout")].as_array();
    ASSERT_TRUE(layout.has_value());
    ASSERT_FALSE(layout->empty());
    auto first = layout->begin()->as_object();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(
        std::string((*first)[std::string_view("id")].as_string().value_or("")),
        "update");

    auto update_section = FindLayoutSection(document, "update");
    ASSERT_TRUE(update_section.has_value());
    auto names = LayoutSectionFieldNames(*update_section);
    for (const auto* expected :
         {"updateVersion",
          "releaseNote",
          "updateProgress",
          "downloadRate",
          "downloadElapsed"})
    {
        EXPECT_NE(std::find(names.begin(), names.end(), expected), names.end())
            << "update section must contain " << expected;
    }
}
