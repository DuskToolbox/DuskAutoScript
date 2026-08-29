#include "../src/MaapiRunTaskComponent.h"
#include "../src/PluginUtils.h"
#include "FakeMaaApiBoundary.h"

#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

    // 仓库解析缝注入 RAII：测试体内 fake resolver，析构恢复生产实现。
    class ScopedRepositoryResolverHook final
    {
    public:
        explicit ScopedRepositoryResolverHook(RepositoryEntryResolver resolver)
        {
            SetRepositoryEntryResolverForTest(std::move(resolver));
        }

        ~ScopedRepositoryResolverHook()
        {
            SetRepositoryEntryResolverForTest(nullptr);
        }
    };

    // 空 PortMap（executionInput 缺席）构造器——repositoryRef 分支的输入面。
    DasPtr<ExportInterface::IDasPortMap> MakeEmptyPortMap()
    {
        DasPtr<ExportInterface::IDasPortMap> input_map;
        EXPECT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
        return input_map;
    }

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

// ApplySettingsChange settings 包装请求构造器：{"settings": <settings>}——
// 与 GraphRuntime Configure 阶段的投递形状一致。
DasPtr<ExportInterface::IDasJson> MakeSettingsChangeRequest(
    const yyjson::value& settings)
{
    auto root = Utils::MakeYyjsonObject();
    (*root.as_object())[std::string_view("settings")] =
        Utils::CloneYyjsonValue(settings);
    return WrapJson(std::move(root));
}

DasPtr<ExportInterface::IDasJson> MakeSettingsChangeRequestFromText(
    std::string_view settings_json)
{
    auto parsed = Utils::ParseYyjsonFromString(std::string{settings_json});
    EXPECT_TRUE(parsed.has_value());
    return MakeSettingsChangeRequest(
        parsed.value_or(Utils::MakeYyjsonObject()));
}

// 读 acceptedSettings 对象（失败返回 nullopt——响应形状由调用方先断言）。
std::optional<yyjson::value> ReadAcceptedSettings(
    ExportInterface::IDasJson* result_json)
{
    const auto parsed = ReadJson(result_json);
    if (!parsed)
    {
        return std::nullopt;
    }
    auto obj = parsed->as_object();
    if (!obj || !obj->contains(std::string_view("acceptedSettings")))
    {
        return std::nullopt;
    }
    auto accepted = (*obj)[std::string_view("acceptedSettings")].as_object();
    if (!accepted)
    {
        return std::nullopt;
    }
    return Das::Utils::CloneYyjsonValue(
        (*obj)[std::string_view("acceptedSettings")]);
}

// manifest config 声明 repositoryRef（type=taskRepositoryRef，值 = 纯
// entryId 整数）与 runtimeInputs（object）两键：ApplySettingsChange 接受并
// 存储两键，acceptedSettings 回显存储值。
TEST(
    DasMaaPiRunTaskComponent,
    ApplySettingsChangeStoresRepositoryRefAndRuntimeInputs)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    auto runtime_inputs = Utils::MakeYyjsonObject();
    (*runtime_inputs.as_object())[std::string_view("x")] = 1;

    auto settings = Utils::MakeYyjsonObject();
    (*settings.as_object())[std::string_view("repositoryRef")] = 7;
    (*settings.as_object())[std::string_view("runtimeInputs")] =
        std::move(runtime_inputs);

    const auto request = MakeSettingsChangeRequest(settings);

    DasPtr<ExportInterface::IDasJson> result_json;
    ASSERT_EQ(
        component.ApplySettingsChange(request.Get(), result_json.Put()),
        DAS_S_OK);
    ASSERT_TRUE(result_json);

    const auto accepted = ReadAcceptedSettings(result_json.Get());
    ASSERT_TRUE(accepted.has_value());
    auto accepted_obj = accepted->as_object();
    ASSERT_TRUE(accepted_obj);
    ASSERT_TRUE(accepted_obj->contains(std::string_view("repositoryRef")));
    EXPECT_EQ(
        (*accepted_obj)[std::string_view("repositoryRef")].as_int().value_or(
            -1),
        7);
    ASSERT_TRUE(accepted_obj->contains(std::string_view("runtimeInputs")));
    auto inputs_obj =
        (*accepted_obj)[std::string_view("runtimeInputs")].as_object();
    ASSERT_TRUE(inputs_obj);
    EXPECT_EQ((*inputs_obj)[std::string_view("x")].as_int().value_or(-1), 1);
}

// repositoryRef 非纯整数（旧 {kind,entryId} 包装形状/字符串/布尔）→ 该键
// 拒绝：返回失败诊断，不落半截状态（后续合法投递照常生效）。
TEST(
    DasMaaPiRunTaskComponent,
    ApplySettingsChangeRejectsObjectShapedRepositoryRef)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    DasPtr<ExportInterface::IDasJson> result_json;
    EXPECT_EQ(
        component.ApplySettingsChange(
            MakeSettingsChangeRequestFromText(
                R"({"repositoryRef": {"kind": "taskRepositoryRef", "entryId": 7}})")
                .Get(),
            result_json.Put()),
        DAS_E_INVALID_ARGUMENT);

    // 字符串 / 布尔同样拒绝（值域 = 纯正整数或 null/缺键）。
    EXPECT_EQ(
        component.ApplySettingsChange(
            MakeSettingsChangeRequestFromText(R"({"repositoryRef": "7"})")
                .Get(),
            result_json.Put()),
        DAS_E_INVALID_ARGUMENT);
    EXPECT_EQ(
        component.ApplySettingsChange(
            MakeSettingsChangeRequestFromText(R"({"repositoryRef": true})")
                .Get(),
            result_json.Put()),
        DAS_E_INVALID_ARGUMENT);

    // 拒绝后不落半截状态：合法投递照常接受。
    EXPECT_EQ(
        component.ApplySettingsChange(
            MakeSettingsChangeRequestFromText(R"({"repositoryRef": 11})").Get(),
            result_json.Put()),
        DAS_S_OK);
    const auto accepted = ReadAcceptedSettings(result_json.Get());
    ASSERT_TRUE(accepted.has_value());
    auto accepted_obj = accepted->as_object();
    ASSERT_TRUE(accepted_obj);
    EXPECT_EQ(
        (*accepted_obj)[std::string_view("repositoryRef")].as_int().value_or(
            -1),
        11);
}

// repositoryRef 0 / 负数（非正整数）显式拒绝——0 内部保留为「未设置」。
TEST(
    DasMaaPiRunTaskComponent,
    ApplySettingsChangeRejectsNonPositiveRepositoryRef)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    DasPtr<ExportInterface::IDasJson> result_json;
    EXPECT_EQ(
        component.ApplySettingsChange(
            MakeSettingsChangeRequestFromText(R"({"repositoryRef": 0})").Get(),
            result_json.Put()),
        DAS_E_INVALID_ARGUMENT);
    EXPECT_EQ(
        component.ApplySettingsChange(
            MakeSettingsChangeRequestFromText(R"({"repositoryRef": -3})").Get(),
            result_json.Put()),
        DAS_E_INVALID_ARGUMENT);
}

// runtimeInputs 非对象（数组/标量）→ 拒绝；repositoryRef 合法也不落半截。
TEST(DasMaaPiRunTaskComponent, ApplySettingsChangeRejectsNonObjectRuntimeInputs)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    DasPtr<ExportInterface::IDasJson> result_json;
    EXPECT_EQ(
        component.ApplySettingsChange(
            MakeSettingsChangeRequestFromText(
                R"({"repositoryRef": 7, "runtimeInputs": [1, 2]})")
                .Get(),
            result_json.Put()),
        DAS_E_INVALID_ARGUMENT);
    EXPECT_EQ(
        component.ApplySettingsChange(
            MakeSettingsChangeRequestFromText(
                R"({"repositoryRef": 7, "runtimeInputs": 5})")
                .Get(),
            result_json.Put()),
        DAS_E_INVALID_ARGUMENT);

    // 上一轮整包拒绝后未存储：空投递回显空 accepted。
    EXPECT_EQ(
        component.ApplySettingsChange(
            MakeSettingsChangeRequestFromText(R"({})").Get(),
            result_json.Put()),
        DAS_S_OK);
    const auto accepted = ReadAcceptedSettings(result_json.Get());
    ASSERT_TRUE(accepted.has_value());
    EXPECT_EQ(accepted->as_object()->size(), 0u);
}

// 连续两次投递存储为最新值（幂等覆盖，全量替换语义）。
TEST(DasMaaPiRunTaskComponent, ApplySettingsChangeOverwritesWithLatestValues)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    DasPtr<ExportInterface::IDasJson> result_json;
    ASSERT_EQ(
        component.ApplySettingsChange(
            MakeSettingsChangeRequestFromText(R"({"repositoryRef": 7})").Get(),
            result_json.Put()),
        DAS_S_OK);
    ASSERT_EQ(
        component.ApplySettingsChange(
            MakeSettingsChangeRequestFromText(R"({"repositoryRef": 9})").Get(),
            result_json.Put()),
        DAS_S_OK);

    const auto accepted = ReadAcceptedSettings(result_json.Get());
    ASSERT_TRUE(accepted.has_value());
    auto accepted_obj = accepted->as_object();
    ASSERT_TRUE(accepted_obj);
    EXPECT_EQ(
        (*accepted_obj)[std::string_view("repositoryRef")].as_int().value_or(
            -1),
        9);
    // 全量替换：第二次投递未携带 runtimeInputs → 不回显。
    EXPECT_FALSE(accepted_obj->contains(std::string_view("runtimeInputs")));
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

// executionInput 缺失 + repositoryRef 有效：Do 经解析缝取编译 envelope，
// 自行执行（invokeRepository 逻辑内化——用户不手动包节点）；输出端口
// 三键照常填充。
TEST(DasMaaPiRunTaskComponent, DoResolvesRepositoryEntryWhenInputPortMissing)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    int64_t                            resolved_entry_id = -1;
    const ScopedRepositoryResolverHook resolver_hook(
        [&resolved_entry_id](int64_t entry_id)
        {
            resolved_entry_id = entry_id;
            RepositoryResolveOutcome outcome;
            outcome.ok = true;
            outcome.envelope_json = MakeEnvelopeJson();
            return outcome;
        });

    DasPtr<ExportInterface::IDasJson> settings_result;
    ASSERT_EQ(
        component.ApplySettingsChange(
            MakeSettingsChangeRequestFromText(R"({"repositoryRef": 5})").Get(),
            settings_result.Put()),
        DAS_S_OK);

    DasPtr<ExportInterface::IDasPortMap> result;
    const auto                           hr =
        component.Do(nullptr, MakeEmptyPortMap().Get(), result.Put());

    EXPECT_EQ(hr, DAS_S_OK);
    EXPECT_EQ(resolved_entry_id, 5);
    EXPECT_TRUE(fake.ContainsSubstring("PostTask:StartE2E:"));

    ASSERT_TRUE(result);
    const auto completed_tasks = ReadStringPort(result.Get(), "completedTasks");
    EXPECT_NE(completed_tasks.find("E2ETestTask"), std::string::npos);
    const auto diagnostics = ReadStringPort(result.Get(), "diagnostics");
    EXPECT_FALSE(diagnostics.empty());
}

// 解析失败：Do 返回失败 DasResult，diagnostics 携带可读原因（不崩、不
// 静默成功），boundary 零调用。
TEST(DasMaaPiRunTaskComponent, DoRepositoryResolveFailureReturnsFail)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    const ScopedRepositoryResolverHook resolver_hook(
        [](int64_t)
        {
            RepositoryResolveOutcome outcome;
            outcome.ok = false;
            outcome.error_message = "repository entry exploded";
            return outcome;
        });

    DasPtr<ExportInterface::IDasJson> settings_result;
    ASSERT_EQ(
        component.ApplySettingsChange(
            MakeSettingsChangeRequestFromText(R"({"repositoryRef": 5})").Get(),
            settings_result.Put()),
        DAS_S_OK);

    DasPtr<ExportInterface::IDasPortMap> result;
    const auto                           hr =
        component.Do(nullptr, MakeEmptyPortMap().Get(), result.Put());

    EXPECT_TRUE(DAS::IsFailed(hr));
    EXPECT_TRUE(fake.calls.empty());
    ASSERT_TRUE(result);
    const auto diagnostics = ReadStringPort(result.Get(), "diagnostics");
    EXPECT_NE(diagnostics.find("repository entry exploded"), std::string::npos);
}

// runtimeInputs 覆盖合并：envelope 的运行时输入段（maapi.tasks[].pipeline
// 前对象级覆盖）被 settings.runtimeInputs 合并后执行——合并键进
// PostTask 的 pipeline_override。
TEST(DasMaaPiRunTaskComponent, DoMergesRuntimeInputsIntoResolvedEnvelope)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    const ScopedRepositoryResolverHook resolver_hook(
        [](int64_t)
        {
            RepositoryResolveOutcome outcome;
            outcome.ok = true;
            outcome.envelope_json = MakeEnvelopeJson();
            return outcome;
        });

    DasPtr<ExportInterface::IDasJson> settings_result;
    ASSERT_EQ(
        component.ApplySettingsChange(
            MakeSettingsChangeRequestFromText(
                R"({"repositoryRef": 5, "runtimeInputs": {"custom_flag": true}})")
                .Get(),
            settings_result.Put()),
        DAS_S_OK);

    DasPtr<ExportInterface::IDasPortMap> result;
    const auto                           hr =
        component.Do(nullptr, MakeEmptyPortMap().Get(), result.Put());

    EXPECT_EQ(hr, DAS_S_OK);
    EXPECT_TRUE(fake.ContainsSubstring("PostTask:StartE2E:"));
    // 合并语义：runtimeInputs 的键覆盖进任务的 pipeline_override——
    // boundary 记录的 PostTask 调用携带该键。
    EXPECT_TRUE(fake.ContainsSubstring("custom_flag"));
}

// executionInput 端口有值时喂线路径优先：即使 repositoryRef 已设置也不走
// 解析缝（既有喂线路径零回归）。
TEST(DasMaaPiRunTaskComponent, DoPrefersExecutionInputPortOverRepositoryRef)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    bool                               resolver_called = false;
    const ScopedRepositoryResolverHook resolver_hook(
        [&resolver_called](int64_t)
        {
            resolver_called = true;
            RepositoryResolveOutcome outcome;
            outcome.ok = true;
            outcome.envelope_json = MakeEnvelopeJson();
            return outcome;
        });

    DasPtr<ExportInterface::IDasJson> settings_result;
    ASSERT_EQ(
        component.ApplySettingsChange(
            MakeSettingsChangeRequestFromText(R"({"repositoryRef": 5})").Get(),
            settings_result.Put()),
        DAS_S_OK);

    DasPtr<ExportInterface::IDasPortMap> input_map =
        MakeInputPortMap(MakeEnvelopeJson());
    DasPtr<ExportInterface::IDasPortMap> result;
    const auto hr = component.Do(nullptr, input_map.Get(), result.Put());

    EXPECT_EQ(hr, DAS_S_OK);
    EXPECT_FALSE(resolver_called);
    EXPECT_TRUE(fake.ContainsSubstring("PostTask:StartE2E:"));
}
