#include "../src/MaapiRunTaskComponent.h"
#include "../src/PluginUtils.h"
#include "FakeMaaApiBoundary.h"

#include <das/Plugins/DasMaaPi/MaaPiExecutionEngine.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasStringVector.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace
{
    using namespace Das;
    using namespace Das::Plugins::DasMaaPi;
    using namespace Das::Plugins::DasMaaPi::Test;

    std::filesystem::path FixturePath(std::string_view name)
    {
        return std::filesystem::current_path() / "DasMaaPi" / "test"
               / "fixtures" / std::filesystem::path{name};
    }

    DasPtr<ExportInterface::IDasPortMap> MakeInputPortMap(
        std::string_view execution_input_json)
    {
        DasPtr<ExportInterface::IDasPortMap> input_map;
        EXPECT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);

        DasReadOnlyString key{"executionInput"};
        DasReadOnlyString val{std::string{execution_input_json}.c_str()};
        input_map->SetString(key.Get(), val.Get());
        return input_map;
    }

    DasPtr<ExportInterface::IDasJson> MakeSettingsJson(
        const std::string& pi_path,
        const std::string& task_name)
    {
        auto settings = Utils::MakeYyjsonObject();
        auto obj = settings.as_object();
        (*obj)["piPath"] = yyjson::value(pi_path);
        (*obj)["taskName"] = yyjson::value(task_name);
        (*obj)["options"] = Utils::MakeYyjsonObject();
        (*obj)["portMap"] = Utils::MakeYyjsonObject();
        return WrapJson(std::move(settings));
    }

    class RequestedStopToken final
        : public PluginInterface::DasStopTokenImplBase<RequestedStopToken>
    {
    public:
        DasResult StopRequested(bool* p_out_stop_requested) override
        {
            if (p_out_stop_requested == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_out_stop_requested = true;
            return DAS_S_OK;
        }
    };

    class ScopedBoundaryHook final
    {
    public:
        explicit ScopedBoundaryHook(FakeMaaApiBoundary& boundary)
        {
            SetMaaApiBoundaryForTest(&boundary);
        }

        ~ScopedBoundaryHook() { SetMaaApiBoundaryForTest(nullptr); }
    };
} // namespace

TEST(DasMaaPiRunTaskComponent, DoReturnsObjectNotInitWithoutSettings)
{
    MaapiRunTaskComponent component;

    DasPtr<ExportInterface::IDasPortMap> result;
    EXPECT_EQ(
        component.Do(nullptr, nullptr, result.Put()),
        DAS_E_OBJECT_NOT_INIT);
}

TEST(DasMaaPiRunTaskComponent, DoHandlesNullInputPortMap)
{
    MaapiRunTaskComponent component;
    FakeMaaApiBoundary    fake;
    ScopedBoundaryHook    hook(fake);

    auto settings = MakeSettingsJson(
        FixturePath("interface_v26_jsonc.jsonc").string(),
        "DailyFarm");
    DasPtr<ExportInterface::IDasJson> result_json;
    ASSERT_EQ(
        component.ApplySettingsChange(settings.Get(), result_json.Put()),
        DAS_S_OK);

    DasPtr<ExportInterface::IDasPortMap> result;
    auto hr = component.Do(nullptr, nullptr, result.Put());
    EXPECT_NE(hr, DAS_E_NO_IMPLEMENTATION);
    EXPECT_NE(hr, DAS_E_OBJECT_NOT_INIT);
}

TEST(DasMaaPiRunTaskComponent, ApplySettingsChangeStoresSettings)
{
    MaapiRunTaskComponent component;
    FakeMaaApiBoundary    fake;
    ScopedBoundaryHook    hook(fake);

    auto settings = MakeSettingsJson(
        FixturePath("interface_v26_jsonc.jsonc").string(),
        "DailyFarm");
    DasPtr<ExportInterface::IDasJson> result_json;
    ASSERT_EQ(
        component.ApplySettingsChange(settings.Get(), result_json.Put()),
        DAS_S_OK);
    ASSERT_TRUE(result_json);

    DasPtr<ExportInterface::IDasPortMap> result;
    auto hr = component.Do(nullptr, nullptr, result.Put());
    EXPECT_NE(hr, DAS_E_OBJECT_NOT_INIT);
}

TEST(DasMaaPiRunTaskComponent, DoCreatesOutputPortMap)
{
    MaapiRunTaskComponent component;
    FakeMaaApiBoundary    fake;
    ScopedBoundaryHook    hook(fake);

    auto settings = MakeSettingsJson(
        FixturePath("interface_v26_jsonc.jsonc").string(),
        "DailyFarm");
    DasPtr<ExportInterface::IDasJson> result_json;
    ASSERT_EQ(
        component.ApplySettingsChange(settings.Get(), result_json.Put()),
        DAS_S_OK);

    DasPtr<ExportInterface::IDasPortMap> result;
    auto hr = component.Do(nullptr, nullptr, result.Put());

    EXPECT_TRUE(result);
    if (result)
    {
        bool              has_completed = false;
        DasReadOnlyString completed_key("completedTasks");
        result->Has(completed_key.Get(), &has_completed);
        EXPECT_TRUE(has_completed);
    }
}

TEST(DasMaaPiRunTaskComponent, DoExecutesWithFakeBoundary)
{
    MaapiRunTaskComponent component;
    FakeMaaApiBoundary    fake;
    ScopedBoundaryHook    hook(fake);

    auto settings = MakeSettingsJson(
        FixturePath("interface_v26_jsonc.jsonc").string(),
        "DailyFarm");
    DasPtr<ExportInterface::IDasJson> result_json;
    ASSERT_EQ(
        component.ApplySettingsChange(settings.Get(), result_json.Put()),
        DAS_S_OK);

    DasPtr<ExportInterface::IDasPortMap> result;
    auto hr = component.Do(nullptr, nullptr, result.Put());

    EXPECT_TRUE(
        fake.Contains("PostTask:StartDaily:") || fake.Contains("CreateTasker"));
}

TEST(DasMaaPiRunTaskComponent, DoMapsCompletedTasksToOutputPortMap)
{
    MaapiRunTaskComponent component;
    FakeMaaApiBoundary    fake;
    ScopedBoundaryHook    hook(fake);

    auto settings = MakeSettingsJson(
        FixturePath("interface_v26_jsonc.jsonc").string(),
        "DailyFarm");
    DasPtr<ExportInterface::IDasJson> result_json;
    ASSERT_EQ(
        component.ApplySettingsChange(settings.Get(), result_json.Put()),
        DAS_S_OK);

    DasPtr<ExportInterface::IDasPortMap> result;
    auto hr = component.Do(nullptr, nullptr, result.Put());

    ASSERT_TRUE(result);
    IDasReadOnlyString* p_val = nullptr;
    DasReadOnlyString   completed_key("completedTasks");
    auto string_hr = result->GetString(completed_key.Get(), &p_val);
    if (DAS::IsOk(string_hr) && p_val)
    {
        const char* raw = nullptr;
        p_val->GetUtf8(&raw);
        EXPECT_NE(raw, nullptr);
        if (raw)
        {
            EXPECT_NE(std::string(raw).find('['), std::string::npos);
        }
        p_val->Release();
    }
}

TEST(DasMaaPiRunTaskComponent, DoPortMapInputMerging)
{
    MaapiRunTaskComponent component;
    FakeMaaApiBoundary    fake;
    ScopedBoundaryHook    hook(fake);

    auto settings_obj = Utils::MakeYyjsonObject();
    auto obj = settings_obj.as_object();
    (*obj)["piPath"] =
        yyjson::value(FixturePath("interface_v26_jsonc.jsonc").string());
    (*obj)["taskName"] = yyjson::value("DailyFarm");
    (*obj)["options"] = Utils::MakeYyjsonObject();

    auto port_map_obj = Utils::MakeYyjsonObject();
    auto pm = port_map_obj.as_object();
    (*pm)["stage"] = yyjson::value("stage");
    (*obj)["portMap"] = std::move(port_map_obj);

    auto settings_json = WrapJson(std::move(settings_obj));
    DasPtr<ExportInterface::IDasJson> result_json;
    ASSERT_EQ(
        component.ApplySettingsChange(settings_json.Get(), result_json.Put()),
        DAS_S_OK);

    DasPtr<ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    DasReadOnlyString stage_key("stage");
    DasReadOnlyString stage_val("1-1");
    input_map->SetString(stage_key.Get(), stage_val.Get());

    DasPtr<ExportInterface::IDasPortMap> output;
    auto hr = component.Do(nullptr, input_map.Get(), output.Put());

    EXPECT_NE(hr, DAS_E_NO_IMPLEMENTATION);
    EXPECT_NE(hr, DAS_E_OBJECT_NOT_INIT);
}
