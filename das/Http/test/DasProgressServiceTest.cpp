#include "SseHub.hpp"
#include "beast/SseSession.hpp" // for SseSessionBase
#include "service/DasProgressServiceImpl.h"

#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProgressCallback.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasProgressUpdate.Implements.hpp>
#include <gtest/gtest.h>

#include <atomic>
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
