#include "../src/DasLocaleServiceImpl.h"
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasLocaleCallback.Implements.hpp>
#include <gtest/gtest.h>

#include <mutex>
#include <string>

namespace
{
    // 假 locale callback：记录最近收到的 locale 与被调用次数（线程安全）。
    // 经 autogen CRTP 基类继承 IDasLocaleCallback（提供 QueryInterface/AddRef/
    // Release），仅 override OnDefaultLocaleChanged。
    class FakeLocaleCallback final
        : public Das::ExportInterface::DasLocaleCallbackImplBase<
              FakeLocaleCallback>
    {
    public:
        DAS_IMPL OnDefaultLocaleChanged(IDasReadOnlyString* new_locale) override
        {
            if (new_locale)
            {
                const char* utf8 = nullptr;
                new_locale->GetUtf8(&utf8);
                const std::unique_lock lock(mutex_);
                last_locale_ =
                    utf8 != nullptr ? std::string(utf8) : std::string{};
                ++call_count_;
            }
            return DAS_S_OK;
        }

        std::string LastLocale()
        {
            const std::unique_lock lock(mutex_);
            return last_locale_;
        }

        size_t CallCount()
        {
            const std::unique_lock lock(mutex_);
            return call_count_;
        }

    private:
        std::mutex  mutex_;
        std::string last_locale_;
        size_t      call_count_ = 0;
    };

    // 经 C 工厂构造只读 IDasReadOnlyString（AddRef 后移交 DasPtr）。
    DAS::DasPtr<IDasReadOnlyString> MakeUtf8String(const char* utf8)
    {
        DAS::DasPtr<IDasReadOnlyString> p;
        ::CreateIDasReadOnlyStringFromUtf8(utf8, p.Put());
        return p;
    }

    std::string ToUtf8(IDasReadOnlyString* p)
    {
        if (p == nullptr)
        {
            return {};
        }
        const char* out = nullptr;
        EXPECT_EQ(p->GetUtf8(&out), DAS_S_OK);
        return out != nullptr ? std::string(out) : std::string{};
    }

    // 进程级 GlobalLocale 是跨测试共享状态：SetDefaultLocale 用例会修改它。
    // 本夹具在 SetUp 保存、TearDown 还原，保证直接跑整二进制（非 ctest 分
    // 进程）时 GetDefaultLocale 默认值用例不被前序用例污染。
    class DasLocaleServiceImplTest : public ::testing::Test
    {
    protected:
        DAS::DasPtr<IDasReadOnlyString>                    saved_locale_;
        Das::DasPtr<Das::Core::i18n::DasLocaleServiceImpl> service_;

        void SetUp() override
        {
            ::DasGetDefaultLocale(saved_locale_.Put());
            service_ = DAS::MakeDasPtr<Das::Core::i18n::DasLocaleServiceImpl>();
            ASSERT_NE(service_.Get(), nullptr);
        }

        void TearDown() override
        {
            service_.Reset();
            if (saved_locale_)
            {
                ::DasSetDefaultLocale(saved_locale_.Get());
            }
        }
    };

} // namespace

// ---- AddCallback ----

TEST_F(DasLocaleServiceImplTest, AddCallbackNullReturnsError)
{
    EXPECT_NE(service_->AddCallback(nullptr), DAS_S_OK);
}

TEST_F(DasLocaleServiceImplTest, AddCallbackReturnsOk)
{
    auto fake = DAS::MakeDasPtr<FakeLocaleCallback>();
    ASSERT_NE(fake.Get(), nullptr);
    EXPECT_EQ(service_->AddCallback(fake.Get()), DAS_S_OK);
}

// ---- RemoveCallback ----

TEST_F(DasLocaleServiceImplTest, RemoveCallbackNullReturnsError)
{
    EXPECT_NE(service_->RemoveCallback(nullptr), DAS_S_OK);
}

TEST_F(DasLocaleServiceImplTest, RemoveCallbackNotFoundReturnsError)
{
    auto fake = DAS::MakeDasPtr<FakeLocaleCallback>();
    ASSERT_NE(fake.Get(), nullptr);
    EXPECT_EQ(service_->RemoveCallback(fake.Get()), DAS_E_NOT_FOUND);
}

TEST_F(DasLocaleServiceImplTest, RemoveCallbackAfterAddReturnsOk)
{
    auto fake = DAS::MakeDasPtr<FakeLocaleCallback>();
    ASSERT_EQ(service_->AddCallback(fake.Get()), DAS_S_OK);
    EXPECT_EQ(service_->RemoveCallback(fake.Get()), DAS_S_OK);
}

// ---- GetDefaultLocale ----

TEST_F(DasLocaleServiceImplTest, GetDefaultLocaleReturnsGlobalDefault)
{
    DAS::DasPtr<IDasReadOnlyString> locale;
    EXPECT_EQ(service_->GetDefaultLocale(locale.Put()), DAS_S_OK);
    ASSERT_NE(locale.Get(), nullptr);
    EXPECT_EQ(ToUtf8(locale.Get()), "en");
}

TEST_F(DasLocaleServiceImplTest, GetDefaultLocaleNullOutReturnsError)
{
    EXPECT_NE(service_->GetDefaultLocale(nullptr), DAS_S_OK);
}

// ---- SetDefaultLocale ----

TEST_F(DasLocaleServiceImplTest, SetDefaultLocaleNullReturnsError)
{
    EXPECT_NE(service_->SetDefaultLocale(nullptr), DAS_S_OK);
}

TEST_F(DasLocaleServiceImplTest, SetDefaultLocaleUpdatesGlobalLocale)
{
    const auto zh = MakeUtf8String("zh-CN");
    ASSERT_NE(zh.Get(), nullptr);
    EXPECT_EQ(service_->SetDefaultLocale(zh.Get()), DAS_S_OK);

    DAS::DasPtr<IDasReadOnlyString> current;
    EXPECT_EQ(::DasGetDefaultLocale(current.Put()), DAS_S_OK);
    EXPECT_EQ(ToUtf8(current.Get()), "zh-CN");
}

TEST_F(DasLocaleServiceImplTest, SetDefaultLocaleNotifiesRegisteredCallback)
{
    auto fake = DAS::MakeDasPtr<FakeLocaleCallback>();
    ASSERT_EQ(service_->AddCallback(fake.Get()), DAS_S_OK);

    const auto fr = MakeUtf8String("fr");
    ASSERT_EQ(service_->SetDefaultLocale(fr.Get()), DAS_S_OK);

    EXPECT_EQ(fake->CallCount(), 1u);
    EXPECT_EQ(fake->LastLocale(), "fr");
}

TEST_F(DasLocaleServiceImplTest, SetDefaultLocaleNotifiesMultipleCallbacks)
{
    auto fake_a = DAS::MakeDasPtr<FakeLocaleCallback>();
    auto fake_b = DAS::MakeDasPtr<FakeLocaleCallback>();
    ASSERT_EQ(service_->AddCallback(fake_a.Get()), DAS_S_OK);
    ASSERT_EQ(service_->AddCallback(fake_b.Get()), DAS_S_OK);

    const auto ja = MakeUtf8String("ja");
    ASSERT_EQ(service_->SetDefaultLocale(ja.Get()), DAS_S_OK);

    EXPECT_EQ(fake_a->CallCount(), 1u);
    EXPECT_EQ(fake_a->LastLocale(), "ja");
    EXPECT_EQ(fake_b->CallCount(), 1u);
    EXPECT_EQ(fake_b->LastLocale(), "ja");
}

TEST_F(DasLocaleServiceImplTest, RemoveCallbackStopsFurtherNotification)
{
    auto fake = DAS::MakeDasPtr<FakeLocaleCallback>();
    ASSERT_EQ(service_->AddCallback(fake.Get()), DAS_S_OK);
    ASSERT_EQ(service_->RemoveCallback(fake.Get()), DAS_S_OK);

    const auto de = MakeUtf8String("de");
    ASSERT_EQ(service_->SetDefaultLocale(de.Get()), DAS_S_OK);

    EXPECT_EQ(fake->CallCount(), 0u);
}
