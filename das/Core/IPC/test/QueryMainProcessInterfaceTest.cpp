/**
 * @file QueryMainProcessInterfaceTest.cpp
 * @brief End-to-end tests for QueryMainProcessInterface
 *
 * Tests the real Main path: create IpcContext -> register real
 * IDasReadOnlyString -> QueryMainProcessInterface -> verify GetUtf8().
 *
 * No mocks. All objects are real.
 */

#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/DasString.hpp>
#include <das/DasSwigApi.h>
#include <das/DasTypes.hpp>
#include <gtest/gtest.h>

using namespace Das::Core::IPC;

namespace
{

    // ====== Simple real IDasReadOnlyString implementation ======

    class TestReadOnlyString final : public IDasReadOnlyString
    {
        std::string str_;

    public:
        explicit TestReadOnlyString(std::string s) : str_(std::move(s)) {}

        // IDasReadOnlyString
        DasResult DAS_STD_CALL GetUtf8(const char** out_string) override
        {
            if (!out_string)
            {
                return DAS_E_INVALID_POINTER;
            }
            *out_string = str_.c_str();
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetUtf16(const char16_t**, size_t*) noexcept override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult DAS_STD_CALL GetW(const wchar_t**) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        const int32_t* CBegin() override { return nullptr; }
        const int32_t* CEnd() override { return nullptr; }

        // IDasBase
        DasResult DAS_STD_CALL QueryInterface(const DasGuid&, void**) override
        {
            return DAS_E_NO_INTERFACE;
        }

        uint32_t AddRef() override { return ++ref_count_; }

        uint32_t Release() override
        {
            auto c = --ref_count_;
            if (c == 0)
            {
                delete this;
            }
            return c;
        }

    private:
        std::atomic<uint32_t> ref_count_{1};
    };

} // namespace
// ====== Tests without IPC context ======

TEST(QueryMainProcessInterfaceTest, NoContext_ReturnsObjectNotInit)
{
    auto result = QueryMainProcessInterface(DAS_IID_READ_ONLY_STRING);
    EXPECT_EQ(result.GetErrorCode(), DAS_E_OBJECT_NOT_INIT);
    EXPECT_EQ(result.value.Get(), nullptr);
}

TEST(QueryMainProcessInterfaceTest, NoContext_QueryByName_ReturnsObjectNotInit)
{
    auto result = QueryMainProcessInterfaceByName("any_name");
    EXPECT_EQ(result.GetErrorCode(), DAS_E_OBJECT_NOT_INIT);
    EXPECT_EQ(result.value.Get(), nullptr);
}

// ====== DasRetIDasBase default state ======

TEST(QueryMainProcessInterfaceTest, DasRetIDasBase_DefaultState)
{
    DasRetIDasBase result;
    EXPECT_EQ(result.GetErrorCode(), DAS_E_UNDEFINED_RETURN_VALUE);
    EXPECT_EQ(result.value.Get(), nullptr);
}

TEST(QueryMainProcessInterfaceTest, DasRetIDasBase_SetGetErrorCode)
{
    DasRetIDasBase result;
    result.SetErrorCode(DAS_S_OK);
    EXPECT_EQ(result.GetErrorCode(), DAS_S_OK);

    result.SetErrorCode(DAS_E_OBJECT_NOT_INIT);
    EXPECT_EQ(result.GetErrorCode(), DAS_E_OBJECT_NOT_INIT);
}

// ====== End-to-end test with real IpcContext ======

class QueryMainProcessInterfaceE2ETest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        concrete_ctx_ = MainProcess::CreateIpcContextEz(false);
        ASSERT_NE(concrete_ctx_.get(), nullptr);
    }

    void TearDown() override
    {
        // Unregister any services we registered to avoid interference
        concrete_ctx_->UnregisterService(DAS_IID_READ_ONLY_STRING);
    }

    MainProcess::IpcContextPtr concrete_ctx_;
};

TEST_F(
    QueryMainProcessInterfaceE2ETest,
    QueryRegisteredString_ReturnsValidObject)
{
    auto* test_string = new TestReadOnlyString("Hello from main process!");

    // Use public RegisterService API (replaces direct object_manager_ +
    // registry_ access)
    ScopedCurrentIpcContext scope(
        static_cast<MainProcess::IpcContext*>(concrete_ctx_.get()));

    DasResult reg_result =
        concrete_ctx_->RegisterService(test_string, DAS_IID_READ_ONLY_STRING);
    ASSERT_EQ(reg_result, DAS_S_OK);

    // Query via public API
    auto result = QueryMainProcessInterface(DAS_IID_READ_ONLY_STRING);
    ASSERT_EQ(result.GetErrorCode(), DAS_S_OK);
    ASSERT_NE(result.value.Get(), nullptr);

    // Verify the object is the same one we registered
    auto* readonly_str = static_cast<IDasReadOnlyString*>(result.value.Get());
    const char* utf8 = nullptr;
    DasResult   hr = readonly_str->GetUtf8(&utf8);
    EXPECT_EQ(hr, DAS_S_OK);
    ASSERT_NE(utf8, nullptr);
    EXPECT_STREQ(utf8, "Hello from main process!");

    // Release the initial reference (the one from new)
    test_string->Release();
}

TEST_F(
    QueryMainProcessInterfaceE2ETest,
    QueryUnregisteredIID_ReturnsObjectNotFound)
{
    auto* test_string = new TestReadOnlyString("test");

    ScopedCurrentIpcContext scope(
        static_cast<MainProcess::IpcContext*>(concrete_ctx_.get()));

    DasResult reg_result =
        concrete_ctx_->RegisterService(test_string, DAS_IID_READ_ONLY_STRING);
    ASSERT_EQ(reg_result, DAS_S_OK);

    // Query with a different IID — should fail
    DasGuid unknown_iid = {
        0xDEADBEEF,
        0xCAFE,
        0xBEEF,
        {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}};

    auto result = QueryMainProcessInterface(unknown_iid);
    EXPECT_EQ(result.GetErrorCode(), DAS_E_IPC_OBJECT_NOT_FOUND);
    EXPECT_EQ(result.value.Get(), nullptr);

    test_string->Release();
}

TEST_F(
    QueryMainProcessInterfaceE2ETest,
    QueryMultipleStrings_CorrectOneReturned)
{
    auto* str1 = new TestReadOnlyString("First string");
    auto* str2 = new TestReadOnlyString("Second string");

    ScopedCurrentIpcContext scope(
        static_cast<MainProcess::IpcContext*>(concrete_ctx_.get()));

    // Register first string under DAS_IID_READ_ONLY_STRING
    DasResult reg1 =
        concrete_ctx_->RegisterService(str1, DAS_IID_READ_ONLY_STRING);
    ASSERT_EQ(reg1, DAS_S_OK);

    // Second registration with same IID should fail (duplicate)
    DasResult reg2 =
        concrete_ctx_->RegisterService(str2, DAS_IID_READ_ONLY_STRING);
    EXPECT_EQ(reg2, DAS_E_DUPLICATE_ELEMENT);

    auto result = QueryMainProcessInterface(DAS_IID_READ_ONLY_STRING);
    ASSERT_EQ(result.GetErrorCode(), DAS_S_OK);
    ASSERT_NE(result.value.Get(), nullptr);

    auto* readonly_str = static_cast<IDasReadOnlyString*>(result.value.Get());
    const char* utf8 = nullptr;
    readonly_str->GetUtf8(&utf8);
    ASSERT_NE(utf8, nullptr);
    EXPECT_STREQ(utf8, "First string");

    str1->Release();
    str2->Release();
}

// ====== By-name query tests ======

TEST_F(QueryMainProcessInterfaceE2ETest, QueryRegisteredName_ReturnsValidObject)
{
    auto* test_string = new TestReadOnlyString("Hello by name!");

    ScopedCurrentIpcContext scope(
        static_cast<MainProcess::IpcContext*>(concrete_ctx_.get()));

    DasResult reg_result = concrete_ctx_->RegisterServiceByName(
        test_string,
        DAS_IID_READ_ONLY_STRING,
        "greeting_service");
    ASSERT_EQ(reg_result, DAS_S_OK);

    // Query by name
    auto result =
        QueryMainProcessInterfaceByName("greeting_service");
    ASSERT_EQ(result.GetErrorCode(), DAS_S_OK);
    ASSERT_NE(result.value.Get(), nullptr);

    auto* readonly_str = static_cast<IDasReadOnlyString*>(result.value.Get());
    const char* utf8 = nullptr;
    DasResult   hr = readonly_str->GetUtf8(&utf8);
    EXPECT_EQ(hr, DAS_S_OK);
    ASSERT_NE(utf8, nullptr);
    EXPECT_STREQ(utf8, "Hello by name!");

    // Clean up
    concrete_ctx_->UnregisterServiceByName("greeting_service");
    test_string->Release();
}

TEST_F(QueryMainProcessInterfaceE2ETest, QueryUnknownName_ReturnsObjectNotFound)
{
    ScopedCurrentIpcContext scope(
        static_cast<MainProcess::IpcContext*>(concrete_ctx_.get()));

    auto result =
        QueryMainProcessInterfaceByName("nonexistent_service");
    EXPECT_EQ(result.GetErrorCode(), DAS_E_IPC_OBJECT_NOT_FOUND);
    EXPECT_EQ(result.value.Get(), nullptr);
}
