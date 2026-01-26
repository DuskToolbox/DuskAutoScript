#include <das/DasException.hpp>
#include <gtest/gtest.h>

// Empty test suite for now - tests will be added in following tasks
// Test file structure verified

TEST(DasExceptionTest, CreateDasExceptionString_Basic)
{
    DasResult              error_code = DAS_E_FAIL;
    DasExceptionSourceInfo source_info{"test.cpp", 10, "test_func"};
    IDasExceptionString*   p_handle = nullptr;

    CreateDasExceptionString(error_code, &source_info, &p_handle);

    ASSERT_NE(p_handle, nullptr);

    // Clean up
    if (p_handle)
    {
        p_handle->Release();
    }
}

TEST(DasExceptionTest, CreateDasExceptionString_WithMessage)
{
    DasResult              error_code = DAS_E_FAIL;
    DasExceptionSourceInfo source_info{"test.cpp", 10, "test_func"};
    std::string            custom_msg = "Custom error message";
    IDasExceptionString*   p_handle = nullptr;

    CreateDasExceptionStringWithMessage(
        error_code,
        &source_info,
        custom_msg.c_str(),
        &p_handle);

    ASSERT_NE(p_handle, nullptr);

    // Clean up
    if (p_handle)
    {
        p_handle->Release();
    }
}

TEST(DasExceptionTest, CreateDasExceptionString_WithTypeInfo)
{
    DasResult              error_code = DAS_E_FAIL;
    IDasTypeInfo*          p_type_info = nullptr;
    DasExceptionSourceInfo source_info{"test.cpp", 10, "test_func"};
    IDasExceptionString*   p_handle = nullptr;

    CreateDasExceptionStringWithTypeInfo(
        error_code,
        &source_info,
        p_type_info,
        &p_handle);

    ASSERT_NE(p_handle, nullptr);

    // Clean up
    if (p_handle)
    {
        p_handle->Release();
    }
}

TEST(DasExceptionTest, GetDasExceptionStringCStr_ValidHandle)
{
    IDasExceptionString* p_handle = nullptr;
    CreateDasExceptionString(DAS_E_FAIL, nullptr, &p_handle);

    ASSERT_NE(p_handle, nullptr);

    const char* cstr = nullptr;
    p_handle->GetU8(&cstr);
    ASSERT_NE(cstr, nullptr);
    EXPECT_STREQ(cstr, "Unknown error"); // Default error message

    if (p_handle)
    {
        p_handle->Release();
    }
}

TEST(DasExceptionTest, GetDasExceptionStringCStr_Nullptr)
{
    IDasExceptionString* p_handle = nullptr;
    CreateDasExceptionString(DAS_E_FAIL, nullptr, &p_handle);

    const char* cstr = nullptr;
    if (p_handle)
    {
        p_handle->GetU8(&cstr);
    }
    // Should return nullptr or empty string without crash

    if (p_handle)
    {
        p_handle->Release();
    }
}
