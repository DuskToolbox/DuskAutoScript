#include <das/Core/ForeignInterfaceHost/ErrorLensManager.h>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasErrorLens.Implements.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace DAS::Core::ForeignInterfaceHost;
using Das::DasPtr;
using namespace Das::PluginInterface;
using namespace std::chrono_literals;

namespace
{
    DasGuid MakeTestGuid(uint32_t value)
    {
        DasGuid guid{};
        guid.data1 = 0x68300000u | value;
        guid.data2 = 0x6830;
        guid.data3 = 0x4000;
        guid.data4[0] = 0x80;
        guid.data4[7] = static_cast<uint8_t>(value & 0xFFu);
        return guid;
    }

    class FakeErrorLens final : public DasErrorLensImplBase<FakeErrorLens>
    {
    public:
        std::vector<DasGuid> supported_iids;
        std::atomic<int>     get_error_message_calls{0};
        std::promise<void>*  entered_get_error_message = nullptr;
        std::atomic<bool>*   allow_get_error_message_return = nullptr;
        DasResult            get_error_message_result = DAS_E_OUT_OF_RANGE;

        DasResult GetSupportedIids(
            Das::ExportInterface::IDasReadOnlyGuidVector** pp_out_iids) override
        {
            DasPtr<Das::ExportInterface::IDasGuidVector> writable_iids;
            const auto create_result = ::CreateIDasGuidVector(
                supported_iids.data(),
                supported_iids.size(),
                writable_iids.Put());
            if (DAS::IsFailed(create_result))
            {
                return create_result;
            }

            return writable_iids->ToConst(pp_out_iids);
        }

        DasResult GetErrorMessage(
            IDasReadOnlyString*,
            DasResult,
            IDasReadOnlyString** out_string) override
        {
            if (out_string != nullptr)
            {
                *out_string = nullptr;
            }

            ++get_error_message_calls;
            if (entered_get_error_message != nullptr)
            {
                entered_get_error_message->set_value();
            }
            while (allow_get_error_message_return != nullptr
                   && !allow_get_error_message_return->load())
            {
                std::this_thread::sleep_for(1ms);
            }
            return get_error_message_result;
        }
    };

    // 按码返回固定文案的 lens：命中返回消息、miss 返 OUT_OF_RANGE——
    // 驱动扫描式按码查询（caller 只知数值码，无 type guid 路由）。
    class CodeMessageFakeLens final
        : public DasErrorLensImplBase<CodeMessageFakeLens>
    {
    public:
        std::unordered_map<DasResult, std::string> messages;
        std::vector<DasGuid>                       supported_iids;

        DasResult GetSupportedIids(
            Das::ExportInterface::IDasReadOnlyGuidVector** pp_out_iids) override
        {
            DasPtr<Das::ExportInterface::IDasGuidVector> writable_iids;
            const auto create_result = ::CreateIDasGuidVector(
                supported_iids.data(),
                supported_iids.size(),
                writable_iids.Put());
            if (DAS::IsFailed(create_result))
            {
                return create_result;
            }

            return writable_iids->ToConst(pp_out_iids);
        }

        DasResult GetErrorMessage(
            IDasReadOnlyString*,
            DasResult            error_code,
            IDasReadOnlyString** out_string) override
        {
            if (out_string != nullptr)
            {
                *out_string = nullptr;
            }
            const auto it = messages.find(error_code);
            if (it == messages.end())
            {
                return DAS_E_OUT_OF_RANGE;
            }
            return ::CreateIDasReadOnlyStringFromUtf8(
                it->second.c_str(),
                out_string);
        }
    };

    template <typename TLens>
    FeatureInfo MakeFeature(TLens& lens)
    {
        FeatureInfo feat{};
        feat.feature_type = DAS_PLUGIN_FEATURE_ERROR_LENS;
        feat.interface_ptr = DasPtr<IDasBase>(static_cast<IDasBase*>(&lens));
        return feat;
    }

    class ErrorLensManagerTest : public ::testing::Test
    {
    protected:
        ErrorLensManager mgr;
    };

    TEST_F(ErrorLensManagerTest, RegistersPluginGuidOutsideOfficialIids)
    {
        const auto plugin_guid = MakeTestGuid(1);
        const auto provider_guid = MakeTestGuid(2);
        auto*      lens = FakeErrorLens::MakeRaw();
        lens->supported_iids = {provider_guid};

        auto         feat = MakeFeature(*lens);
        FeatureInfo* fp = &feat;
        ASSERT_EQ(mgr.OnPluginLoaded(plugin_guid, {&fp, 1}), DAS_S_OK);

        DasPtr<IDasErrorLens> found;
        EXPECT_EQ(mgr.FindInterface(provider_guid, found.Put()), DAS_S_OK);
        EXPECT_EQ(found.Get(), static_cast<IDasErrorLens*>(lens));

        lens->Release();
    }

    TEST_F(ErrorLensManagerTest, OnPluginUnloadingClearsRoutes)
    {
        const auto plugin_guid = MakeTestGuid(3);
        const auto provider_guid = MakeTestGuid(4);
        auto*      lens = FakeErrorLens::MakeRaw();
        lens->supported_iids = {provider_guid};

        auto         feat = MakeFeature(*lens);
        FeatureInfo* fp = &feat;
        ASSERT_EQ(mgr.OnPluginLoaded(plugin_guid, {&fp, 1}), DAS_S_OK);

        DasPtr<IDasErrorLens> found;
        ASSERT_EQ(mgr.FindInterface(provider_guid, found.Put()), DAS_S_OK);

        ASSERT_EQ(mgr.OnPluginUnloading(plugin_guid), DAS_S_OK);

        DasPtr<IDasErrorLens> after_unload;
        EXPECT_EQ(
            mgr.FindInterface(provider_guid, after_unload.Put()),
            DAS_E_NO_INTERFACE);

        lens->Release();
    }

    TEST_F(ErrorLensManagerTest, DoesNotHoldManagerLockAcrossLensCall)
    {
        const auto plugin_guid = MakeTestGuid(5);
        const auto provider_guid = MakeTestGuid(6);
        auto*      lens = FakeErrorLens::MakeRaw();
        lens->supported_iids = {provider_guid};

        std::promise<void> entered_get_error_message;
        auto entered_future = entered_get_error_message.get_future();
        std::atomic<bool> allow_get_error_message_return{false};
        lens->entered_get_error_message = &entered_get_error_message;
        lens->allow_get_error_message_return = &allow_get_error_message_return;

        auto         feat = MakeFeature(*lens);
        FeatureInfo* fp = &feat;
        ASSERT_EQ(mgr.OnPluginLoaded(plugin_guid, {&fp, 1}), DAS_S_OK);

        std::thread lookup_thread(
            [&]
            {
                static_cast<void>(
                    mgr.GetErrorMessage(provider_guid, nullptr, DAS_E_FAIL));
            });

        ASSERT_EQ(entered_future.wait_for(1s), std::future_status::ready);

        auto unload_future = std::async(
            std::launch::async,
            [&] { return mgr.OnPluginUnloading(plugin_guid); });
        EXPECT_EQ(unload_future.wait_for(200ms), std::future_status::ready);

        allow_get_error_message_return.store(true);
        lookup_thread.join();

        EXPECT_EQ(unload_future.get(), DAS_S_OK);
        EXPECT_EQ(lens->get_error_message_calls.load(), 1);

        DasPtr<IDasErrorLens> after_unload;
        EXPECT_EQ(
            mgr.FindInterface(provider_guid, after_unload.Put()),
            DAS_E_NO_INTERFACE);

        lens->Release();
    }

    // 扫描式按码查询：两个不同 plugin 域的 lens 各自解释不同码——无
    // type guid 路由时横扫全部注册域，各自码返回各自文案、未注册码
    // 全 miss 回落（调用方接 Core 预定义表）。
    TEST_F(ErrorLensManagerTest, GetErrorMessageForCodeScansAllDomains)
    {
        const auto plugin_guid_a = MakeTestGuid(7);
        const auto domain_a = MakeTestGuid(8);
        const auto plugin_guid_b = MakeTestGuid(9);
        const auto domain_b = MakeTestGuid(10);

        auto* lens_a = CodeMessageFakeLens::MakeRaw();
        lens_a->supported_iids = {domain_a};
        lens_a->messages[DAS_E_FAIL] = "domain A explains fail";

        auto* lens_b = CodeMessageFakeLens::MakeRaw();
        lens_b->supported_iids = {domain_b};
        lens_b->messages[DAS_E_TIMEOUT] = "domain B explains timeout";

        auto         feature_a = MakeFeature(*lens_a);
        auto         feature_b = MakeFeature(*lens_b);
        FeatureInfo* features[] = {&feature_a, &feature_b};
        ASSERT_EQ(mgr.OnPluginLoaded(plugin_guid_a, {features, 1}), DAS_S_OK);
        ASSERT_EQ(
            mgr.OnPluginLoaded(plugin_guid_b, {features + 1, 1}),
            DAS_S_OK);

        const auto read_text =
            [](const DasPtr<IDasReadOnlyString>& text) -> std::string
        {
            if (!text)
            {
                return {};
            }
            const char* utf8 = nullptr;
            if (DAS::IsFailed(text->GetUtf8(&utf8)) || utf8 == nullptr)
            {
                return {};
            }
            return utf8;
        };

        auto message_a = mgr.GetErrorMessageForCode(nullptr, DAS_E_FAIL);
        ASSERT_TRUE(message_a.has_value());
        EXPECT_EQ(read_text(message_a.value()), "domain A explains fail");

        auto message_b = mgr.GetErrorMessageForCode(nullptr, DAS_E_TIMEOUT);
        ASSERT_TRUE(message_b.has_value());
        EXPECT_EQ(read_text(message_b.value()), "domain B explains timeout");

        // 未注册码：全部域 miss → 错误返回，调用方回落预定义表。
        auto message_miss =
            mgr.GetErrorMessageForCode(nullptr, DAS_E_ACCESS_DENIED);
        EXPECT_FALSE(message_miss.has_value());

        lens_a->Release();
        lens_b->Release();
    }
} // namespace
