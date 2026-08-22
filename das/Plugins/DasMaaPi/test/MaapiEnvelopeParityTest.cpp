#include "../src/PluginUtils.h"
#include "MaapiAuthoringSession.h"

#include <das/Core/TaskScheduler/RepositoryInvokeCompiler.h>
#include <das/Core/TaskScheduler/TaskAuthoringContract.h>
#include <das/Plugins/DasMaaPi/AuthoringProjector.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// envelope 逐字节等价验收：同一份 acceptedProperties 走两条真实编译管道——
// 调度器直跑路径（session Compile purpose=execution → compile.executionInput）
// 与 graph 引用路径（RepositoryInvokeCompiler 快照 execution_input）——断言
// 序列化字节相等，锁定「一处编译、处处消费」不变量。两条管道都以真实
// MaapiAuthoringSession::Compile 为编译器，不 mock 编译本身。

namespace
{
    using namespace Das;
    using namespace Das::Core::TaskScheduler;
    using namespace Das::Plugins::DasMaaPi;

    // 被引仓库条目的固定身份（与真实仓库条目同空间同形状）。
    constexpr int64_t FIXTURE_ENTRY_ID = 25;
    constexpr int64_t FIXTURE_ENTRY_REVISION = 7;

    // maapi run 执行组件（manifest DasMaaPi.json 声明的 componentGuid）。
    const std::string MAAPI_RUN_COMPONENT_GUID_TEXT =
        "69F20008-0000-4000-8000-000000000001";

    std::filesystem::path FixturePath(std::string_view name)
    {
        return std::filesystem::current_path() / "DasMaaPi" / "test"
               / "fixtures" / std::filesystem::path{name};
    }

    // 同一份 acceptedProperties：interface_compile fixture + 与既有编译测试
    // 相同的用户选择集（difficulty=hard / server 开 / controller-mode=safe /
    // inactive=skip / DailyFarm retry+medicine+notify）。两条管道都从这一份
    // JSON 构造各自的 session——与生产两侧（任务实例存储 / 仓库条目存储）
    // 持有同一持久化形态一致。
    std::string MakeAcceptedPropertiesJson(std::string_view interface_path)
    {
        return R"({"adapter":{"interfacePath":")" + std::string(interface_path)
               + R"(","executionPolicy":{"failFast":true}},"pi":{)"
                 "\"controllerName\":\"Android\","
                 "\"resourceName\":\"Official\","
                 "\"globalOptions\":[{\"optionName\":\"difficulty\","
                 "\"kind\":\"select\",\"selectedCases\":[\"hard\"]}],"
                 "\"resourceOptions\":[{\"optionName\":\"server\","
                 "\"kind\":\"switch\",\"boolValue\":true}],"
                 "\"controllerOptions\":[{\"optionName\":\"controller-mode\","
                 "\"kind\":\"select\",\"selectedCases\":[\"safe\"]},"
                 "{\"optionName\":\"inactive\",\"kind\":\"select\","
                 "\"selectedCases\":[\"skip\"]}],"
                 "\"tasks\":[{\"taskName\":\"DailyFarm\",\"enabled\":true,"
                 "\"options\":[{\"optionName\":\"retry\",\"kind\":\"input\","
                 "\"inputValues\":{\"times\":\"7\"}},"
                 "{\"optionName\":\"medicine\",\"kind\":\"checkbox\","
                 "\"selectedCases\":[\"large\",\"small\"]},"
                 "{\"optionName\":\"notify\",\"kind\":\"switch\","
                 "\"boolValue\":true}]}]}}";
    }

    // 生产 session factory 的解析语义：上下文 {properties: ...} →
    // ParseAcceptedSettings。两条管道共用此函数，保证同参数构造。
    AcceptedSettingsDto SettingsFromProperties(const yyjson::value& properties)
    {
        yyjson::value session_context(Utils::MakeYyjsonObject());
        auto          context_object = session_context.as_object();
        (*context_object)[std::string_view("properties")] =
            Utils::CloneYyjsonValue(properties);
        return ParseAcceptedSettings(session_context);
    }

    // 调度器 MakeExecutionCompileRequest 同款请求体。
    yyjson::value MakeExecutionCompileRequestValue()
    {
        yyjson::value request(Utils::MakeYyjsonObject());
        (*request.as_object())[std::string_view("purpose")] = "execution";
        return request;
    }

    // 每次经 session Compile 的请求审计（复用证明：两路的请求字节一致）。
    struct SessionCompileAudit
    {
        std::vector<std::string> compile_request_jsons;
    };

    struct SessionCompileOutcome
    {
        bool          ok = false;
        yyjson::value compile;
        yyjson::value diagnostics;
        std::string   error_kind;
        std::string   error_message;
    };

    // 调度器直跑路径的请求/提取语义：session Compile → 严格信封校验
    // （ParseAuthoringProviderCompile 同款）→ unwrap compile 载荷。
    // graph 引用路径的 compile_authoring 回调复用本函数——两路的 Provider
    // 调用形态在代码层即同源。
    SessionCompileOutcome CompileOnSession(
        MaapiAuthoringSession& session,
        const yyjson::value&   request,
        SessionCompileAudit*   audit)
    {
        if (audit != nullptr)
        {
            auto request_text = Utils::SerializeYyjsonValue(request);
            if (request_text)
            {
                audit->compile_request_jsons.push_back(*request_text);
            }
        }

        const auto request_json = WrapJson(Utils::CloneYyjsonValue(request));
        DasPtr<ExportInterface::IDasJson> result_json;
        const auto                        compile_result =
            session.Compile(request_json.Get(), result_json.Put());

        SessionCompileOutcome outcome;
        if (DAS::IsFailed(compile_result) || !result_json)
        {
            outcome.error_kind = "compile-request-failed";
            return outcome;
        }

        const auto envelope = ReadJson(result_json.Get());
        if (!envelope)
        {
            outcome.error_kind = "compile-envelope-unreadable";
            return outcome;
        }
        const auto validation = ValidateAuthoringCompileResult(*envelope);
        if (!validation.valid)
        {
            outcome.error_kind = validation.error_kind;
            outcome.error_message = validation.message;
            return outcome;
        }

        const auto envelope_object = envelope->as_object();
        outcome.compile = Utils::CloneYyjsonValue(
            (*envelope_object)[std::string_view("compile")]);
        outcome.diagnostics = Utils::CloneYyjsonValue(
            (*envelope_object)[std::string_view("diagnostics")]);
        outcome.ok = true;
        return outcome;
    }

    // ExtractProviderExecutionInput 同款提取：compile["executionInput"]。
    yyjson::value ExtractExecutionInputValue(const yyjson::value& compile)
    {
        auto compile_object = compile.as_object();
        if (compile_object
            && compile_object->contains(std::string_view("executionInput")))
        {
            return Utils::CloneYyjsonValue(
                (*compile_object)[std::string_view("executionInput")]);
        }
        return yyjson::value{};
    }

    std::optional<std::string> SerializeCompact(const yyjson::value& value)
    {
        return Utils::SerializeYyjsonValue(Utils::CloneYyjsonValue(value));
    }

    struct ParityServicesAudit
    {
        int                 compile_authoring_calls = 0;
        SessionCompileAudit session_audit;
    };

    // 手组 graph 引用路径的三个服务回调，形态照生产装配
    // （SchedulerService::BuildRepositoryInvokeCompileServices）：load_entry
    // 返回 fixture 条目、find_execution_component 返回 maapi run 组件、
    // compile_authoring 以同一份 acceptedProperties 经同一
    // SettingsFromProperties + CompileOnSession 驱动 Provider 编译。
    RepositoryInvoke::RepositoryInvokeCompileServices MakeParityCompileServices(
        const yyjson::value& properties,
        ParityServicesAudit* audit)
    {
        RepositoryInvoke::RepositoryInvokeCompileServices services;

        services.load_entry = [properties](int64_t entry_id)
            -> std::optional<Repository::Dto::RepositoryEntryDto>
        {
            if (entry_id != FIXTURE_ENTRY_ID)
            {
                return std::nullopt;
            }
            Repository::Dto::RepositoryEntryDto entry;
            entry.entry_id = FIXTURE_ENTRY_ID;
            entry.plugin_guid = std::string(PLUGIN_GUID_TEXT);
            entry.task_type_guid = std::string(TASK_GUID_TEXT);
            entry.authoring.revision = FIXTURE_ENTRY_REVISION;
            entry.accepted_properties = Utils::CloneYyjsonValue(properties);
            return entry;
        };

        services.find_execution_component =
            [](const Repository::Dto::RepositoryEntryDto& /*entry*/)
            -> std::optional<std::string>
        { return MAAPI_RUN_COMPONENT_GUID_TEXT; };

        services.compile_authoring =
            [audit](
                const Repository::Dto::RepositoryEntryDto& entry,
                const yyjson::value&                       request)
            -> RepositoryInvoke::RepositoryInvokeProviderCompileResult
        {
            if (audit != nullptr)
            {
                ++audit->compile_authoring_calls;
            }
            // 与调度器直跑路径同参数新建 session（同一份 acceptedProperties
            // + 同一 revision 来源），Provider 调用复用 CompileOnSession。
            MaapiAuthoringSession session(
                SettingsFromProperties(entry.accepted_properties),
                entry.authoring.revision);
            auto compiled = CompileOnSession(
                session,
                request,
                audit != nullptr ? &audit->session_audit : nullptr);

            RepositoryInvoke::RepositoryInvokeProviderCompileResult result;
            result.ok = compiled.ok;
            if (!compiled.ok)
            {
                RepositoryInvoke::RepositoryInvokeCompileDiagnostic diagnostic;
                diagnostic.code = "provider-compile-failed";
                diagnostic.message =
                    compiled.error_kind + ": " + compiled.error_message;
                result.diagnostics.push_back(std::move(diagnostic));
                return result;
            }
            result.compile_result = std::move(compiled.compile);
            return result;
        };

        return services;
    }

    RepositoryInvoke::Dto::RepositoryTaskRefDto MakeRepositoryRef(
        std::optional<int64_t> expected_revision)
    {
        RepositoryInvoke::Dto::RepositoryTaskRefDto repository_ref;
        repository_ref.kind = "taskRepositoryRef";
        repository_ref.entry_id = FIXTURE_ENTRY_ID;
        repository_ref.expected_revision = expected_revision;
        return repository_ref;
    }

    yyjson::value ParsePropertiesOrEmpty()
    {
        const auto properties_text = MakeAcceptedPropertiesJson(
            FixturePath("interface_compile.jsonc").generic_string());
        auto properties = Utils::ParseYyjsonFromString(properties_text);
        if (!properties)
        {
            return Utils::MakeYyjsonObject();
        }
        return std::move(*properties);
    }
} // namespace

// 观测点 1 主断言：两条真实编译管道对同一份 acceptedProperties 产出的
// envelope 序列化字节逐字节相等（直接 ==，无字段级宽松比较）。不等即
// graph 引用链路引入了有损变换——真 bug，不得放宽断言。
TEST(
    MaapiEnvelopeParity,
    SchedulerAndGraphPathsSerializeIdenticalExecutionInput)
{
    const auto properties = ParsePropertiesOrEmpty();

    // A 路（调度器直跑）：同参数新建 session → purpose=execution 请求 →
    // 严格信封解包 → compile["executionInput"]。
    MaapiAuthoringSession scheduler_session(
        SettingsFromProperties(properties),
        FIXTURE_ENTRY_REVISION);
    const auto scheduler_compiled = CompileOnSession(
        scheduler_session,
        MakeExecutionCompileRequestValue(),
        nullptr);
    ASSERT_TRUE(scheduler_compiled.ok)
        << "error_kind=" << scheduler_compiled.error_kind
        << " message=" << scheduler_compiled.error_message;
    const auto scheduler_execution_input =
        ExtractExecutionInputValue(scheduler_compiled.compile);
    ASSERT_FALSE(scheduler_execution_input.is_null())
        << "调度器路径 compile 载荷缺 executionInput";

    // B 路（graph 引用）：真实 RepositoryInvokeCompiler 经三回调解析快照。
    const auto services = MakeParityCompileServices(properties, nullptr);
    const auto resolved = RepositoryInvoke::ResolveRepositoryInvokeSnapshot(
        services,
        MakeRepositoryRef(FIXTURE_ENTRY_REVISION));
    ASSERT_TRUE(resolved.ok);
    ASSERT_TRUE(resolved.snapshot.has_value());

    const auto scheduler_serialized =
        SerializeCompact(scheduler_execution_input);
    const auto graph_serialized =
        SerializeCompact(resolved.snapshot->execution_input);
    ASSERT_TRUE(scheduler_serialized.has_value());
    ASSERT_TRUE(graph_serialized.has_value());
    EXPECT_EQ(*scheduler_serialized, *graph_serialized)
        << "两条管道的 envelope 序列化字节必须逐字节相等";
}

// 复用证明：graph 路径的 compile_authoring 经与调度器路径完全相同的
// session 构造（同参数）与 Provider 调用驱动编译；两路发给 session 的
// 请求字节一致，快照元数据（revision/plugin/task/component）与 fixture
// 条目一致。
TEST(MaapiEnvelopeParity, GraphPathReusesSchedulerSessionCompileSemantics)
{
    const auto properties = ParsePropertiesOrEmpty();

    ParityServicesAudit audit;

    MaapiAuthoringSession scheduler_session(
        SettingsFromProperties(properties),
        FIXTURE_ENTRY_REVISION);
    const auto scheduler_compiled = CompileOnSession(
        scheduler_session,
        MakeExecutionCompileRequestValue(),
        &audit.session_audit);
    ASSERT_TRUE(scheduler_compiled.ok);

    const auto services = MakeParityCompileServices(properties, &audit);
    const auto resolved = RepositoryInvoke::ResolveRepositoryInvokeSnapshot(
        services,
        MakeRepositoryRef(FIXTURE_ENTRY_REVISION));
    ASSERT_TRUE(resolved.ok);
    ASSERT_TRUE(resolved.snapshot.has_value());

    // 两路各发起一次 session Compile，请求体字节一致（B 路请求由
    // RepositoryInvokeCompiler 内部构造后经服务回调透传）。
    ASSERT_EQ(audit.session_audit.compile_request_jsons.size(), 2u);
    EXPECT_EQ(
        audit.session_audit.compile_request_jsons[0],
        audit.session_audit.compile_request_jsons[1]);
    EXPECT_EQ(
        audit.session_audit.compile_request_jsons[0],
        R"({"purpose":"execution"})");

    // 快照元数据与 fixture 条目一致（revision 钉住 plumbing 无漂移）。
    EXPECT_EQ(resolved.snapshot->source_entry_id, FIXTURE_ENTRY_ID);
    EXPECT_EQ(resolved.snapshot->source_revision, FIXTURE_ENTRY_REVISION);
    EXPECT_EQ(resolved.snapshot->plugin_guid, std::string(PLUGIN_GUID_TEXT));
    EXPECT_EQ(resolved.snapshot->task_type_guid, std::string(TASK_GUID_TEXT));
    EXPECT_EQ(resolved.snapshot->component_guid, MAAPI_RUN_COMPONENT_GUID_TEXT);
}

// revision 钉住语义（编译墙既有行为回归）：expected_revision 与条目
// revision 不一致时 graph 路径失败（revisionConflict 诊断），快照不产出，
// 且失败发生在 Provider 编译之前（compile_authoring 零调用）。
TEST(MaapiEnvelopeParity, RevisionMismatchFailsClosedBeforeProviderCompile)
{
    const auto properties = ParsePropertiesOrEmpty();

    ParityServicesAudit audit;
    const auto services = MakeParityCompileServices(properties, &audit);
    const auto resolved = RepositoryInvoke::ResolveRepositoryInvokeSnapshot(
        services,
        MakeRepositoryRef(FIXTURE_ENTRY_REVISION + 1));

    EXPECT_FALSE(resolved.ok);
    EXPECT_FALSE(resolved.snapshot.has_value());
    ASSERT_EQ(resolved.diagnostics.size(), 1u);
    EXPECT_EQ(resolved.diagnostics[0].code, "repository-revision-mismatch");
    EXPECT_EQ(
        resolved.diagnostics[0].current_revision.value_or(-1),
        FIXTURE_ENTRY_REVISION);
    EXPECT_EQ(
        resolved.diagnostics[0].expected_revision.value_or(-1),
        FIXTURE_ENTRY_REVISION + 1);
    EXPECT_EQ(audit.compile_authoring_calls, 0);
}

// 干净 fixture 前提：两路 diagnostics 均为空——信封级与编译载荷级都是。
// 非空说明测试自身 fixture 有误（脏选择或环境差异），先修 fixture。
TEST(MaapiEnvelopeParity, CleanFixtureProducesEmptyDiagnosticsOnBothPaths)
{
    const auto properties = ParsePropertiesOrEmpty();

    MaapiAuthoringSession scheduler_session(
        SettingsFromProperties(properties),
        FIXTURE_ENTRY_REVISION);
    const auto scheduler_compiled = CompileOnSession(
        scheduler_session,
        MakeExecutionCompileRequestValue(),
        nullptr);
    ASSERT_TRUE(scheduler_compiled.ok);

    const auto envelope_diagnostics = scheduler_compiled.diagnostics.as_array();
    ASSERT_TRUE(envelope_diagnostics.has_value());
    EXPECT_EQ(envelope_diagnostics->size(), 0u) << "信封 diagnostics 应为空";

    const auto compile_object = scheduler_compiled.compile.as_object();
    ASSERT_TRUE(compile_object.has_value());
    ASSERT_TRUE(compile_object->contains(std::string_view("diagnostics")));
    const auto payload_diagnostics =
        (*compile_object)[std::string_view("diagnostics")].as_array();
    ASSERT_TRUE(payload_diagnostics.has_value());
    EXPECT_EQ(payload_diagnostics->size(), 0u) << "编译载荷 diagnostics 应为空";

    const auto services = MakeParityCompileServices(properties, nullptr);
    const auto resolved = RepositoryInvoke::ResolveRepositoryInvokeSnapshot(
        services,
        MakeRepositoryRef(FIXTURE_ENTRY_REVISION));
    ASSERT_TRUE(resolved.ok);
    EXPECT_EQ(resolved.diagnostics.size(), 0u)
        << "graph 路径结果 diagnostics 应为空";
}
