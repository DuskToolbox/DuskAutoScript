#include <das/Core/ForeignInterfaceHost/ErrorLensManager.h>
#include <das/Core/IPC/MainProcess/IIpcContext.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/IDasAsyncCallback.h>
#include <das/_autogen/idl/abi/IDasTypeInfo.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasErrorLens.Implements.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using Das::DasPtr;
using DAS::Core::ForeignInterfaceHost::ClearActiveErrorLensManager;
using DAS::Core::ForeignInterfaceHost::ErrorLensManager;
using DAS::Core::ForeignInterfaceHost::SetActiveErrorLensManager;
using DAS::Core::IPC::MainProcess::IIpcContext;
using Das::PluginInterface::DasErrorLensImplBase;
using Das::PluginInterface::IDasErrorLens;
using namespace std::chrono_literals;

namespace
{
    DasGuid MakeTestGuid(uint32_t value)
    {
        DasGuid guid{};
        guid.data1 = 0x68330000u | value;
        guid.data2 = 0x6833;
        guid.data3 = 0x4000;
        guid.data4[0] = 0x80;
        guid.data4[7] = static_cast<uint8_t>(value & 0xFFu);
        return guid;
    }

    std::string ToUtf8(IDasReadOnlyString* p_string)
    {
        if (p_string == nullptr)
        {
            return {};
        }

        const char* text = nullptr;
        EXPECT_EQ(p_string->GetUtf8(&text), DAS_S_OK);
        return text != nullptr ? std::string{text} : std::string{};
    }

    class FakeTypeInfo final : public IDasTypeInfo
    {
    public:
        explicit FakeTypeInfo(DasGuid guid) : guid_(guid) {}

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override { return --ref_count_; }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_object) override
        {
            if (pp_object == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DasIidOf<IDasBase>() || iid == DasIidOf<IDasTypeInfo>())
            {
                *pp_object = static_cast<IDasTypeInfo*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            ++get_guid_calls;
            if (p_out_guid == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_out_guid = guid_;
            return get_guid_result;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            return ::CreateIDasReadOnlyStringFromUtf8(
                "FakeTypeInfo",
                pp_out_name);
        }

        std::atomic<int> get_guid_calls{0};
        DasResult        get_guid_result = DAS_S_OK;

    private:
        DasGuid               guid_{};
        std::atomic<uint32_t> ref_count_{0};
    };

    class FakeErrorLens final : public DasErrorLensImplBase<FakeErrorLens>
    {
    public:
        std::vector<DasGuid>                       supported_iids;
        std::unordered_map<DasResult, std::string> messages;
        std::atomic<int>                           get_error_message_calls{0};
        DasResult   miss_result = DAS_E_OUT_OF_RANGE;
        std::string last_locale;

        DasResult DAS_STD_CALL GetSupportedIids(
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

        DasResult DAS_STD_CALL GetErrorMessage(
            IDasReadOnlyString*  locale_name,
            DasResult            error_code,
            IDasReadOnlyString** pp_out_message) override
        {
            if (pp_out_message == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_message = nullptr;

            ++get_error_message_calls;
            last_locale = ToUtf8(locale_name);

            const auto it = messages.find(error_code);
            if (it == messages.end())
            {
                return miss_result;
            }

            return ::CreateIDasReadOnlyStringFromUtf8(
                it->second.c_str(),
                pp_out_message);
        }
    };

    class BusinessThreadCallback final : public IDasAsyncCallback
    {
    public:
        explicit BusinessThreadCallback(std::function<void()> callback)
            : callback_(std::move(callback))
        {
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

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_object) override
        {
            if (pp_object == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DasIidOf<IDasBase>()
                || iid == DasIidOf<IDasAsyncCallback>())
            {
                *pp_object = static_cast<IDasAsyncCallback*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_object = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL Do() noexcept override
        {
            callback_();
            return DAS_S_OK;
        }

    private:
        std::atomic<uint32_t> ref_count_{0};
        std::function<void()> callback_;
    };

    class GlobalErrorMessagesTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            SetActiveErrorLensManager(&manager_);
            ipc_context_ =
                DAS::Core::IPC::MainProcess::CreateIpcContextShared(false);
            ASSERT_NE(ipc_context_, nullptr);
        }

        void TearDown() override
        {
            ipc_context_.reset();
            ClearActiveErrorLensManager(&manager_);
        }

        DasPtr<IDasErrorLens> RegisterLens(
            FakeErrorLens* const lens,
            const DasGuid&       provider_guid)
        {
            lens->supported_iids = {provider_guid};

            DasPtr<Das::ExportInterface::IDasReadOnlyGuidVector> supported;
            EXPECT_EQ(lens->GetSupportedIids(supported.Put()), DAS_S_OK);
            EXPECT_EQ(manager_.Register(supported.Get(), lens), DAS_S_OK);

            return DasPtr<IDasErrorLens>::Attach(
                static_cast<IDasErrorLens*>(lens));
        }

        void RunOnBusinessThread(const std::function<void()>& callback)
        {
            struct State
            {
                std::promise<void> done;
                std::exception_ptr exception;
            };

            auto state = std::make_shared<State>();
            auto done = state->done.get_future();

            auto* bt_callback = new BusinessThreadCallback(
                [state, callback]
                {
                    try
                    {
                        callback();
                    }
                    catch (...)
                    {
                        state->exception = std::current_exception();
                    }
                    state->done.set_value();
                });

            ipc_context_->PostToBusinessThread(bt_callback);

            ASSERT_EQ(done.wait_for(2s), std::future_status::ready);
            done.get();
            if (state->exception)
            {
                std::rethrow_exception(state->exception);
            }
        }

        ErrorLensManager             manager_;
        std::shared_ptr<IIpcContext> ipc_context_;
    };

    TEST_F(GlobalErrorMessagesTest, PluginExactMessageWins)
    {
        const auto provider_guid = MakeTestGuid(1);
        auto*      lens = FakeErrorLens::MakeRaw();
        lens->messages[DAS_E_FAIL] = "plugin failure text";
        auto         lens_guard = RegisterLens(lens, provider_guid);
        FakeTypeInfo type_info{provider_guid};

        DasResult   result = DAS_E_FAIL;
        std::string message;
        RunOnBusinessThread(
            [&]
            {
                DasPtr<IDasReadOnlyString> out_message;
                result = ::DasGetErrorMessage(
                    &type_info,
                    DAS_E_FAIL,
                    out_message.Put());
                message = ToUtf8(out_message.Get());
            });

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_EQ(message, "plugin failure text");
        EXPECT_EQ(lens->get_error_message_calls.load(), 1);
        EXPECT_EQ(type_info.get_guid_calls.load(), 1);
    }

    TEST_F(GlobalErrorMessagesTest, PluginMissFallsBackToPredefined)
    {
        const auto   provider_guid = MakeTestGuid(2);
        auto*        lens = FakeErrorLens::MakeRaw();
        auto         lens_guard = RegisterLens(lens, provider_guid);
        FakeTypeInfo type_info{provider_guid};

        DasResult   result = DAS_E_FAIL;
        std::string message;
        RunOnBusinessThread(
            [&]
            {
                DasPtr<IDasReadOnlyString> out_message;
                result = ::DasGetErrorMessage(
                    &type_info,
                    DAS_E_FAIL,
                    out_message.Put());
                message = ToUtf8(out_message.Get());
            });

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_EQ(message, "Operation failed");
        EXPECT_EQ(lens->get_error_message_calls.load(), 1);
    }

    TEST_F(GlobalErrorMessagesTest, UnknownCodeFallsBackToGeneric)
    {
        constexpr DasResult kUnknownErrorCode =
            static_cast<DasResult>(-12345678);
        const auto   provider_guid = MakeTestGuid(3);
        auto*        lens = FakeErrorLens::MakeRaw();
        auto         lens_guard = RegisterLens(lens, provider_guid);
        FakeTypeInfo type_info{provider_guid};

        DasResult   result = DAS_E_FAIL;
        std::string message;
        RunOnBusinessThread(
            [&]
            {
                DasPtr<IDasReadOnlyString> out_message;
                result = ::DasGetErrorMessage(
                    &type_info,
                    kUnknownErrorCode,
                    out_message.Put());
                message = ToUtf8(out_message.Get());
            });

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_EQ(message, "Unknown error");
        EXPECT_EQ(lens->get_error_message_calls.load(), 1);
    }

    TEST_F(GlobalErrorMessagesTest, UsesDefaultLocaleForLensLookup)
    {
        const auto provider_guid = MakeTestGuid(4);
        auto*      lens = FakeErrorLens::MakeRaw();
        lens->messages[DAS_E_FAIL] = "localized plugin text";
        auto         lens_guard = RegisterLens(lens, provider_guid);
        FakeTypeInfo type_info{provider_guid};

        DasPtr<IDasReadOnlyString> expected_locale_string;
        ASSERT_EQ(
            ::DasGetDefaultLocale(expected_locale_string.Put()),
            DAS_S_OK);
        const auto expected_locale = ToUtf8(expected_locale_string.Get());

        RunOnBusinessThread(
            [&]
            {
                DasPtr<IDasReadOnlyString> out_message;
                EXPECT_EQ(
                    ::DasGetErrorMessage(
                        &type_info,
                        DAS_E_FAIL,
                        out_message.Put()),
                    DAS_S_OK);
            });

        EXPECT_EQ(lens->last_locale, expected_locale);
    }

    TEST_F(GlobalErrorMessagesTest, BusinessThreadDirectCallInvokesLens)
    {
        const auto provider_guid = MakeTestGuid(5);
        auto*      lens = FakeErrorLens::MakeRaw();
        lens->messages[DAS_E_FAIL] = "business thread plugin text";
        auto         lens_guard = RegisterLens(lens, provider_guid);
        FakeTypeInfo type_info{provider_guid};

        DasResult result = DAS_E_FAIL;
        RunOnBusinessThread(
            [&]
            {
                DasPtr<IDasReadOnlyString> out_message;
                result = ::DasGetErrorMessage(
                    &type_info,
                    DAS_E_FAIL,
                    out_message.Put());
            });

        EXPECT_EQ(result, DAS_S_OK);
        EXPECT_EQ(lens->get_error_message_calls.load(), 1);
    }

    TEST_F(
        GlobalErrorMessagesTest,
        NonBusinessThreadReturnsUnexpectedThreadDetected)
    {
        const auto provider_guid = MakeTestGuid(6);
        auto*      lens = FakeErrorLens::MakeRaw();
        lens->messages[DAS_E_FAIL] = "must not be read";
        auto         lens_guard = RegisterLens(lens, provider_guid);
        FakeTypeInfo type_info{provider_guid};

        DasPtr<IDasReadOnlyString> out_message;
        const auto                 result =
            ::DasGetErrorMessage(&type_info, DAS_E_FAIL, out_message.Put());

        EXPECT_EQ(result, DAS_E_UNEXPECTED_THREAD_DETECTED);
        EXPECT_EQ(lens->get_error_message_calls.load(), 0);
        EXPECT_EQ(type_info.get_guid_calls.load(), 0);
    }
} // namespace
