#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <gtest/gtest.h>

// ============================================================================
// GetUtf16 null-termination contract tests (Phase 78.1)
//
// All IDasReadOnlyString::GetUtf16 implementations must satisfy:
//   out_string[out_string_size] == 0
//   out_string_size does NOT include the NUL terminator
// ============================================================================

TEST(DasStringNullTerminationTest, GetUtf16_Ascii)
{
    auto* p_string = new DasStringCppImpl();
    p_string->SetUtf8("hello");
    const char16_t* out_string = nullptr;
    size_t          out_string_size = 0;
    auto            result = p_string->GetUtf16(&out_string, &out_string_size);
    EXPECT_EQ(result, DAS_S_OK);
    ASSERT_NE(out_string, nullptr);
    EXPECT_EQ(out_string_size, 5u); // "hello" = 5 UTF-16 code units
    EXPECT_EQ(out_string[out_string_size], u'\0');
    p_string->Release();
}

TEST(DasStringNullTerminationTest, GetUtf16_CJK)
{
    auto* p_string = new DasStringCppImpl();
    p_string->SetUtf8(reinterpret_cast<const char*>(u8"测试"));
    const char16_t* out_string = nullptr;
    size_t          out_string_size = 0;
    auto            result = p_string->GetUtf16(&out_string, &out_string_size);
    EXPECT_EQ(result, DAS_S_OK);
    ASSERT_NE(out_string, nullptr);
    EXPECT_EQ(out_string_size, 2u); // "测试" = 2 UTF-16 code units
    EXPECT_EQ(out_string[out_string_size], u'\0');
    p_string->Release();
}

TEST(DasStringNullTerminationTest, GetUtf16_NullString)
{
    IDasReadOnlyString* p_null = nullptr;
    CreateNullDasString(&p_null);
    ASSERT_NE(p_null, nullptr);
    const char16_t* out_string = nullptr;
    size_t          out_string_size = 42; // sentinel value
    auto            result = p_null->GetUtf16(&out_string, &out_string_size);
    EXPECT_EQ(result, DAS_S_OK);
    EXPECT_EQ(out_string_size, 0u);
    ASSERT_NE(out_string, nullptr);
    EXPECT_EQ(out_string[0], u'\0');
}

TEST(DasStringNullTerminationTest, GetUtf16_EmptyString)
{
    auto* p_string = new DasStringCppImpl();
    p_string->SetUtf8("");
    const char16_t* out_string = nullptr;
    size_t          out_string_size = 0;
    auto            result = p_string->GetUtf16(&out_string, &out_string_size);
    EXPECT_EQ(result, DAS_S_OK);
    ASSERT_NE(out_string, nullptr);
    EXPECT_EQ(out_string_size, 0u);
    EXPECT_EQ(out_string[out_string_size], u'\0');
    p_string->Release();
}

// ============================================================================
// GetUtf16 null-terminated buffer interoperability with C API conventions
//
// BorrowOrtPath (DasOrt.h) relies on the GetUtf16 null-termination contract:
// the returned pointer can be passed directly to any function expecting a
// null-terminated char16_t* (e.g., std::u16string constructor, reinterpret_cast
// to wchar_t* on Windows for Ort::Session).
// ============================================================================

TEST(DasStringNullTerminationTest, GetUtf16_StdU16StringInterop)
{
    auto* p_string = new DasStringCppImpl();
    p_string->SetUtf8(reinterpret_cast<const char*>(u8"路径/测试.onnx"));
    const char16_t* out_string = nullptr;
    size_t          out_string_size = 0;
    auto            result = p_string->GetUtf16(&out_string, &out_string_size);
    EXPECT_EQ(result, DAS_S_OK);
    ASSERT_NE(out_string, nullptr);

    // Construct std::u16string using the null-terminated constructor (no
    // explicit length). This will read until it finds u'\0'. If the buffer
    // were not null-terminated, this would read past the end.
    std::u16string reconstructed(out_string);
    EXPECT_EQ(reconstructed.size(), out_string_size);

    std::u16string expected = u"路径/测试.onnx";
    EXPECT_EQ(reconstructed, expected);

    p_string->Release();
}

TEST(DasStringNullTerminationTest, GetUtf16_MixedAsciiCjk)
{
    auto* p_string = new DasStringCppImpl();
    p_string->SetUtf8(
        reinterpret_cast<const char*>(u8"model_\xE6\xA8\xA1\xE5\x9E\x8B_v2"));
    const char16_t* out_string = nullptr;
    size_t          out_string_size = 0;
    p_string->GetUtf16(&out_string, &out_string_size);
    ASSERT_NE(out_string, nullptr);

    // Null-terminated constructor must produce identical content
    std::u16string reconstructed(out_string);
    std::u16string expected = u"model_模型_v2";
    EXPECT_EQ(reconstructed, expected);
    EXPECT_EQ(reconstructed.size(), out_string_size);
    EXPECT_EQ(out_string[out_string_size], u'\0');

    p_string->Release();
}
