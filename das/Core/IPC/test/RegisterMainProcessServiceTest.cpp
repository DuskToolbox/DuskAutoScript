/**
 * @file RegisterMainProcessServiceTest.cpp
 * @brief Tests for DasRegisterMainProcessService and
 * DasUnregisterMainProcessService
 *
 * Covers:
 *   - No IPC context → DAS_E_OBJECT_NOT_INIT
 *   - Null pointer → DAS_E_INVALID_POINTER
 *   - E2E: register → query → verify
 *   - Duplicate IID → DAS_E_DUPLICATE_ELEMENT
 *   - Unregister → query returns not found
 *   - Unregister unknown IID → DAS_E_IPC_OBJECT_NOT_FOUND
 *
 * No mocks. All objects are real.
 */

#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <das/Core/IPC/MainProcess/IpcContext.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/DasSwigApi.h>
#include <das/Utils/fmt.h>
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

TEST(
    RegisterMainProcessServiceNoCtxTest,
    NoContext_RegisterReturnsObjectNotInit)
{
    // Use a real object to bypass the null-pointer guard and reach the
    // context check inside DasRegisterMainProcessService.
    auto* obj = new TestReadOnlyString("dummy");
    auto  result = DasRegisterMainProcessService(obj, DAS_IID_READ_ONLY_STRING);
    EXPECT_EQ(result, DAS_E_OBJECT_NOT_INIT);
    obj->Release();
}

TEST(
    RegisterMainProcessServiceNoCtxTest,
    NoContext_UnregisterReturnsObjectNotInit)
{
    auto result = DasUnregisterMainProcessService(DAS_IID_READ_ONLY_STRING);
    EXPECT_EQ(result, DAS_E_OBJECT_NOT_INIT);
}

// ====== Tests with IPC context (fixture) ======

class RegisterMainProcessServiceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ctx_ = MainProcess::CreateIpcContextEz(false);
        ASSERT_NE(ctx_.get(), nullptr);
    }

    void TearDown() override
    {
        ctx_->UnregisterService(DAS_IID_READ_ONLY_STRING);
    }

    MainProcess::IpcContextPtr ctx_;
};

TEST_F(RegisterMainProcessServiceTest, NullObject_ReturnsInvalidPointer)
{
    ScopedCurrentIpcContext scope(
        static_cast<MainProcess::IpcContext*>(ctx_.get()));

    auto result =
        DasRegisterMainProcessService(nullptr, DAS_IID_READ_ONLY_STRING);
    EXPECT_EQ(result, DAS_E_INVALID_POINTER);
}

TEST_F(RegisterMainProcessServiceTest, E2E_RegisterQuery_VerifyObject)
{
    auto* test_string = new TestReadOnlyString("Service OK");

    ScopedCurrentIpcContext scope(
        static_cast<MainProcess::IpcContext*>(ctx_.get()));

    // Register
    auto reg_result =
        DasRegisterMainProcessService(test_string, DAS_IID_READ_ONLY_STRING);
    ASSERT_EQ(reg_result, DAS_S_OK);

    // Query via SWIG wrapper (end-to-end)
    auto query = QueryMainProcessInterface(DAS_IID_READ_ONLY_STRING);
    ASSERT_EQ(query.GetErrorCode(), DAS_S_OK);
    ASSERT_NE(query.value.Get(), nullptr);

    auto* readonly_str = static_cast<IDasReadOnlyString*>(query.value.Get());
    const char* utf8 = nullptr;
    DasResult   hr = readonly_str->GetUtf8(&utf8);
    EXPECT_EQ(hr, DAS_S_OK);
    ASSERT_NE(utf8, nullptr);
    EXPECT_STREQ(utf8, "Service OK");

    // Release the initial reference (the one from new)
    test_string->Release();
}

TEST_F(RegisterMainProcessServiceTest, DuplicateIID_ReturnsDuplicateElement)
{
    auto* obj1 = new TestReadOnlyString("first");
    auto* obj2 = new TestReadOnlyString("second");

    ScopedCurrentIpcContext scope(
        static_cast<MainProcess::IpcContext*>(ctx_.get()));

    // First registration succeeds
    auto reg1 = DasRegisterMainProcessService(obj1, DAS_IID_READ_ONLY_STRING);
    ASSERT_EQ(reg1, DAS_S_OK);

    // Duplicate IID fails
    auto reg2 = DasRegisterMainProcessService(obj2, DAS_IID_READ_ONLY_STRING);
    EXPECT_EQ(reg2, DAS_E_DUPLICATE_ELEMENT);

    // obj2 was never registered — release the initial reference
    obj2->Release();
    // obj1 still held by framework — release initial reference
    obj1->Release();
}

TEST_F(
    RegisterMainProcessServiceTest,
    UnregisterThenQuery_ReturnsObjectNotFound)
{
    auto* test_string = new TestReadOnlyString("temporary");

    ScopedCurrentIpcContext scope(
        static_cast<MainProcess::IpcContext*>(ctx_.get()));

    // Register
    auto reg_result =
        DasRegisterMainProcessService(test_string, DAS_IID_READ_ONLY_STRING);
    ASSERT_EQ(reg_result, DAS_S_OK);

    // Unregister
    auto unreg_result =
        DasUnregisterMainProcessService(DAS_IID_READ_ONLY_STRING);
    EXPECT_EQ(unreg_result, DAS_S_OK);

    // Query should now fail
    auto query = QueryMainProcessInterface(DAS_IID_READ_ONLY_STRING);
    EXPECT_EQ(query.GetErrorCode(), DAS_E_IPC_OBJECT_NOT_FOUND);
    EXPECT_EQ(query.value.Get(), nullptr);
}

TEST_F(
    RegisterMainProcessServiceTest,
    UnregisterUnknownIID_ReturnsObjectNotFound)
{
    ScopedCurrentIpcContext scope(
        static_cast<MainProcess::IpcContext*>(ctx_.get()));

    DasGuid unknown_iid = {
        0xDEADBEEF,
        0xCAFE,
        0xBEEF,
        {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}};

    auto result = DasUnregisterMainProcessService(unknown_iid);
    EXPECT_EQ(result, DAS_E_IPC_OBJECT_NOT_FOUND);
}

// ====== By-name tests without IPC context ======

TEST(
    RegisterMainProcessServiceNoCtxTest,
    NoContext_RegisterByNameReturnsObjectNotInit)
{
    auto* obj = new TestReadOnlyString("dummy");
    auto  result = DasRegisterMainProcessServiceByName(
        obj,
        DAS_IID_READ_ONLY_STRING,
        "test_name");
    EXPECT_EQ(result, DAS_E_OBJECT_NOT_INIT);
    obj->Release();
}

TEST(
    RegisterMainProcessServiceNoCtxTest,
    NoContext_QueryByNameReturnsObjectNotInit)
{
    IDasBase* out = nullptr;
    auto      result = DasQueryMainProcessInterfaceByName("test_name", &out);
    EXPECT_EQ(result, DAS_E_OBJECT_NOT_INIT);
}

TEST(
    RegisterMainProcessServiceNoCtxTest,
    NoContext_UnregisterByNameReturnsObjectNotInit)
{
    auto result = DasUnregisterMainProcessServiceByName("test_name");
    EXPECT_EQ(result, DAS_E_OBJECT_NOT_INIT);
}

// ====== By-name tests with IPC context ======

class RegisterMainProcessServiceByNameTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ctx_ = MainProcess::CreateIpcContextEz(false);
        ASSERT_NE(ctx_.get(), nullptr);
    }

    void TearDown() override
    {
        // Clean up by-name registrations
        ctx_->UnregisterServiceByName("my_service");
        ctx_->UnregisterServiceByName("custom_name");
        ctx_->UnregisterServiceByName("shared_name");
        ctx_->UnregisterService(DAS_IID_READ_ONLY_STRING);
    }

    MainProcess::IpcContextPtr ctx_;
};

TEST_F(
    RegisterMainProcessServiceByNameTest,
    NullName_RegisterByNameReturnsInvalidPointer)
{
    auto*                   obj = new TestReadOnlyString("test");
    ScopedCurrentIpcContext scope(
        static_cast<MainProcess::IpcContext*>(ctx_.get()));

    auto result = DasRegisterMainProcessServiceByName(
        obj,
        DAS_IID_READ_ONLY_STRING,
        nullptr);
    EXPECT_EQ(result, DAS_E_INVALID_POINTER);
    obj->Release();
}

TEST_F(
    RegisterMainProcessServiceByNameTest,
    NullObject_RegisterByNameReturnsInvalidPointer)
{
    ScopedCurrentIpcContext scope(
        static_cast<MainProcess::IpcContext*>(ctx_.get()));

    auto result = DasRegisterMainProcessServiceByName(
        nullptr,
        DAS_IID_READ_ONLY_STRING,
        "my_service");
    EXPECT_EQ(result, DAS_E_INVALID_POINTER);
}

TEST_F(
    RegisterMainProcessServiceByNameTest,
    E2E_RegisterByName_QueryByName_VerifyObject)
{
    auto* test_string = new TestReadOnlyString("Named Service");

    ScopedCurrentIpcContext scope(
        static_cast<MainProcess::IpcContext*>(ctx_.get()));

    // Register by name
    auto reg_result = DasRegisterMainProcessServiceByName(
        test_string,
        DAS_IID_READ_ONLY_STRING,
        "my_service");
    ASSERT_EQ(reg_result, DAS_S_OK);

    // Query by name
    auto query = QueryMainProcessInterfaceByName("my_service");
    ASSERT_EQ(query.GetErrorCode(), DAS_S_OK);
    ASSERT_NE(query.value.Get(), nullptr);

    auto* readonly_str = static_cast<IDasReadOnlyString*>(query.value.Get());
    const char* utf8 = nullptr;
    DasResult   hr = readonly_str->GetUtf8(&utf8);
    EXPECT_EQ(hr, DAS_S_OK);
    ASSERT_NE(utf8, nullptr);
    EXPECT_STREQ(utf8, "Named Service");

    test_string->Release();
}

TEST_F(
    RegisterMainProcessServiceByNameTest,
    RegisterDefaultGuidName_QueryByNameGuidString_Succeeds)
{
    auto* test_string = new TestReadOnlyString("Default Name");

    ScopedCurrentIpcContext scope(
        static_cast<MainProcess::IpcContext*>(ctx_.get()));

    // Register with default (IID-based) name
    auto reg_result =
        DasRegisterMainProcessService(test_string, DAS_IID_READ_ONLY_STRING);
    ASSERT_EQ(reg_result, DAS_S_OK);

    // The default name is the GUID string representation of the IID
    std::string guid_name = DAS::fmt::format("{}", DAS_IID_READ_ONLY_STRING);

    // Query by the GUID string name
    auto query = QueryMainProcessInterfaceByName(guid_name.c_str());
    ASSERT_EQ(query.GetErrorCode(), DAS_S_OK);
    ASSERT_NE(query.value.Get(), nullptr);

    auto* readonly_str = static_cast<IDasReadOnlyString*>(query.value.Get());
    const char* utf8 = nullptr;
    readonly_str->GetUtf8(&utf8);
    ASSERT_NE(utf8, nullptr);
    EXPECT_STREQ(utf8, "Default Name");

    test_string->Release();
}

TEST_F(
    RegisterMainProcessServiceByNameTest,
    DuplicateNameAcrossDifferentIids_ReturnsDuplicateElement)
{
    auto* obj1 = new TestReadOnlyString("first");

    DasGuid other_iid = {
        0xAAAAAAAA,
        0xBBBB,
        0xCCCC,
        {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}};

    ScopedCurrentIpcContext scope(
        static_cast<MainProcess::IpcContext*>(ctx_.get()));

    // First registration with name "shared_name"
    auto reg1 = DasRegisterMainProcessServiceByName(
        obj1,
        DAS_IID_READ_ONLY_STRING,
        "shared_name");
    ASSERT_EQ(reg1, DAS_S_OK);

    // Second registration with different IID but same name → should fail
    // (Need to use a different object for the different IID since
    // TestReadOnlyString only implements IDasReadOnlyString)
    auto* obj2 = new TestReadOnlyString("second");
    auto  reg2 =
        DasRegisterMainProcessServiceByName(obj2, other_iid, "shared_name");
    EXPECT_EQ(reg2, DAS_E_DUPLICATE_ELEMENT);

    obj2->Release();
    obj1->Release();
}

TEST_F(
    RegisterMainProcessServiceByNameTest,
    UnregisterByName_ThenQueryByName_ReturnsObjectNotFound)
{
    auto* test_string = new TestReadOnlyString("temporary_named");

    ScopedCurrentIpcContext scope(
        static_cast<MainProcess::IpcContext*>(ctx_.get()));

    // Register by name
    auto reg_result = DasRegisterMainProcessServiceByName(
        test_string,
        DAS_IID_READ_ONLY_STRING,
        "my_service");
    ASSERT_EQ(reg_result, DAS_S_OK);

    // Unregister by name
    auto unreg_result = DasUnregisterMainProcessServiceByName("my_service");
    EXPECT_EQ(unreg_result, DAS_S_OK);

    // Query by name should now fail
    auto query = QueryMainProcessInterfaceByName("my_service");
    EXPECT_EQ(query.GetErrorCode(), DAS_E_IPC_OBJECT_NOT_FOUND);
    EXPECT_EQ(query.value.Get(), nullptr);

    test_string->Release();
}
