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
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/DasString.hpp>
#include <das/DasSwigApi.h>
#include <das/DasTypes.hpp>
#include <gtest/gtest.h>

using Das::Swig::QueryMainProcessInterface;
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
        // Create real IpcContext with heartbeat disabled
        concrete_ctx_ = MainProcess::CreateIpcContextEz(false);
        ASSERT_NE(concrete_ctx_.get(), nullptr);

        // Clear per-context registry to avoid interference from other tests
        concrete_ctx_->registry_.Clear();
    }

    void TearDown() override
    {
        // Clean up the per-context registry
        concrete_ctx_->registry_.Clear();
        // concrete_ctx_ destructor cleans up everything internally
    }

    MainProcess::IpcContextPtr concrete_ctx_;
};

TEST_F(
    QueryMainProcessInterfaceE2ETest,
    QueryRegisteredString_ReturnsValidObject)
{
    // Create real IDasReadOnlyString implementation
    auto* test_string = new TestReadOnlyString("Hello from main process!");

    // Register to DistributedObjectManager (takes ownership via AddRef)
    ObjectId  obj_id;
    DasResult reg_result =
        concrete_ctx_->object_manager_.RegisterLocalObject(test_string, obj_id);
    ASSERT_EQ(reg_result, DAS_S_OK);

    // Register to RemoteObjectRegistry for interface-based lookup
    DasResult registry_result = concrete_ctx_->registry_.RegisterObject(
        obj_id,
        DAS_IID_READ_ONLY_STRING,
        1,
        "test_readonly_string",
        1);
    ASSERT_EQ(registry_result, DAS_S_OK);

    // Bind IPC context
    ScopedCurrentIpcContext scope(
        static_cast<MainProcess::IpcContext*>(concrete_ctx_.get()));

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
    // DistributedObjectManager holds its own DasPtr, so we need to balance
    // the initial ref_count of 1
    test_string->Release();
}

TEST_F(
    QueryMainProcessInterfaceE2ETest,
    QueryUnregisteredIID_ReturnsObjectNotFound)
{
    // Register an object under DAS_IID_READ_ONLY_STRING
    auto*    test_string = new TestReadOnlyString("test");
    ObjectId obj_id;
    concrete_ctx_->object_manager_.RegisterLocalObject(test_string, obj_id);

    concrete_ctx_->registry_
        .RegisterObject(obj_id, DAS_IID_READ_ONLY_STRING, 1, "test_string", 1);

    ScopedCurrentIpcContext scope(
        static_cast<MainProcess::IpcContext*>(concrete_ctx_.get()));

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
    // Register two different strings
    auto* str1 = new TestReadOnlyString("First string");
    auto* str2 = new TestReadOnlyString("Second string");

    ObjectId id1, id2;
    concrete_ctx_->object_manager_.RegisterLocalObject(str1, id1);
    concrete_ctx_->object_manager_.RegisterLocalObject(str2, id2);

    // Both registered under same IID — LookupByInterface returns the first
    concrete_ctx_->registry_
        .RegisterObject(id1, DAS_IID_READ_ONLY_STRING, 1, "str1", 1);
    concrete_ctx_->registry_
        .RegisterObject(id2, DAS_IID_READ_ONLY_STRING, 1, "str2", 1);

    ScopedCurrentIpcContext scope(
        static_cast<MainProcess::IpcContext*>(concrete_ctx_.get()));

    auto result = QueryMainProcessInterface(DAS_IID_READ_ONLY_STRING);
    ASSERT_EQ(result.GetErrorCode(), DAS_S_OK);
    ASSERT_NE(result.value.Get(), nullptr);

    auto* readonly_str = static_cast<IDasReadOnlyString*>(result.value.Get());
    const char* utf8 = nullptr;
    readonly_str->GetUtf8(&utf8);
    ASSERT_NE(utf8, nullptr);
    // Either "First string" or "Second string" is valid — both are registered
    EXPECT_TRUE(
        strcmp(utf8, "First string") == 0
        || strcmp(utf8, "Second string") == 0);

    str1->Release();
    str2->Release();
}
