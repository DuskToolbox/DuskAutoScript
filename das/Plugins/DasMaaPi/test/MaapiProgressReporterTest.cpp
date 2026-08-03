#include "../src/DasProgressReporter.h"
#include "../src/PiUpdateService.h"

#include <das/DasApi.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProgressService.Implements.hpp>
#include <gtest/gtest.h>

#include <cstdint>
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

    DasProgressReporter reporter("key1", service);
    ASSERT_TRUE(reporter.Start());
    ASSERT_EQ(fake->add_count, 1);

    UpdateProgress progress{};
    progress.stage = UpdateStage::Download;
    progress.bytes_received = 100;
    progress.total_bytes = 200;
    progress.rate_mbps = 2.5;
    reporter.Report(progress);

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

    reporter.Stop();
    EXPECT_EQ(fake->remove_count, 1);
}

// Behavior 3: OnAllSubscribersDisconnected 按 key 过滤——own key 命中停止上报，
// 其它 key 忽略。
TEST(MaapiProgressReporterTest, OnAllSubscribersDisconnectedFiltersByOwnKey)
{
    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());

    DasProgressReporter reporter("key1", service);
    ASSERT_TRUE(reporter.Start());
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
    reporter.Report(progress);
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
    EXPECT_TRUE(reporter.IsDisconnected());
    reporter.Report(progress);
    ASSERT_EQ(fake->entries.size(), 1u);

    reporter.Stop();
}

// Behavior 4: RemoveCallback 成对（service 侧 callback 释放）+ 停止后不再上报。
TEST(MaapiProgressReporterTest, RemoveCallbackStopsReporting)
{
    DasPtr<Das::ExportInterface::IDasProgressService> service =
        FakeProgressService::Make();
    auto* fake = static_cast<FakeProgressService*>(service.Get());

    DasProgressReporter reporter("key1", service);
    ASSERT_TRUE(reporter.Start());
    ASSERT_EQ(fake->add_count, 1);
    ASSERT_TRUE(fake->callback);

    reporter.Stop();
    EXPECT_EQ(fake->remove_count, 1);
    EXPECT_FALSE(fake->callback);

    UpdateProgress progress{};
    progress.stage = UpdateStage::Extract;
    reporter.Report(progress);
    EXPECT_EQ(fake->entries.size(), 0u);
}
