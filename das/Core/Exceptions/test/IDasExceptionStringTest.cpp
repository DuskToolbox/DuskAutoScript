#include <cstring>
#include <das/DasException.hpp>
#include <das/DasTypes.hpp>
#include <gtest/gtest.h>

TEST(IDasExceptionStringTest, Creation_Release)
{
    DasResult              error_code = DAS_E_FAIL;
    DasExceptionSourceInfo source_info{"test.cpp", 10, "test_func"};
    IDasExceptionString*   p_handle = nullptr;

    CreateDasExceptionString(error_code, &source_info, &p_handle);

    ASSERT_NE(p_handle, nullptr);

    const char* cstr = nullptr;
    p_handle->GetU8(&cstr);

    EXPECT_NE(cstr, nullptr);
    EXPECT_GT(std::strlen(cstr), 0);

    p_handle->Release();
}

TEST(IDasExceptionStringTest, EmptyErrorCode)
{
    DasResult              error_code = DAS_S_OK;
    DasExceptionSourceInfo source_info{"test.cpp", 20, "empty_func"};
    IDasExceptionString*   p_handle = nullptr;

    CreateDasExceptionString(error_code, &source_info, &p_handle);

    ASSERT_NE(p_handle, nullptr);

    const char* cstr = nullptr;
    p_handle->GetU8(&cstr);

    EXPECT_NE(cstr, nullptr);

    p_handle->Release();
}

TEST(IDasExceptionStringTest, MultipleRelease)
{
    DasResult              error_code = DAS_E_INVALID_ARGUMENT;
    DasExceptionSourceInfo source_info{"test.cpp", 30, "test_func"};
    IDasExceptionString*   p_handle = nullptr;

    CreateDasExceptionString(error_code, &source_info, &p_handle);

    ASSERT_NE(p_handle, nullptr);

    p_handle->Release();
}
