#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/_autogen/idl/abi/IDasErrorLens.h>
#include <das/_autogen/idl/abi/IDasGuidVector.h>
#include <gtest/gtest.h>

using Das::DasPtr;
using namespace Das::PluginInterface;

namespace
{
    DasGuid MakeTestGuid(uint32_t value)
    {
        DasGuid guid{};
        guid.data1 = 0x68310000u | value;
        guid.data2 = 0x6831;
        guid.data3 = 0x4000;
        guid.data4[0] = 0x80;
        guid.data4[7] = static_cast<uint8_t>(value & 0xFFu);
        return guid;
    }

    DasPtr<IDasReadOnlyString> MakeString(const char* value)
    {
        DasPtr<IDasReadOnlyString> result;
        EXPECT_EQ(
            ::CreateIDasReadOnlyStringFromUtf8(value, result.Put()),
            DAS_S_OK);
        return result;
    }

    DasPtr<IDasBasicErrorLens> MakeLensWithSupportedGuid()
    {
        DasPtr<IDasBasicErrorLens> lens;
        EXPECT_EQ(::CreateIDasBasicErrorLens(lens.Put()), DAS_S_OK);

        DasPtr<Das::ExportInterface::IDasGuidVector> supported_iids;
        EXPECT_EQ(lens->GetWritableSupportedIids(supported_iids.Put()), DAS_S_OK);
        EXPECT_EQ(supported_iids->PushBack(MakeTestGuid(1)), DAS_S_OK);
        return lens;
    }
} // namespace

TEST(IDasBasicErrorLensTest, MissingMessageReturnsOutOfRange)
{
    auto lens = MakeLensWithSupportedGuid();
    ASSERT_TRUE(lens);

    auto locale = MakeString("en-US");
    ASSERT_TRUE(locale);

    auto* out = reinterpret_cast<IDasReadOnlyString*>(0x1);
    EXPECT_EQ(
        lens->GetErrorMessage(locale.Get(), DAS_E_FAIL, &out),
        DAS_E_OUT_OF_RANGE);
    EXPECT_EQ(out, nullptr);
}

TEST(IDasBasicErrorLensTest, RegisteredMessageReturnsString)
{
    auto lens = MakeLensWithSupportedGuid();
    ASSERT_TRUE(lens);

    auto locale = MakeString("en-US");
    auto message = MakeString("specific failure");
    ASSERT_TRUE(locale);
    ASSERT_TRUE(message);
    ASSERT_EQ(
        lens->RegisterErrorMessage(locale.Get(), DAS_E_FAIL, message.Get()),
        DAS_S_OK);

    DasPtr<IDasReadOnlyString> out;
    ASSERT_EQ(
        lens->GetErrorMessage(locale.Get(), DAS_E_FAIL, out.Put()),
        DAS_S_OK);
    ASSERT_TRUE(out);

    const char* text = nullptr;
    ASSERT_EQ(out->GetUtf8(&text), DAS_S_OK);
    EXPECT_STREQ(text, "specific failure");
}
