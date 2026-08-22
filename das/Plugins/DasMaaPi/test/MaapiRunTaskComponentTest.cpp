#include "../src/MaapiRunTaskComponent.h"
#include "../src/PluginUtils.h"
#include "FakeMaaApiBoundary.h"

#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace
{
    using namespace Das;
    using namespace Das::Plugins::DasMaaPi;
    using namespace Das::Plugins::DasMaaPi::Test;

    // envelope fixture 直接构造 ExecutionEnvelopeDto 并序列化——cpp_yyjson
    // 的 default_caster 经 PFR 对每个字段调 obj[key]，缺 key（如
    // focusTranslationsPath/schedule 即便为空也必须显式存在）在 cast 前抛
    // std::out_of_range，手搓 JSON 字符串极易漏键。
    std::string MakeEnvelopeJson()
    {
        ExecutionEnvelopeDto envelope;
        envelope.maapi.interface_directory = "C:/maa";
        envelope.maapi.controller_name = "Android";
        envelope.maapi.controller.name = "Android";
        envelope.maapi.controller.type = "Adb";
        envelope.maapi.controller.address = "127.0.0.1:5555";

        AdbInitParams adb;
        adb.address = "127.0.0.1:5555";
        ControllerInitParams controller_init_params;
        controller_init_params.controller_type = "Adb";
        controller_init_params.adb = std::move(adb);
        envelope.maapi.controller_init_params =
            std::move(controller_init_params);

        envelope.maapi.resource_name = "Official";
        envelope.maapi.resource_paths = {"C:/maa/resource"};
        // 与 FakeMaaApiBoundary::GetResourceHash 的默认返回一致，避免
        // resource hash mismatch 警告诊断混入断言面。
        envelope.maapi.resource_hash = "hash-expected";

        MaaTaskExecutionDto task;
        task.task_name = "E2ETestTask";
        task.entry = "StartE2E";
        task.pipeline_override = Utils::MakeYyjsonObject();
        envelope.maapi.tasks.push_back(std::move(task));

        const auto serialized = Utils::SerializeYyjsonValue(
            Utils::CloneYyjsonValue(yyjson::object(envelope)));
        return serialized.value_or("{}");
    }

    // 输入口构造器：executionInput 端口喂序列化的 envelope JSON——与
    // RepositoryInvoke 组件的喂线契约（SetPortString("executionInput",
    // serialize(snapshot.execution_input))）同键名同形状。
    DasPtr<ExportInterface::IDasPortMap> MakeInputPortMap(
        std::string_view execution_input_json)
    {
        DasPtr<ExportInterface::IDasPortMap> input_map;
        EXPECT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);

        const DasReadOnlyString key{"executionInput"};
        const DasReadOnlyString val{std::string{execution_input_json}.c_str()};
        input_map->SetString(key.Get(), val.Get());
        return input_map;
    }

    class ScopedBoundaryHook final
    {
    public:
        explicit ScopedBoundaryHook(FakeMaaApiBoundary& boundary)
        {
            SetMaaApiBoundaryForTest(&boundary);
        }

        ~ScopedBoundaryHook() { SetMaaApiBoundaryForTest(nullptr); }
    };

    std::string ReadStringPort(
        ExportInterface::IDasReadOnlyPortMap* map,
        std::string_view                      port_id)
    {
        IDasReadOnlyString*     p_str = nullptr;
        const DasReadOnlyString key{std::string{port_id}.c_str()};
        if (DAS::IsFailed(map->GetString(key.Get(), &p_str)) || !p_str)
        {
            return {};
        }
        const char* utf8 = nullptr;
        p_str->GetUtf8(&utf8);
        std::string value(utf8 != nullptr ? utf8 : "");
        p_str->Release();
        return value;
    }
} // namespace

// 输入口喂合法 envelope：经共享内核驱动 MaaRuntime，boundary 观测到
// PostTask:StartE2E，completedTasks/diagnostics 输出口可读。
TEST(DasMaaPiRunTaskComponent, DoExecutesEnvelopeFromExecutionInputPort)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    DasPtr<ExportInterface::IDasPortMap> input_map =
        MakeInputPortMap(MakeEnvelopeJson());

    DasPtr<ExportInterface::IDasPortMap> result;
    const auto hr = component.Do(nullptr, input_map.Get(), result.Put());

    EXPECT_EQ(hr, DAS_S_OK);
    EXPECT_TRUE(fake.ContainsSubstring("PostTask:StartE2E:"));
    EXPECT_TRUE(fake.Contains("CreateTasker"));

    ASSERT_TRUE(result);
    const auto completed_tasks = ReadStringPort(result.Get(), "completedTasks");
    EXPECT_NE(completed_tasks.find("E2ETestTask"), std::string::npos);
    EXPECT_NE(completed_tasks.find('['), std::string::npos);
    const auto diagnostics = ReadStringPort(result.Get(), "diagnostics");
    EXPECT_FALSE(diagnostics.empty());
}

// 输入口缺 executionInput（空 PortMap）：显性失败，无静默兜底。
TEST(DasMaaPiRunTaskComponent, DoWithoutExecutionInputReturnsInvalidArgument)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    DasPtr<ExportInterface::IDasPortMap> empty_map;
    ASSERT_EQ(CreateIDasPortMap(empty_map.Put()), DAS_S_OK);

    DasPtr<ExportInterface::IDasPortMap> result;
    const auto hr = component.Do(nullptr, empty_map.Get(), result.Put());

    EXPECT_EQ(hr, DAS_E_INVALID_ARGUMENT);
    EXPECT_TRUE(fake.calls.empty());
}

// executionInput 端口内容为非法 JSON：返回解析失败码，不调 boundary。
TEST(DasMaaPiRunTaskComponent, DoWithMalformedExecutionInputJsonFails)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    DasPtr<ExportInterface::IDasPortMap> input_map =
        MakeInputPortMap("not-valid-json");

    DasPtr<ExportInterface::IDasPortMap> result;
    const auto hr = component.Do(nullptr, input_map.Get(), result.Put());

    EXPECT_EQ(hr, DAS_E_INVALID_JSON);
    EXPECT_TRUE(fake.calls.empty());
}

// manifest settings:[] —— ApplySettingsChange 对任意输入返回
// {"acceptedSettings": {}}（空 accepted 是终态真语义，方法与响应形状保留
// 供 GraphRuntime Configure 阶段消费）。
TEST(DasMaaPiRunTaskComponent, ApplySettingsChangeReturnsEmptyAcceptedSettings)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    const auto request = WrapJson(
        []
        {
            auto root = Utils::MakeYyjsonObject();
            (*root.as_object())[std::string_view("settings")] =
                Utils::MakeYyjsonObject();
            return root;
        }());

    DasPtr<ExportInterface::IDasJson> result_json;
    ASSERT_EQ(
        component.ApplySettingsChange(request.Get(), result_json.Put()),
        DAS_S_OK);
    ASSERT_TRUE(result_json);

    const auto parsed = ReadJson(result_json.Get());
    ASSERT_TRUE(parsed);
    const auto obj = parsed->as_object();
    ASSERT_TRUE(obj);
    EXPECT_TRUE(obj->contains(std::string_view("acceptedSettings")));
    EXPECT_TRUE((*obj)[std::string_view("acceptedSettings")].is_object());
    EXPECT_EQ(
        (*obj)[std::string_view("acceptedSettings")].as_object()->size(),
        0u);
}

// 任务执行失败（MaaFW 任务 Failed）：输出口 diagnostics 带失败诊断，
// stopped=false。
TEST(DasMaaPiRunTaskComponent, DoPropagatesRuntimeFailureWithDiagnostics)
{
    FakeMaaApiBoundary fake;
    fake.wait_status_by_entry["StartE2E"] = MaaTaskStatus::Failed;
    const ScopedBoundaryHook hook(fake);

    MaapiRunTaskComponent component;

    DasPtr<ExportInterface::IDasPortMap> input_map =
        MakeInputPortMap(MakeEnvelopeJson());

    DasPtr<ExportInterface::IDasPortMap> result;
    const auto hr = component.Do(nullptr, input_map.Get(), result.Put());

    EXPECT_TRUE(DAS::IsFailed(hr));
    ASSERT_TRUE(result);
    const auto diagnostics = ReadStringPort(result.Get(), "diagnostics");
    EXPECT_NE(diagnostics.find('['), std::string::npos);
    bool                    stopped = true;
    const DasReadOnlyString stopped_key("stopped");
    ASSERT_EQ(result->GetBool(stopped_key.Get(), &stopped), DAS_S_OK);
    EXPECT_FALSE(stopped);
}
