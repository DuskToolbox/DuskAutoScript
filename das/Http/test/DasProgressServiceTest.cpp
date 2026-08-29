#include "SseHub.hpp"
#include "beast/SseSession.hpp" // for SseSessionBase
#include "service/DasProgressServiceImpl.h"

#include <boost/beast/_experimental/test/stream.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProgressCallback.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProgressUpdate.Implements.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // 轻量 fake SSE 会话：只记录广播消息，供 SseHub 广播验证。
    class FakeSession : public Das::Http::Beast::SseSessionBase
    {
    public:
        FakeSession(boost::asio::io_context& ioc, std::string key)
            : key_(std::move(key)), ex_(ioc.get_executor())
        {
        }

        boost::asio::any_io_executor GetExecutor() override { return ex_; }

        void WriteData(std::shared_ptr<const std::string> payload) override
        {
            messages_.push_back(std::move(payload));
        }

        const std::string& Key() const noexcept override { return key_; }

        std::string                                     key_;
        boost::asio::any_io_executor                    ex_;
        std::vector<std::shared_ptr<const std::string>> messages_;
    };

    // 插件侧 IDasJson 构造惯例：经 ParseDasJsonFromString 序列化往返创建。
    Das::DasPtr<Das::ExportInterface::IDasJson> MakeJsonValue(
        const std::string& text)
    {
        yyjson::value value{std::string(text), yyjson::copy_string};
        auto          serialized = Das::Utils::SerializeYyjsonValue(value);
        if (!serialized)
        {
            return {};
        }
        Das::DasPtr<Das::ExportInterface::IDasJson> result;
        ParseDasJsonFromString(serialized->c_str(), result.Put());
        return result;
    }

    // 直接把 JSON 文本解析成 IDasJson（value 为 object/array 等非标量，
    // 用于非法字段值测试——service 应安全跳过）。
    Das::DasPtr<Das::ExportInterface::IDasJson> MakeJsonDocumentValue(
        const std::string& json_text)
    {
        Das::DasPtr<Das::ExportInterface::IDasJson> result;
        ParseDasJsonFromString(json_text.c_str(), result.Put());
        return result;
    }

    // 只读进度更新 fake（IDasProgressUpdate 本地实现）。字段项为 nullopt 时
    // GetFieldByIndex 返回越界错误（模拟坏字段，Report 应安全跳过）。
    class FakeProgressUpdate final
        : public Das::ExportInterface::DasProgressUpdateImplBase<
              FakeProgressUpdate>
    {
    public:
        using Field =
            std::pair<std::string, Das::DasPtr<Das::ExportInterface::IDasJson>>;

        FakeProgressUpdate(
            std::string                       key,
            std::vector<std::optional<Field>> fields)
            : key_(std::move(key)), fields_(std::move(fields))
        {
        }

        DasResult GetKey(IDasReadOnlyString** pp_out_key) override
        {
            if (pp_out_key == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_key = nullptr;
            return CreateIDasReadOnlyStringFromUtf8(key_.c_str(), pp_out_key);
        }

        DasResult GetFieldCount(size_t* p_out_count) override
        {
            if (p_out_count == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_out_count = fields_.size();
            return DAS_S_OK;
        }

        DasResult GetFieldByIndex(
            size_t                           index,
            IDasReadOnlyString**             pp_out_name,
            Das::ExportInterface::IDasJson** pp_out_value) override
        {
            if (pp_out_name == nullptr || pp_out_value == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_name = nullptr;
            *pp_out_value = nullptr;
            if (index >= fields_.size() || !fields_[index])
            {
                return DAS_E_OUT_OF_RANGE;
            }
            auto rc = CreateIDasReadOnlyStringFromUtf8(
                fields_[index]->first.c_str(),
                pp_out_name);
            if (DAS::IsFailed(rc))
            {
                return rc;
            }
            *pp_out_value = fields_[index]->second.Get();
            (*pp_out_value)->AddRef();
            return DAS_S_OK;
        }

    private:
        std::string                       key_;
        std::vector<std::optional<Field>> fields_;
    };

    // 反向通知回调 fake（IDasProgressCallback 本地实现），带原子引用计数可
    // 断言 RemoveCallback 后引用归位（无泄漏）。
    class FakeProgressCallback final
        : public Das::ExportInterface::IDasProgressCallback
    {
    public:
        FakeProgressCallback() = default;

        DasResult OnAllSubscribersDisconnected(IDasReadOnlyString* key) override
        {
            if (key == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            const char* raw = nullptr;
            if (DAS::IsFailed(key->GetUtf8(&raw)) || raw == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            disconnected_keys_.push_back(raw);
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out_object) override
        {
            if (pp_out_object == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_object = nullptr;
            if (iid == DasIidOf<IDasBase>()
                || iid
                       == DasIidOf<
                           Das::ExportInterface::IDasProgressCallback>())
            {
                AddRef();
                *pp_out_object =
                    static_cast<Das::ExportInterface::IDasProgressCallback*>(
                        this);
                return DAS_S_OK;
            }
            return DAS_E_NO_INTERFACE;
        }

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        [[nodiscard]]
        uint32_t RefCount() const
        {
            return ref_count_.load();
        }

        [[nodiscard]]
        const std::vector<std::string>& DisconnectedKeys() const
        {
            return disconnected_keys_;
        }

    private:
        std::atomic<uint32_t>    ref_count_{1};
        std::vector<std::string> disconnected_keys_;
    };

    // 解析 setValue payload JSON，返回 fieldName / value 便于断言。
    struct ParsedPayload
    {
        std::string field_name;
        std::string value_text;
        std::string target_kind;
    };

    std::optional<ParsedPayload> ParsePayload(const std::string& json_text)
    {
        auto parsed = Das::Utils::ParseYyjsonFromString(json_text);
        if (!parsed)
        {
            return std::nullopt;
        }
        auto obj = parsed->as_object();
        if (!obj)
        {
            return std::nullopt;
        }
        ParsedPayload result;
        result.field_name =
            (*obj)[std::string_view("fieldName")].as_string().value_or("");
        auto target = (*obj)[std::string_view("target")].as_object();
        if (target)
        {
            result.target_kind =
                (*target)[std::string_view("kind")].as_string().value_or("");
        }
        auto value_ref = (*obj)[std::string_view("value")];
        auto value_clone = Das::Utils::CloneYyjsonValue(value_ref);
        result.value_text =
            Das::Utils::SerializeYyjsonValue(value_clone).value_or("");
        return result;
    }

    // 构建 service + hub 夹具。
    struct ServiceFixture
    {
        boost::asio::io_context                            ioc;
        std::shared_ptr<Das::Http::SseHub>                 hub;
        std::shared_ptr<Das::Http::DasProgressServiceImpl> service;

        ServiceFixture()
            : hub(std::make_shared<Das::Http::SseHub>(ioc)),
              service(std::make_shared<Das::Http::DasProgressServiceImpl>(hub))
        {
        }

        void WireKeyEmpty()
        {
            hub->SetOnKeyEmpty([this](const std::string& key)
                               { service->OnKeySubscribersEmpty(key); });
        }
    };

    // 驱动就绪的异步 handler 链（SSE 头写 → 注册 → 重放 → 直播多级链）。
    void PumpIo(boost::asio::io_context& ioc)
    {
        for (int i = 0; i < 50; ++i)
        {
            ioc.poll();
        }
    }
} // namespace

TEST(DasProgressServiceTest, ReportBroadcastsSetValuePayloadPerField)
{
    ServiceFixture fixture;
    auto session = std::make_shared<FakeSession>(fixture.ioc, "rt-key");
    fixture.hub->Register(session, "rt-key");

    std::vector<std::optional<FakeProgressUpdate::Field>> fields;
    fields.emplace_back(
        std::make_pair(
            std::string("updateProgress"),
            MakeJsonValue("正在下载（12.3 MB / 45.6 MB）27%")));
    fields.emplace_back(
        std::make_pair(std::string("downloadRate"), MakeJsonValue("2.5 MB/s")));
    auto entry = FakeProgressUpdate::Make("rt-key", std::move(fields));

    ASSERT_EQ(fixture.service->Report(entry.Get()), DAS_S_OK);
    fixture.ioc.poll();

    ASSERT_EQ(session->messages_.size(), 2u);
    auto first = ParsePayload(*session->messages_[0]);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->target_kind, "document");
    EXPECT_EQ(first->field_name, "updateProgress");
    EXPECT_EQ(first->value_text, "\"正在下载（12.3 MB / 45.6 MB）27%\"");

    auto second = ParsePayload(*session->messages_[1]);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->target_kind, "document");
    EXPECT_EQ(second->field_name, "downloadRate");
    EXPECT_EQ(second->value_text, "\"2.5 MB/s\"");
}

TEST(DasProgressServiceTest, AddCallbackStoresAndRemoveCallbackReleases)
{
    ServiceFixture fixture;

    auto* raw_cb = new FakeProgressCallback();
    ASSERT_EQ(raw_cb->RefCount(), 1u);

    ASSERT_EQ(fixture.service->AddCallback(raw_cb), DAS_S_OK);
    ASSERT_EQ(raw_cb->RefCount(), 2u); // service 持有 DasPtr（AddRef）

    ASSERT_EQ(fixture.service->RemoveCallback(raw_cb), DAS_S_OK);
    EXPECT_EQ(raw_cb->RefCount(), 1u); // 释放 proxy，引用回到 baseline

    // 重复 Remove 返回未找到
    EXPECT_EQ(fixture.service->RemoveCallback(raw_cb), DAS_E_NOT_FOUND);

    raw_cb->Release();
}

TEST(DasProgressServiceTest, KeyEmptyNotifiesAllCallbacks)
{
    ServiceFixture fixture;
    fixture.WireKeyEmpty();

    auto* raw_cb = new FakeProgressCallback();
    ASSERT_EQ(fixture.service->AddCallback(raw_cb), DAS_S_OK);

    auto session = std::make_shared<FakeSession>(fixture.ioc, "k1");
    fixture.hub->Register(session, "k1");
    ASSERT_EQ(fixture.hub->SubscriberCount("k1"), 1u);

    fixture.hub->Unregister(session.get(), "k1");

    ASSERT_EQ(fixture.hub->SubscriberCount("k1"), 0u);
    // key-empty 反向通知经 post 异步执行（不阻塞 strand 线程），poll 驱动。
    fixture.ioc.poll();
    ASSERT_EQ(raw_cb->DisconnectedKeys().size(), 1u);
    EXPECT_EQ(raw_cb->DisconnectedKeys()[0], "k1");

    fixture.service->RemoveCallback(raw_cb);
    raw_cb->Release();
}

TEST(DasProgressServiceTest, ReportSkipsBadFieldsWithoutCrashing)
{
    ServiceFixture fixture;
    auto session = std::make_shared<FakeSession>(fixture.ioc, "bad-key");
    fixture.hub->Register(session, "bad-key");

    // 坏字段：nullopt（GetFieldByIndex 越界）+ 非法值（JSON 对象）。
    std::vector<std::optional<FakeProgressUpdate::Field>> fields;
    fields.emplace_back(
        std::make_pair(std::string("goodField"), MakeJsonValue("ok")));
    fields.emplace_back(std::nullopt);
    fields.emplace_back(
        std::make_pair(
            std::string("badField"),
            MakeJsonDocumentValue(R"({"nested":1})")));

    auto entry = FakeProgressUpdate::Make("bad-key", std::move(fields));

    ASSERT_EQ(fixture.service->Report(entry.Get()), DAS_S_OK);
    fixture.ioc.poll();

    ASSERT_EQ(session->messages_.size(), 1u);
    auto parsed = ParsePayload(*session->messages_[0]);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->field_name, "goodField");
    EXPECT_EQ(parsed->value_text, "\"ok\"");
}

// 无订阅者时 Report 的帧不被任何人消费，但快照必须记录最后状态
// （订阅建立处重放的来源）。
TEST(DasProgressServiceTest, SnapshotRecordsFieldsReportedWithoutSubscribers)
{
    ServiceFixture fixture;

    std::vector<std::optional<FakeProgressUpdate::Field>> fields;
    fields.emplace_back(
        std::make_pair(
            std::string("updateProgress"),
            MakeJsonValue("发现新版本（v2.23.0）")));
    fields.emplace_back(
        std::make_pair(std::string("downloadRate"), MakeJsonValue("2.5 MB/s")));
    auto entry = FakeProgressUpdate::Make("snap-key", std::move(fields));

    ASSERT_EQ(fixture.service->Report(entry.Get()), DAS_S_OK);
    fixture.ioc.poll();

    const auto snapshot = fixture.service->TakeSnapshot("snap-key");
    ASSERT_EQ(snapshot.size(), 2u);
    // TakeSnapshot 按 fieldName 字典序返回。
    auto first = ParsePayload(*snapshot[0]);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->field_name, "downloadRate");
    EXPECT_EQ(first->value_text, "\"2.5 MB/s\"");

    auto second = ParsePayload(*snapshot[1]);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->field_name, "updateProgress");
    EXPECT_EQ(second->value_text, "\"发现新版本（v2.23.0）\"");
}

// 同 key 后续 Report 覆盖同名字段：快照只保留每字段的最后 payload。
TEST(DasProgressServiceTest, SnapshotMergesLatestPayloadPerField)
{
    ServiceFixture fixture;

    std::vector<std::optional<FakeProgressUpdate::Field>> first_fields;
    first_fields.emplace_back(
        std::make_pair(std::string("alpha"), MakeJsonValue("v1")));
    first_fields.emplace_back(
        std::make_pair(std::string("beta"), MakeJsonValue("b1")));
    auto first_entry =
        FakeProgressUpdate::Make("merge-key", std::move(first_fields));
    ASSERT_EQ(fixture.service->Report(first_entry.Get()), DAS_S_OK);

    std::vector<std::optional<FakeProgressUpdate::Field>> second_fields;
    second_fields.emplace_back(
        std::make_pair(std::string("alpha"), MakeJsonValue("v2")));
    auto second_entry =
        FakeProgressUpdate::Make("merge-key", std::move(second_fields));
    ASSERT_EQ(fixture.service->Report(second_entry.Get()), DAS_S_OK);
    fixture.ioc.poll();

    const auto snapshot = fixture.service->TakeSnapshot("merge-key");
    ASSERT_EQ(snapshot.size(), 2u);
    auto first = ParsePayload(*snapshot[0]);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->field_name, "alpha");
    EXPECT_EQ(first->value_text, "\"v2\"");

    auto second = ParsePayload(*snapshot[1]);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->field_name, "beta");
    EXPECT_EQ(second->value_text, "\"b1\"");
}

// key 订阅归零（key-empty 反向通知路径）逐出该 key 的快照——内存有界
// 的主路径。
TEST(DasProgressServiceTest, KeyEmptyEvictsSnapshot)
{
    ServiceFixture fixture;
    fixture.WireKeyEmpty();

    std::vector<std::optional<FakeProgressUpdate::Field>> fields;
    fields.emplace_back(
        std::make_pair(std::string("updateProgress"), MakeJsonValue("done")));
    auto entry = FakeProgressUpdate::Make("evict-key", std::move(fields));
    ASSERT_EQ(fixture.service->Report(entry.Get()), DAS_S_OK);
    ASSERT_FALSE(fixture.service->TakeSnapshot("evict-key").empty());

    auto session = std::make_shared<FakeSession>(fixture.ioc, "evict-key");
    fixture.hub->Register(session, "evict-key");
    fixture.hub->Unregister(session.get(), "evict-key");
    ASSERT_EQ(fixture.hub->SubscriberCount("evict-key"), 0u);
    // key-empty 反向通知经 post 异步执行（不阻塞 strand 线程），poll 驱动。
    fixture.ioc.poll();

    EXPECT_TRUE(fixture.service->TakeSnapshot("evict-key").empty());
}

// 拒绝谓词双信号：快照命中 OR 有活跃上报者（callbacks 非空）。后者保住
// check job 期间零 Report 零快照的合法首订阅不被误杀。
TEST(DasProgressServiceTest, IsKeyKnownUsesSnapshotOrCallbackDualSignal)
{
    ServiceFixture fixture;

    std::vector<std::optional<FakeProgressUpdate::Field>> fields;
    fields.emplace_back(
        std::make_pair(std::string("updateProgress"), MakeJsonValue("x")));
    auto entry = FakeProgressUpdate::Make("known-key", std::move(fields));
    ASSERT_EQ(fixture.service->Report(entry.Get()), DAS_S_OK);

    // 快照命中 → 已知。
    EXPECT_TRUE(fixture.service->IsKeyKnown("known-key"));
    // 无快照且无活跃上报者 → 拒绝。
    EXPECT_FALSE(fixture.service->IsKeyKnown("dead-key"));

    // 无快照但有活跃上报者 → 放行（受理路径 reporter Start 先于订阅）。
    auto* raw_cb = new FakeProgressCallback();
    ASSERT_EQ(fixture.service->AddCallback(raw_cb), DAS_S_OK);
    EXPECT_TRUE(fixture.service->IsKeyKnown("dead-key"));

    fixture.service->RemoveCallback(raw_cb);
    raw_cb->Release();
}

// 快照总量上限兜底：never-subscribed key 不触发 key-empty，FIFO 逐出
// 最旧保证快照集有界。
TEST(DasProgressServiceTest, SnapshotCapEvictsOldestKey)
{
    ServiceFixture fixture;

    constexpr auto total =
        Das::Http::DasProgressServiceImpl::MAX_SNAPSHOT_KEYS + 1;
    for (int index = 0; index < static_cast<int>(total); ++index)
    {
        std::vector<std::optional<FakeProgressUpdate::Field>> fields;
        fields.emplace_back(
            std::make_pair(
                std::string("updateProgress"),
                MakeJsonValue(std::to_string(index))));
        auto entry = FakeProgressUpdate::Make(
            "cap-key-" + std::to_string(index),
            std::move(fields));
        ASSERT_EQ(fixture.service->Report(entry.Get()), DAS_S_OK);
    }

    // 最旧 key 被逐出，最新 key 仍在。
    EXPECT_TRUE(fixture.service->TakeSnapshot("cap-key-0").empty());
    const auto newest =
        fixture.service->TakeSnapshot("cap-key-" + std::to_string(total - 1));
    ASSERT_EQ(newest.size(), 1u);
    auto parsed = ParsePayload(*newest[0]);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->value_text, "\"" + std::to_string(total - 1) + "\"");
}

// 快照重放端到端：受理期 Report（无订阅，广播丢失但快照已记）→ 新订阅
// 建立（App 同款门 + 注册回调接线）→ 订阅者先收重放帧再接直播帧。
TEST(DasProgressServiceTest, SnapshotReplayE2E_ReplayBeforeLive)
{
    ServiceFixture             fixture;
    boost::beast::test::stream client(fixture.ioc);
    boost::beast::test::stream server_stream(fixture.ioc);
    client.connect(server_stream);

    // 受理期推一帧：无人订阅，广播丢失，快照记录最后状态。
    std::vector<std::optional<FakeProgressUpdate::Field>> early_fields;
    early_fields.emplace_back(
        std::make_pair(
            std::string("updateProgress"),
            MakeJsonValue("检查失败：网络错误")));
    auto early_entry =
        FakeProgressUpdate::Make("e2e-key", std::move(early_fields));
    ASSERT_EQ(fixture.service->Report(early_entry.Get()), DAS_S_OK);
    fixture.ioc.poll();

    // App.cpp 同款接线（weak_service 捕获形态照抄）。
    const std::weak_ptr<Das::Http::DasProgressServiceImpl> weak_service =
        fixture.service;
    const auto key_gate = [weak_service](const std::string& key)
    {
        if (auto service = weak_service.lock())
        {
            return service->IsKeyKnown(key);
        }
        return true;
    };
    const auto on_registered =
        [weak_service](Das::Http::Beast::SseSessionBase& session)
    {
        if (auto service = weak_service.lock())
        {
            for (const auto& payload : service->TakeSnapshot(session.Key()))
            {
                session.WriteData(payload);
            }
        }
    };

    // 快照命中 → 门放行，订阅建立（真实 SSE 会话走头写 → 注册 → 重放链）。
    ASSERT_TRUE(key_gate("e2e-key"));
    using TestSseSession =
        Das::Http::Beast::SseSessionT<boost::beast::test::stream>;
    auto session = std::make_shared<TestSseSession>(
        std::move(server_stream),
        "e2e-key",
        fixture.hub,
        std::chrono::seconds(15),
        on_registered);
    session->DoStart();
    PumpIo(fixture.ioc);
    ASSERT_EQ(fixture.hub->SubscriberCount("e2e-key"), 1u);

    // 注册后推直播帧。
    std::vector<std::optional<FakeProgressUpdate::Field>> live_fields;
    live_fields.emplace_back(
        std::make_pair(
            std::string("updateProgress"),
            MakeJsonValue("已是最新（v2.23.0）")));
    auto live_entry =
        FakeProgressUpdate::Make("e2e-key", std::move(live_fields));
    ASSERT_EQ(fixture.service->Report(live_entry.Get()), DAS_S_OK);
    PumpIo(fixture.ioc);

    // 客户端字节流：200 头 → 重放帧（受理期快照）→ 直播帧。
    const std::string data(client.str());
    const auto        header_pos = data.find("HTTP/1.1 200 OK");
    const auto        replay_pos = data.find("检查失败：网络错误");
    const auto        live_pos = data.find("已是最新（v2.23.0）");
    ASSERT_NE(header_pos, std::string::npos);
    ASSERT_NE(replay_pos, std::string::npos);
    ASSERT_NE(live_pos, std::string::npos);
    EXPECT_LT(header_pos, replay_pos);
    EXPECT_LT(replay_pos, live_pos);

    session->Close();
    PumpIo(fixture.ioc);
}

// 无效 key 拒绝端到端：App 同款门谓词对无快照且无活跃上报者的 key 返回
// false（Server 层落普通路由 404）；上报者存在时放行（合法首订阅不误杀）。
TEST(DasProgressServiceTest, UnknownKeyGateRejectionE2E)
{
    ServiceFixture fixture;

    const std::weak_ptr<Das::Http::DasProgressServiceImpl> weak_service =
        fixture.service;
    const auto key_gate = [weak_service](const std::string& key)
    {
        if (auto service = weak_service.lock())
        {
            return service->IsKeyKnown(key);
        }
        return true;
    };

    // 无快照、无活跃上报者 → 拒绝（订了个死 key 的可诊断信号）。
    EXPECT_FALSE(key_gate("never-reported-key"));

    // 上报者存在 → 放行（check job 期间零 Report 零快照的首订阅）。
    auto* raw_cb = new FakeProgressCallback();
    ASSERT_EQ(fixture.service->AddCallback(raw_cb), DAS_S_OK);
    EXPECT_TRUE(key_gate("never-reported-key"));

    fixture.service->RemoveCallback(raw_cb);
    raw_cb->Release();
}
