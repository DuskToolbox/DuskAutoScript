#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasImage.h>
#include <das/_autogen/idl/abi/IDasVariantVector.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasImage.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasVariantVector.hpp>
#include <gtest/gtest.h>

extern DasResult CreateIDasVariantVector(
    Das::ExportInterface::IDasVariantVector** pp_out_vector);

namespace
{
    class TestImageStub final
        : public Das::ExportInterface::DasImageImplBase<TestImageStub>
    {
    public:
        DAS_IMPL GetSize(Das::ExportInterface::DasSize* p_out_size) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }
        DAS_IMPL GetChannelCount(int32_t* p_out_channel_count) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }
        DAS_IMPL Clip(
            const Das::ExportInterface::DasRect* p_rect,
            Das::ExportInterface::IDasImage**    p_out_image) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }
        DAS_IMPL GetDataSize(uint64_t* p_out_size) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }
        DAS_IMPL GetBinaryBuffer(
            Das::ExportInterface::IDasBinaryBuffer** pp_out_buffer) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }
        DAS_IMPL GetPixelFormat(
            ::Das::ExportInterface::DasImagePixelFormat* p_out_format) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }
    };
} // namespace

// Test 1: Enum values
static_assert(
    Das::ExportInterface::DAS_VARIANT_TYPE_IMAGE == 6,
    "DAS_VARIANT_TYPE_IMAGE must be 6");
static_assert(
    Das::ExportInterface::DAS_VARIANT_TYPE_NULL == 7,
    "DAS_VARIANT_TYPE_NULL must be 7");

// Test 2: Factory returns success
TEST(DasVariantVectorTest, FactoryReturnsSuccess)
{
    DAS::DasPtr<Das::ExportInterface::IDasVariantVector> vector;
    DasResult result = CreateIDasVariantVector(vector.Put());
    ASSERT_EQ(result, DAS_S_OK);
    ASSERT_NE(vector.Get(), nullptr);
}

// Test 3: PushBackNull + IsNull round-trip
TEST(DasVariantVectorTest, PushBackNullIsNull)
{
    DAS::DasPtr<Das::ExportInterface::IDasVariantVector> vector;
    ASSERT_EQ(CreateIDasVariantVector(vector.Put()), DAS_S_OK);

    ASSERT_EQ(vector->PushBackNull(), DAS_S_OK);

    bool is_null = false;
    ASSERT_EQ(vector->IsNull(0, &is_null), DAS_S_OK);
    EXPECT_TRUE(is_null);
}

// Test 4: IsNull returns false for non-null
TEST(DasVariantVectorTest, IsNullFalseForNonNull)
{
    DAS::DasPtr<Das::ExportInterface::IDasVariantVector> vector;
    ASSERT_EQ(CreateIDasVariantVector(vector.Put()), DAS_S_OK);

    ASSERT_EQ(vector->PushBackInt(42), DAS_S_OK);

    bool is_null = true;
    ASSERT_EQ(vector->IsNull(0, &is_null), DAS_S_OK);
    EXPECT_FALSE(is_null);
}

// Test 5: IsNull out-of-range
TEST(DasVariantVectorTest, IsNullOutOfRange)
{
    DAS::DasPtr<Das::ExportInterface::IDasVariantVector> vector;
    ASSERT_EQ(CreateIDasVariantVector(vector.Put()), DAS_S_OK);

    bool is_null = false;
    EXPECT_EQ(vector->IsNull(0, &is_null), DAS_E_OUT_OF_RANGE);
}

// Test 6: GetImage type error
TEST(DasVariantVectorTest, GetImageTypeError)
{
    DAS::DasPtr<Das::ExportInterface::IDasVariantVector> vector;
    ASSERT_EQ(CreateIDasVariantVector(vector.Put()), DAS_S_OK);

    ASSERT_EQ(vector->PushBackInt(42), DAS_S_OK);

    Das::ExportInterface::IDasImage* p_image = nullptr;
    EXPECT_EQ(vector->GetImage(0, &p_image), DAS_E_TYPE_ERROR);
}

// Test 7: GetImage out-of-range
TEST(DasVariantVectorTest, GetImageOutOfRange)
{
    DAS::DasPtr<Das::ExportInterface::IDasVariantVector> vector;
    ASSERT_EQ(CreateIDasVariantVector(vector.Put()), DAS_S_OK);

    Das::ExportInterface::IDasImage* p_image = nullptr;
    EXPECT_EQ(vector->GetImage(0, &p_image), DAS_E_OUT_OF_RANGE);
}

// Test 8: GetImage null out-param
TEST(DasVariantVectorTest, GetImageNullOutParam)
{
    DAS::DasPtr<Das::ExportInterface::IDasVariantVector> vector;
    ASSERT_EQ(CreateIDasVariantVector(vector.Put()), DAS_S_OK);

    EXPECT_EQ(vector->GetImage(0, nullptr), DAS_E_INVALID_POINTER);
}

// Test 9: SetImage null in-param
TEST(DasVariantVectorTest, SetImageNullInParam)
{
    DAS::DasPtr<Das::ExportInterface::IDasVariantVector> vector;
    ASSERT_EQ(CreateIDasVariantVector(vector.Put()), DAS_S_OK);
    ASSERT_EQ(vector->PushBackInt(0), DAS_S_OK);

    EXPECT_EQ(vector->SetImage(0, nullptr), DAS_E_INVALID_POINTER);
}

// Test 10: PushBackImage null in-param
TEST(DasVariantVectorTest, PushBackImageNullInParam)
{
    DAS::DasPtr<Das::ExportInterface::IDasVariantVector> vector;
    ASSERT_EQ(CreateIDasVariantVector(vector.Put()), DAS_S_OK);

    EXPECT_EQ(vector->PushBackImage(nullptr), DAS_E_INVALID_POINTER);
}

// Test 11: GetType for IMAGE
TEST(DasVariantVectorTest, GetTypeForImage)
{
    DAS::DasPtr<Das::ExportInterface::IDasVariantVector> vector;
    ASSERT_EQ(CreateIDasVariantVector(vector.Put()), DAS_S_OK);

    auto* stub = TestImageStub::MakeRaw();
    ASSERT_NE(stub, nullptr);
    ASSERT_EQ(vector->PushBackImage(stub), DAS_S_OK);
    stub->Release();

    Das::ExportInterface::DasVariantType type =
        Das::ExportInterface::DAS_VARIANT_TYPE_FORCE_DWORD;
    ASSERT_EQ(vector->GetType(0, &type), DAS_S_OK);
    EXPECT_EQ(type, Das::ExportInterface::DAS_VARIANT_TYPE_IMAGE);
}

// Test 12: GetType for NULL
TEST(DasVariantVectorTest, GetTypeForNull)
{
    DAS::DasPtr<Das::ExportInterface::IDasVariantVector> vector;
    ASSERT_EQ(CreateIDasVariantVector(vector.Put()), DAS_S_OK);

    ASSERT_EQ(vector->PushBackNull(), DAS_S_OK);

    Das::ExportInterface::DasVariantType type =
        Das::ExportInterface::DAS_VARIANT_TYPE_FORCE_DWORD;
    ASSERT_EQ(vector->GetType(0, &type), DAS_S_OK);
    EXPECT_EQ(type, Das::ExportInterface::DAS_VARIANT_TYPE_NULL);
}

// Test 13: PushBackImage + GetImage round-trip
TEST(DasVariantVectorTest, GetImageRoundTrip)
{
    DAS::DasPtr<Das::ExportInterface::IDasVariantVector> vector;
    ASSERT_EQ(CreateIDasVariantVector(vector.Put()), DAS_S_OK);

    auto* stub = TestImageStub::MakeRaw();
    ASSERT_NE(stub, nullptr);

    ASSERT_EQ(vector->PushBackImage(stub), DAS_S_OK);
    // PushBackImage AddRefs internally, caller still holds original ref
    stub->Release();

    Das::ExportInterface::IDasImage* p_out = nullptr;
    ASSERT_EQ(vector->GetImage(0, &p_out), DAS_S_OK);
    ASSERT_NE(p_out, nullptr);
    // GetImage returns an AddRef'd pointer — release it
    p_out->Release();
}

// Test 14: SetImage overwrite
TEST(DasVariantVectorTest, SetImageOverwrite)
{
    DAS::DasPtr<Das::ExportInterface::IDasVariantVector> vector;
    ASSERT_EQ(CreateIDasVariantVector(vector.Put()), DAS_S_OK);

    ASSERT_EQ(vector->PushBackInt(0), DAS_S_OK);

    auto* stub = TestImageStub::MakeRaw();
    ASSERT_NE(stub, nullptr);
    ASSERT_EQ(vector->SetImage(0, stub), DAS_S_OK);
    stub->Release();

    Das::ExportInterface::DasVariantType type =
        Das::ExportInterface::DAS_VARIANT_TYPE_FORCE_DWORD;
    ASSERT_EQ(vector->GetType(0, &type), DAS_S_OK);
    EXPECT_EQ(type, Das::ExportInterface::DAS_VARIANT_TYPE_IMAGE);

    Das::ExportInterface::IDasImage* p_out = nullptr;
    ASSERT_EQ(vector->GetImage(0, &p_out), DAS_S_OK);
    ASSERT_NE(p_out, nullptr);
    p_out->Release();
}

// Test 15: PushBackImage ordering
TEST(DasVariantVectorTest, PushBackImageOrdering)
{
    DAS::DasPtr<Das::ExportInterface::IDasVariantVector> vector;
    ASSERT_EQ(CreateIDasVariantVector(vector.Put()), DAS_S_OK);

    auto* stub_a = TestImageStub::MakeRaw();
    ASSERT_NE(stub_a, nullptr);
    auto* stub_b = TestImageStub::MakeRaw();
    ASSERT_NE(stub_b, nullptr);

    ASSERT_EQ(vector->PushBackImage(stub_a), DAS_S_OK);
    stub_a->Release();
    ASSERT_EQ(vector->PushBackImage(stub_b), DAS_S_OK);
    stub_b->Release();
    ASSERT_EQ(vector->PushBackInt(99), DAS_S_OK);

    // GetImage at 0 returns stub_a
    Das::ExportInterface::IDasImage* p_out_a = nullptr;
    ASSERT_EQ(vector->GetImage(0, &p_out_a), DAS_S_OK);
    ASSERT_NE(p_out_a, nullptr);
    p_out_a->Release();

    // GetImage at 1 returns stub_b
    Das::ExportInterface::IDasImage* p_out_b = nullptr;
    ASSERT_EQ(vector->GetImage(1, &p_out_b), DAS_S_OK);
    ASSERT_NE(p_out_b, nullptr);
    p_out_b->Release();

    // Index 2 is INT, GetImage should fail
    Das::ExportInterface::IDasImage* p_out_c = nullptr;
    EXPECT_EQ(vector->GetImage(2, &p_out_c), DAS_E_TYPE_ERROR);
}
