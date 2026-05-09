#include <atomic>
#include <das/DasException.hpp>
#include <das/_autogen/idl/abi/IDasTypeInfo.h>
#include <gtest/gtest.h>
#include <string_view>

namespace
{
    class FailingTypeInfo final : public IDasTypeInfo
    {
    public:
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

        DasResult DAS_STD_CALL GetGuid(DasGuid*) override { return DAS_E_FAIL; }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString**) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

    private:
        std::atomic<uint32_t> ref_count_{0};
    };

    std::string_view GetExceptionString(IDasExceptionString* const p_handle)
    {
        const char* cstr = nullptr;
        EXPECT_EQ(p_handle->GetU8(&cstr), DAS_S_OK);
        EXPECT_NE(cstr, nullptr);
        return cstr != nullptr ? std::string_view{cstr} : std::string_view{};
    }

    void ExpectContainsOperationFailed(IDasExceptionString* const p_handle)
    {
        const auto text = GetExceptionString(p_handle);
        EXPECT_NE(text.find("Operation failed"), std::string_view::npos);
    }
} // namespace

TEST(DasExceptionTest, CreateDasExceptionString_Basic)
{
    DasResult              error_code = DAS_E_FAIL;
    DasExceptionSourceInfo source_info{"test.cpp", 10, "test_func"};
    IDasExceptionString*   p_handle = nullptr;

    CreateDasExceptionString(error_code, &source_info, &p_handle);

    ASSERT_NE(p_handle, nullptr);
    ExpectContainsOperationFailed(p_handle);

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
    ExpectContainsOperationFailed(p_handle);

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
    ExpectContainsOperationFailed(p_handle);

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

    EXPECT_EQ(GetExceptionString(p_handle), "Operation failed");

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

TEST(DasExceptionTest, CreateDasExceptionString_WithFailingTypeInfoFallback)
{
    FailingTypeInfo        type_info{};
    DasExceptionSourceInfo source_info{"test.cpp", 10, "test_func"};
    IDasExceptionString*   p_handle = nullptr;

    CreateDasExceptionStringWithTypeInfo(
        DAS_E_FAIL,
        &source_info,
        &type_info,
        &p_handle);

    ASSERT_NE(p_handle, nullptr);
    ExpectContainsOperationFailed(p_handle);

    if (p_handle)
    {
        p_handle->Release();
    }
}
