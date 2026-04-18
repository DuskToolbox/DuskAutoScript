#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/Utils/Config.h>
#include <das/Core/Utils/DasJsonSettingImpl.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasJsonSettingOnDeletedHandler.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasJsonSettingOperator.Implements.hpp>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

// Forward declaration — defined in DasJsonImpl.cpp
DasResult ParseDasJsonFromString(
    const char*                      p_u8_string,
    Das::ExportInterface::IDasJson** pp_out_json);

DAS_CORE_UTILS_NS_BEGIN

class TestJsonSettingOperator final
    : public Das::ExportInterface::DasJsonSettingOperatorImplBase<
          TestJsonSettingOperator>
{
public:
    DasResult DAS_STD_CALL
    Apply(Das::ExportInterface::IDasJson* p_json) override
    {
        if (!p_json)
        {
            return DAS_E_INVALID_POINTER;
        }
        auto key = DasReadOnlyString::FromUtf8("modified", nullptr);
        return p_json->SetIntByName(key.Get(), 42);
    }
};

class TestOnDeletedHandler final
    : public Das::ExportInterface::DasJsonSettingOnDeletedHandlerImplBase<
          TestOnDeletedHandler>
{
public:
    bool was_called = false;

    DasResult DAS_STD_CALL OnDeleted() override
    {
        was_called = true;
        return DAS_S_OK;
    }
};

DAS_CORE_UTILS_NS_END

namespace
{

    Das::DasPtr<IDasReadOnlyString> MakeString(const char* str)
    {
        IDasReadOnlyString* p = nullptr;
        ::CreateIDasReadOnlyStringFromUtf8(str, &p);
        return Das::DasPtr<IDasReadOnlyString>{p};
    }

    std::string ToStringValue(Das::ExportInterface::IDasJsonSetting& setting)
    {
        Das::DasPtr<IDasReadOnlyString> p_str;
        setting.ToString(p_str.Put());
        const auto expected =
            Das::Utils::ToU8StringWithoutOwnership(p_str.Get());
        return expected ? std::string{expected.value()} : "";
    }

} // namespace

TEST(DasJsonSettingTest, ExecuteAtomicallySyncsBack)
{
    auto setting = DAS::Core::Utils::DasJsonSettingImpl::Make();
    auto input = MakeString(R"({"count":0})");
    ASSERT_EQ(DAS_S_OK, setting->FromString(input.Get()));

    auto op = DAS::Core::Utils::TestJsonSettingOperator::Make();
    ASSERT_EQ(DAS_S_OK, setting->ExecuteAtomically(op.Get()));

    const auto result = ToStringValue(*setting.Get());
    auto       parsed = nlohmann::json::parse(result);
    EXPECT_EQ(42, parsed["modified"].get<int>());
    EXPECT_EQ(0, parsed["count"].get<int>());
}

TEST(DasJsonSettingTest, ConcurrentReadsAndWrites)
{
    auto setting = DAS::Core::Utils::DasJsonSettingImpl::Make();
    auto input = MakeString(R"({"value":0})");
    ASSERT_EQ(DAS_S_OK, setting->FromString(input.Get()));

    constexpr int iterations = 100;

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
    {
        threads.emplace_back(
            [&setting, iterations]()
            {
                for (int j = 0; j < iterations; ++j)
                {
                    Das::DasPtr<IDasReadOnlyString> p_str;
                    setting->ToString(p_str.Put());
                }
            });
    }

    threads.emplace_back(
        [&setting, iterations]()
        {
            for (int j = 0; j < iterations; ++j)
            {
                auto json_str = R"({"value":)" + std::to_string(j) + "}";
                auto input = MakeString(json_str.c_str());
                setting->FromString(input.Get());
            }
        });

    for (auto& t : threads)
    {
        t.join();
    }

    SUCCEED();
}

TEST(DasJsonSettingTest, ToStringFromStringRoundTrip)
{
    auto        setting = DAS::Core::Utils::DasJsonSettingImpl::Make();
    const char* json_text = R"({"name":"test","count":42})";
    auto        input = MakeString(json_text);
    ASSERT_EQ(DAS_S_OK, setting->FromString(input.Get()));

    const auto result = ToStringValue(*setting.Get());
    auto       parsed = nlohmann::json::parse(result);
    EXPECT_EQ("test", parsed["name"].get<std::string>());
    EXPECT_EQ(42, parsed["count"].get<int>());
}

TEST(DasJsonSettingTest, SavePersistsToFile)
{
    auto temp_path = std::filesystem::current_path() / "das_test_save.json";
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);

    {
        auto setting = DAS::Core::Utils::DasJsonSettingImpl::Make(temp_path);
        auto input = MakeString(R"({"key":"value"})");
        ASSERT_EQ(DAS_S_OK, setting->FromString(input.Get()));
        ASSERT_EQ(DAS_S_OK, setting->Save());
    }

    std::ifstream ifs{temp_path};
    ASSERT_TRUE(ifs.is_open());
    std::string content{
        std::istreambuf_iterator<char>(ifs),
        std::istreambuf_iterator<char>()};
    auto parsed = nlohmann::json::parse(content);
    EXPECT_EQ("value", parsed["key"].get<std::string>());

    std::filesystem::remove(temp_path, ec);
}

TEST(DasJsonSettingTest, SaveToWorkingDirectoryRelative)
{
    const char*     filename = "das_test_working_dir.json";
    auto            full_path = std::filesystem::current_path() / filename;
    std::error_code ec;
    std::filesystem::remove(full_path, ec);

    {
        auto setting = DAS::Core::Utils::DasJsonSettingImpl::Make();
        auto input = MakeString(R"({"saved":true})");
        ASSERT_EQ(DAS_S_OK, setting->FromString(input.Get()));

        auto rel_path = MakeString(filename);
        ASSERT_EQ(DAS_S_OK, setting->SaveToWorkingDirectory(rel_path.Get()));
    }

    ASSERT_TRUE(std::filesystem::exists(full_path));
    std::ifstream ifs{full_path};
    ASSERT_TRUE(ifs.is_open());
    std::string content{
        std::istreambuf_iterator<char>(ifs),
        std::istreambuf_iterator<char>()};
    auto parsed = nlohmann::json::parse(content);
    EXPECT_TRUE(parsed["saved"].get<bool>());

    std::filesystem::remove(full_path, ec);
}

TEST(DasJsonSettingTest, ConstructorLoadsFromFile)
{
    auto temp_path =
        std::filesystem::current_path() / "das_test_ctor_load.json";
    {
        std::ofstream ofs{temp_path};
        ofs << R"({"loaded":true})";
    }

    auto       setting = DAS::Core::Utils::DasJsonSettingImpl::Make(temp_path);
    const auto result = ToStringValue(*setting.Get());
    auto       parsed = nlohmann::json::parse(result);
    EXPECT_TRUE(parsed["loaded"].get<bool>());

    std::error_code ec;
    std::filesystem::remove(temp_path, ec);
}

TEST(DasJsonSettingTest, SetOnDeletedHandlerCalledOnDestruct)
{
    auto handler = DAS::Core::Utils::TestOnDeletedHandler::Make();
    auto raw =
        static_cast<DAS::Core::Utils::TestOnDeletedHandler*>(handler.Get());

    {
        auto setting = DAS::Core::Utils::DasJsonSettingImpl::Make();
        ASSERT_EQ(
            DAS_S_OK,
            setting->SetOnDeletedHandler(
                static_cast<
                    Das::ExportInterface::IDasJsonSettingOnDeletedHandler*>(
                    raw)));
        raw->AddRef();
    }

    EXPECT_TRUE(raw->was_called);
}

TEST(DasJsonSettingTest, NullPointerArgumentsReturnError)
{
    auto setting = DAS::Core::Utils::DasJsonSettingImpl::Make();

    EXPECT_EQ(DAS_E_INVALID_POINTER, setting->FromString(nullptr));
    EXPECT_EQ(DAS_E_INVALID_POINTER, setting->SaveToWorkingDirectory(nullptr));
    EXPECT_EQ(DAS_E_INVALID_POINTER, setting->ExecuteAtomically(nullptr));
}
