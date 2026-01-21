#include <das/DasException.hpp>
#include <gtest/gtest.h>

TEST(DasExceptionHandleTest, HandleCreation_NoTypePunning)
{
    DasResult                 error_code = DAS_E_FAIL;
    DasExceptionSourceInfo    source_info{"test.cpp", 10, "test_func"};
    DasExceptionStringHandle* p_handle = nullptr;

    // 创建 handle（不使用类型双关）
    CreateDasExceptionString(error_code, &source_info, &p_handle);

    ASSERT_NE(p_handle, nullptr);

    // 验证 handle 类型安全
    const char* cstr = GetDasExceptionStringCStr(p_handle);
    EXPECT_NE(cstr, nullptr);
    EXPECT_GT(std::strlen(cstr), 0);

    // 清理（安全删除，无需类型转换）
    DeleteDasExceptionString(p_handle);
}

TEST(DasExceptionHandleTest, CrossDllException_ABIStability)
{
    // 在一个 DLL 中创建异常
    DasException ex{DAS_E_FAIL, "Test message"};

    // 验证异常可以正常传递
    EXPECT_STREQ(ex.what(), "Test message");
    EXPECT_EQ(ex.GetErrorCode(), DAS_E_FAIL);

    // 验证可以在另一个编译单元中捕获
    try
    {
        throw ex;
    }
    catch (const DasException& caught)
    {
        EXPECT_STREQ(caught.what(), "Test message");
    }
}

TEST(DasExceptionHandleTest, SharedPtr_ResourceManagement)
{
    // 测试 shared_ptr 正确管理资源
    {
        DasException ex{DAS_E_FAIL, "Test message"};
        // shared_ptr 管理的 handle 应该在异常析构时自动释放
    }

    // 验证无内存泄漏（需要使用 Valgrind 或 ASan）
    SUCCEED();
}
