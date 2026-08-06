#include "../src/DasProgressReporter.h"
#include "../src/PiUpdateService.h"

#include <das/DasApi.h>
#include <das/Plugins/DasMaaPi/AcceptedSettings.h>
#include <das/Plugins/DasMaaPi/AuthoringProjector.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProgressService.Implements.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using namespace Das;
    using namespace Das::Plugins::DasMaaPi;

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
} // namespace

// ============================================================================
// Task 1: DasProgressReporter（entry 装配 + callback 生命周期 + Report）
// ============================================================================

// Behavior 1+2: Report(entry) → fake service 收到 entry，字段增量逐字段可读回
// （GetKey / GetFieldCount / GetFieldByIndex）。
TEST(MaapiProgressReporterTest, ReportAssemblesFieldIncrementReadableByService)
{
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
