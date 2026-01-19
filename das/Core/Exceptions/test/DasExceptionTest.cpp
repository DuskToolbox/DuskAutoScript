#include <das/DasException.hpp>
#include <gtest/gtest.h>

// Empty test suite for now - tests will be added in following tasks
// Test file structure verified

TEST(DasExceptionTest, CreateDasExceptionString_Basic)
{
    DasResult                 error_code = DAS_E_FAIL;
    DasExceptionSourceInfo    source_info{"test.cpp", 10, "test_func"};
    DasExceptionStringHandle* p_handle = nullptr;

    CreateDasExceptionString(error_code, &source_info, &p_handle);

    ASSERT_NE(p_handle, nullptr);

    // Clean up
    DeleteDasExceptionString(p_handle);
}

TEST(DasExceptionTest, CreateDasExceptionString_WithMessage)
{
    DasResult                 error_code = DAS_E_FAIL;
    DasExceptionSourceInfo    source_info{"test.cpp", 10, "test_func"};
    std::string               custom_msg = "Custom error message";
    DasExceptionStringHandle* p_handle = nullptr;

    CreateDasExceptionString(error_code, custom_msg, &source_info, &p_handle);

    ASSERT_NE(p_handle, nullptr);

    // Clean up
    DeleteDasExceptionString(p_handle);
}

TEST(DasExceptionTest, CreateDasExceptionString_WithTypeInfo)
{
    DasResult                 error_code = DAS_E_FAIL;
    IDasTypeInfo*             p_type_info = nullptr;
    DasExceptionSourceInfo    source_info{"test.cpp", 10, "test_func"};
    DasExceptionStringHandle* p_handle = nullptr;

    CreateDasExceptionString(error_code, p_type_info, &source_info, &p_handle);

    ASSERT_NE(p_handle, nullptr);

    // Clean up
    DeleteDasExceptionString(p_handle);
}

TEST(DasExceptionTest, GetDasExceptionStringCStr_ValidHandle)
{
    DasExceptionStringHandle* p_handle = nullptr;
    CreateDasExceptionString(DAS_E_FAIL, nullptr, &p_handle);

    ASSERT_NE(p_handle, nullptr);

    const char* cstr = GetDasExceptionStringCStr(p_handle);
    ASSERT_NE(cstr, nullptr);
    EXPECT_STREQ(cstr, "Unknown error"); // Default error message

    DeleteDasExceptionString(p_handle);
}

TEST(DasExceptionTest, GetDasExceptionStringCStr_Nullptr)
{
    const char* cstr = GetDasExceptionStringCStr(nullptr);
    // Should return nullptr or empty string without crash
}

TEST(DasExceptionTest, CreateDasExceptionString_WithMessage)
{
    DasResult                 error_code = DAS_E_FAIL;
    DasExceptionSourceInfo    source_info{"test.cpp", 10, "test_func"};
    std::string               custom_msg = "Custom error message";
    DasExceptionStringHandle* p_handle = nullptr;

    CreateDasExceptionString(error_code, custom_msg, &source_info, &p_handle);

    ASSERT_NE(p_handle, nullptr);

    // Clean up
    DeleteDasExceptionString(p_handle);
}
