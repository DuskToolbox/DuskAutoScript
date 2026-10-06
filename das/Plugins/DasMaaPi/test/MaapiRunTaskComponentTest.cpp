#include "../src/MaapiRunTaskComponent.h"
#include "../src/MaapiScheduleBrain.h"
#include "../src/PluginUtils.h"
#include "FakeMaaApiBoundary.h"

#include <das/Plugins/DasMaaPi/MaaPiErrorCodes.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasStringVector.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <ctime>
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
    // std::out_of_range，手搓 JSON 字符串极易漏键。schedule 由参数注入
    // （默认 off：kind=""）；pipeline_override 由参数注入（默认空对象 =
    // entry 编译期选项值形态）。
    std::string MakeEnvelopeJson(
        const MaaScheduleConfigDto& schedule = {},
        const yyjson::value&        pipeline_override = {})
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

        envelope.maapi.schedule = schedule;

        MaaTaskExecutionDto task;
        task.task_name = "E2ETestTask";
        task.entry = "StartE2E";
        task.pipeline_override =
            pipeline_override.is_object()
                ? Utils::CloneYyjsonValue(pipeline_override)
                : Utils::MakeYyjsonObject();
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

    // 空 PortMap（executionInput 缺席）构造器——缺线具名失败用例的输入面。
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

    // 跨平台 gmtime_s(Win32) / gmtime_r(POSIX)——UTC 字段期望值计算用
    // （D-B5 wire 契约的 UTC 语义断言）。
    std::tm* PortableGmtime(const std::time_t* timer, std::tm* buf)
    {
#ifdef _WIN32
        return gmtime_s(buf, timer) == 0 ? buf : nullptr;
#else
        return gmtime_r(timer, buf);
#endif
    }

    std::tm* PortableLocaltime(const std::time_t* timer, std::tm* buf)
    {
#ifdef _WIN32
        return localtime_s(buf, timer) == 0 ? buf : nullptr;
#else
        return localtime_r(timer, buf);
#endif
    }

    // 构造"本地 HH:MM:00 今天"的 unix 时间戳（fake clock 注入值）。
    std::time_t LocalTodayUnix(int hour, int minute)
    {
        std::time_t now = std::time(nullptr);
        std::tm     local{};
        PortableLocaltime(&now, &local);
        local.tm_hour = hour;
        local.tm_min = minute;
        local.tm_sec = 0;
        local.tm_isdst = -1;
        return std::mktime(&local);
    }

    // output PortMap 键集（GetKeys）是否含指定键——engaged-only 断言用
    // （off 不发 nextTime 键的键集级验证，比读值空串更强）。
    bool PortMapHasKey(
        ExportInterface::IDasReadOnlyPortMap* map,
        std::string_view                      port_id)
    {
        Das::DasPtr<ExportInterface::IDasStringVector> keys;
        if (DAS::IsFailed(map->GetKeys(keys.Put())) || !keys)
        {
            return false;
        }
        std::uint64_t count = 0;
        if (DAS::IsFailed(keys->Size(&count)))
        {
            return false;
        }
        for (std::uint64_t i = 0; i < count; ++i)
        {
            IDasReadOnlyString* p_key = nullptr;
            if (DAS::IsFailed(keys->At(i, &p_key)) || p_key == nullptr)
            {
                continue;
            }
            const char* utf8 = nullptr;
            p_key->GetUtf8(&utf8);
            const bool matched =
                utf8 != nullptr && std::string_view(utf8) == port_id;
            p_key->Release();
            if (matched)
            {
                return true;
            }
        }
        return false;
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

// 输入口缺 executionInput（空 PortMap）：遇缺即错——具名失败码
// DAS_E_MAAPI_EXECUTION_INPUT_MISSING，运行期零回退、零主进程服务查询，
// boundary 零调用。
TEST(DasMaaPiRunTaskComponent, DoWithoutExecutionInputReturnsMissingCode)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    DasPtr<ExportInterface::IDasPortMap> empty_map;
    ASSERT_EQ(CreateIDasPortMap(empty_map.Put()), DAS_S_OK);

    DasPtr<ExportInterface::IDasPortMap> result;
    const auto hr = component.Do(nullptr, empty_map.Get(), result.Put());

    EXPECT_EQ(hr, DAS_E_MAAPI_EXECUTION_INPUT_MISSING);
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
// entryId 整数）单键：ApplySettingsChange 接受，acceptedSettings 回显存储
// 值。
TEST(DasMaaPiRunTaskComponent, ApplySettingsChangeStoresRepositoryRef)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    auto settings = Utils::MakeYyjsonObject();
    (*settings.as_object())[std::string_view("repositoryRef")] = 7;

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
    EXPECT_EQ(accepted_obj->size(), 1u);
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

// settings 携带 repositoryRef 但 executionInput 端口缺线：同样遇缺即错
// （envelope 唯一来源 = 编排层执行时编译经输入口喂线，节点不再自查
// 仓库条目）——具名失败码与空 PortMap 情形一致，boundary 零调用。
TEST(DasMaaPiRunTaskComponent, DoWithRepositoryRefButMissingWireFailsLoudly)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    DasPtr<ExportInterface::IDasJson> settings_result;
    ASSERT_EQ(
        component.ApplySettingsChange(
            MakeSettingsChangeRequestFromText(R"({"repositoryRef": 5})").Get(),
            settings_result.Put()),
        DAS_S_OK);

    DasPtr<ExportInterface::IDasPortMap> result;
    const auto                           hr =
        component.Do(nullptr, MakeEmptyPortMap().Get(), result.Put());

    EXPECT_EQ(hr, DAS_E_MAAPI_EXECUTION_INPUT_MISSING);
    EXPECT_TRUE(fake.calls.empty());
}

// 输入 portmap 携带暴露口键（非保留键，键名 = 口 id = PI 参数名）——
// 恒等映射进 envelope 运行时输入段（maapi.tasks[].pipelineOverride 的
// 对象级覆盖）：标量四类（string/int/bool/float）全部经 PostTask 的
// override 串可观测。
TEST(DasMaaPiRunTaskComponent, DoMapsWiredExposedInputsToPipelineOverride)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    DasPtr<ExportInterface::IDasPortMap> input_map =
        MakeInputPortMap(MakeEnvelopeJson());
    input_map->SetString(
        DasReadOnlyString("exposed_text").Get(),
        DasReadOnlyString("wired-text").Get());
    input_map->SetInt(DasReadOnlyString("exposed_count").Get(), 7);
    input_map->SetBool(DasReadOnlyString("exposed_flag").Get(), true);
    input_map->SetFloat(DasReadOnlyString("exposed_ratio").Get(), 0.25);

    DasPtr<ExportInterface::IDasPortMap> result;
    const auto hr = component.Do(nullptr, input_map.Get(), result.Put());

    EXPECT_EQ(hr, DAS_S_OK);
    EXPECT_TRUE(fake.ContainsSubstring("PostTask:StartE2E:"));
    EXPECT_TRUE(fake.ContainsSubstring("wired-text"));
    EXPECT_TRUE(fake.ContainsSubstring("exposed_count"));
    EXPECT_TRUE(fake.ContainsSubstring("exposed_flag"));
    EXPECT_TRUE(fake.ContainsSubstring("exposed_ratio"));
}

// 同一参数键既有暴露口连线值又有 entry 编译期选项值 → 连线值胜出（覆盖
// 通道天然实现）——执行期参数覆盖两层语义：暴露口连线值 > entry 编译期
// 选项值，无第三来源。
TEST(DasMaaPiRunTaskComponent, DoWiredValueOverridesCompileTimeOption)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    auto compile_time_override = Utils::MakeYyjsonObject();
    (*compile_time_override.as_object())[std::string_view("target")] =
        yyjson::value(std::string("compile-time-value"));

    DasPtr<ExportInterface::IDasPortMap> input_map =
        MakeInputPortMap(MakeEnvelopeJson({}, compile_time_override));
    input_map->SetString(
        DasReadOnlyString("target").Get(),
        DasReadOnlyString("wired-value").Get());

    DasPtr<ExportInterface::IDasPortMap> result;
    const auto hr = component.Do(nullptr, input_map.Get(), result.Put());

    EXPECT_EQ(hr, DAS_S_OK);
    EXPECT_TRUE(fake.ContainsSubstring("wired-value"));
    EXPECT_FALSE(fake.ContainsSubstring("compile-time-value"));
}

// 口未连线（portmap 无该键）→ 编译期选项值生效（两层语义的另一半）。
TEST(DasMaaPiRunTaskComponent, DoUnwiredPortKeepsCompileTimeOption)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    auto compile_time_override = Utils::MakeYyjsonObject();
    (*compile_time_override.as_object())[std::string_view("target")] =
        yyjson::value(std::string("compile-time-value"));

    DasPtr<ExportInterface::IDasPortMap> input_map =
        MakeInputPortMap(MakeEnvelopeJson({}, compile_time_override));

    DasPtr<ExportInterface::IDasPortMap> result;
    const auto hr = component.Do(nullptr, input_map.Get(), result.Put());

    EXPECT_EQ(hr, DAS_S_OK);
    EXPECT_TRUE(fake.ContainsSubstring("compile-time-value"));
}

// 保留键 executionInput（产物运输专用）不参与映射：override 不携带该键
// （空 override 即证据——envelope 原文串未泄漏进传参通道）。
TEST(DasMaaPiRunTaskComponent, DoSkipsReservedExecutionInputKeyInOverride)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    DasPtr<ExportInterface::IDasPortMap> input_map =
        MakeInputPortMap(MakeEnvelopeJson());

    DasPtr<ExportInterface::IDasPortMap> result;
    const auto hr = component.Do(nullptr, input_map.Get(), result.Put());

    EXPECT_EQ(hr, DAS_S_OK);
    EXPECT_TRUE(fake.ContainsSubstring("PostTask:StartE2E:{}"));
}

// settings 携带 repositoryRef 且 executionInput 端口喂线：直接消费端口
// envelope（唯一来源契约锁定——repositoryRef 供编排层执行时编译识别节点
// 绑定的条目，运行期不参与供数）。
TEST(DasMaaPiRunTaskComponent, DoWithRepositoryRefConsumesExecutionInputPort)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

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
    EXPECT_TRUE(fake.ContainsSubstring("PostTask:StartE2E:"));
}

// D-B5：daily schedule 的 Do 成功后 nextTime 口值 == 共享 brain 助手计算值
// 的 ISO 8601 UTC 串（定长 19 字符逐字符；字段 == 该 unix 经 PortableGmtime
// 的 UTC 字段——UTC 语义断言，禁本地字段）。fake clock 固定 now，组件按
// 「本次 Do 刚执行（今日已 fired）」语义调助手 → daily 恒推明日 slot。
TEST(DasMaaPiRunTaskComponent, DoEmitsNextTimePortFromScheduleBrain)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    MaaScheduleConfigDto schedule;
    schedule.kind = "daily";
    schedule.daily_time = "09:00";
    schedule.daily_catch_up = false;

    const std::time_t now = LocalTodayUnix(8, 0);
    component.SetClockForTest([stamp = now] { return stamp; });

    DasPtr<ExportInterface::IDasPortMap> input_map =
        MakeInputPortMap(MakeEnvelopeJson(schedule));

    DasPtr<ExportInterface::IDasPortMap> result;
    const auto hr = component.Do(nullptr, input_map.Get(), result.Put());
    ASSERT_EQ(hr, DAS_S_OK);

    // 助手期望值：last_fired = 今日 ymd（Do 刚执行）→ 明日 09:00 slot。
    const auto expected =
        ComputeNextScheduleRun(schedule, now, EncodeLocalYmd(now));
    ASSERT_EQ(expected.status, ScheduleNextStatus::Ok);
    const std::string expected_text =
        FormatNextRunIso8601Utc(expected.next_unix);
    ASSERT_EQ(expected_text.size(), 19u);

    ASSERT_TRUE(result);
    EXPECT_TRUE(PortMapHasKey(result.Get(), "nextTime"));
    const auto next_time = ReadStringPort(result.Get(), "nextTime");

    // 值 == 助手计算值的 ISO 串（同 helper 同格式）。
    EXPECT_EQ(next_time, expected_text);

    // 定长格式逐字符：YYYY-MM-DDTHH:MM:SS 分隔符位精确。
    ASSERT_EQ(next_time.size(), 19u);
    EXPECT_EQ(next_time[4], '-');
    EXPECT_EQ(next_time[7], '-');
    EXPECT_EQ(next_time[10], 'T');
    EXPECT_EQ(next_time[13], ':');
    EXPECT_EQ(next_time[16], ':');

    // UTC 字段语义：串的日期时间字段 == 期望 unix 经 PortableGmtime 的 UTC
    // 字段（实现若误用本地字段，非 UTC 时区机器上必不相等——与 111-02 解析
    // 端 TimegmPortable 直填/DasDateToUnix 往返一致）。
    std::tm utc{};
    ASSERT_NE(PortableGmtime(&expected.next_unix, &utc), nullptr);
    char utc_text[20];
    ASSERT_EQ(
        std::snprintf(
            utc_text,
            sizeof(utc_text),
            "%04d-%02d-%02dT%02d:%02d:%02d",
            utc.tm_year + 1900,
            utc.tm_mon + 1,
            utc.tm_mday,
            utc.tm_hour,
            utc.tm_min,
            utc.tm_sec),
        19);
    EXPECT_EQ(next_time, std::string(utc_text));
}

// D-B5 engaged-only 纪律（Q4 裁决）：schedule=off 的 Do 成功后 output 键集
// 不含 nextTime（GetKeys 不含）——adapter（111-02）捕获不到即回落静态通道。
TEST(DasMaaPiRunTaskComponent, DoWithScheduleOffOmitsNextTimePort)
{
    MaapiRunTaskComponent    component;
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    component.SetClockForTest([stamp = LocalTodayUnix(8, 0)] { return stamp; });

    // 默认 schedule = off（kind=""）。
    DasPtr<ExportInterface::IDasPortMap> input_map =
        MakeInputPortMap(MakeEnvelopeJson());

    DasPtr<ExportInterface::IDasPortMap> result;
    const auto hr = component.Do(nullptr, input_map.Get(), result.Put());
    ASSERT_EQ(hr, DAS_S_OK);

    ASSERT_TRUE(result);
    EXPECT_FALSE(PortMapHasKey(result.Get(), "nextTime"));
}
