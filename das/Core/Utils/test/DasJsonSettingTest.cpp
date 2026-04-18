#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/Utils/Config.h>
#include <das/Core/Utils/DasJsonSettingImpl.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasJsonSettingOperator.Implements.hpp>
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
