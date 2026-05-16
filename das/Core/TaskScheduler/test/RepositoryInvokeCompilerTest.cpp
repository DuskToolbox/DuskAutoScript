#include <das/Core/TaskScheduler/RepositoryInvokeCompiler.h>

#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using Das::Core::TaskScheduler::Repository::Dto::RepositoryEntryDto;
    using namespace Das::Core::TaskScheduler::RepositoryInvoke;
    using namespace Das::Core::TaskScheduler::RepositoryInvoke::Dto;

    constexpr char kPluginGuid[] = "12345678-9ABC-4DEF-8123-456789ABCDEF";
    constexpr char kTaskGuid[] = "87654321-CBA9-4FED-9123-FEDCBA987654";
    constexpr char kComponentGuid[] =
        "68F10001-0000-4000-8000-000000000001";

    yyjson::value ParseJson(std::string_view json)
    {
        auto parsed = Das::Utils::ParseYyjsonFromString(json);
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : yyjson::value{};
    }

    RepositoryEntryDto MakeAvailableEntry(int64_t revision = 7)
    {
        RepositoryEntryDto entry;
        entry.entry_id = 42;
        entry.display_name = "Repository task";
        entry.plugin_guid = kPluginGuid;
        entry.task_type_guid = kTaskGuid;
        entry.authoring.revision = revision;
        entry.authoring.kind = "formSequence";
        entry.authoring.source_fingerprint = "source-a";
        entry.accepted_properties = ParseJson(R"json({"key1":"accepted"})json");
        entry.availability.state = "available";
        return entry;
    }

    struct FakeCompilerServices
    {
        std::optional<RepositoryEntryDto> entry;
        std::optional<std::string>        execution_component_guid =
            std::string(kComponentGuid);
        bool             provider_ok = true;
        std::string      compile_result_json =
            R"json({"ok":true,"executionInput":{"key1":"compiled"}})json";
        int                               load_count = 0;
        int                               capability_count = 0;
        int                               compile_count = 0;
        std::string                       last_compile_purpose;

        RepositoryInvokeCompileServices Bind()
        {
            RepositoryInvokeCompileServices services;
            services.load_entry =
                [this](int64_t entry_id) -> std::optional<RepositoryEntryDto>
            {
                ++load_count;
                if (!entry || entry->entry_id != entry_id)
                {
                    return std::nullopt;
                }
                return entry;
            };
            services.find_execution_component =
                [this](const RepositoryEntryDto&)
                    -> std::optional<std::string>
            {
                ++capability_count;
                return execution_component_guid;
            };
            services.compile_authoring =
                [this](
                    const RepositoryEntryDto&,
                    const yyjson::value& request)
                    -> RepositoryInvokeProviderCompileResult
            {
                ++compile_count;
                auto request_obj = request.as_object();
                if (request_obj
                    && request_obj->contains(std::string_view("purpose")))
                {
                    last_compile_purpose =
                        std::string(
                            (*request_obj)[std::string_view("purpose")]
                                .as_string()
                                .value_or(""));
                }

                RepositoryInvokeProviderCompileResult result;
                result.ok = provider_ok;
                result.compile_result = ParseJson(compile_result_json);
                return result;
            };
            return services;
        }
    };

    RepositoryTaskRefDto MakeRef(int64_t entry_id = 42)
    {
        RepositoryTaskRefDto ref;
        ref.entry_id = entry_id;
        return ref;
    }

    const RepositoryInvokeCompileDiagnostic& FirstDiagnostic(
        const RepositoryInvokeCompileResult& result)
    {
        EXPECT_FALSE(result.diagnostics.empty());
        return result.diagnostics.front();
    }

    std::vector<int64_t> CycleEntryIds(
        const std::vector<RepositoryCyclePathItem>& path)
    {
        std::vector<int64_t> ids;
        ids.reserve(path.size());
        for (const auto& item : path)
        {
            ids.push_back(item.entry_id);
        }
        return ids;
    }
} // namespace

TEST(RepositoryInvokeCompilerTest, NotFoundReturnsDiagnosticBeforeCompile)
{
    FakeCompilerServices fake;
    auto                 services = fake.Bind();

    auto result = ResolveRepositoryInvokeSnapshot(services, MakeRef());

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.snapshot.has_value());
    EXPECT_EQ(FirstDiagnostic(result).code, "repository-entry-not-found");
    EXPECT_EQ(fake.load_count, 1);
    EXPECT_EQ(fake.capability_count, 0);
    EXPECT_EQ(fake.compile_count, 0);
}

TEST(RepositoryInvokeCompilerTest, UnavailableReturnsDiagnosticBeforeCompile)
{
    FakeCompilerServices fake;
    fake.entry = MakeAvailableEntry();
    fake.entry->availability.state = "unavailable";
    fake.entry->availability.reason = "pluginUnavailable";
    fake.entry->availability.message = "Plugin package is not loaded";
    auto services = fake.Bind();

    auto result = ResolveRepositoryInvokeSnapshot(services, MakeRef());

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.snapshot.has_value());
    const auto& diagnostic = FirstDiagnostic(result);
    EXPECT_EQ(diagnostic.code, "repository-entry-unavailable");
    EXPECT_EQ(diagnostic.message, "Plugin package is not loaded");
    EXPECT_EQ(fake.compile_count, 0);
}

TEST(RepositoryInvokeCompilerTest, RevisionMismatchReturnsCurrentRevision)
{
    FakeCompilerServices fake;
    fake.entry = MakeAvailableEntry(9);
    auto ref = MakeRef();
    ref.expected_revision = 8;
    auto services = fake.Bind();

    auto result = ResolveRepositoryInvokeSnapshot(services, ref);

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.snapshot.has_value());
    const auto& diagnostic = FirstDiagnostic(result);
    EXPECT_EQ(diagnostic.code, "repository-revision-mismatch");
    EXPECT_EQ(diagnostic.current_revision, 9);
    EXPECT_EQ(diagnostic.expected_revision, 8);
    EXPECT_EQ(fake.compile_count, 0);
}

TEST(RepositoryInvokeCompilerTest, SourceFingerprintMismatchIsDeterministic)
{
    FakeCompilerServices fake;
    fake.entry = MakeAvailableEntry();
    auto ref = MakeRef();
    ref.source_fingerprint = "source-b";
    auto services = fake.Bind();

    auto result = ResolveRepositoryInvokeSnapshot(services, ref);

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.snapshot.has_value());
    const auto& diagnostic = FirstDiagnostic(result);
    EXPECT_EQ(diagnostic.code, "repository-source-fingerprint-mismatch");
    EXPECT_EQ(diagnostic.current_source_fingerprint, "source-a");
    EXPECT_EQ(diagnostic.expected_source_fingerprint, "source-b");
    EXPECT_EQ(fake.compile_count, 0);
}

TEST(RepositoryInvokeCompilerTest, ExecutionComponentMissingStopsBeforeCompile)
{
    FakeCompilerServices fake;
    fake.entry = MakeAvailableEntry();
    fake.execution_component_guid = std::nullopt;
    auto services = fake.Bind();

    auto result = ResolveRepositoryInvokeSnapshot(services, MakeRef());

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.snapshot.has_value());
    EXPECT_EQ(FirstDiagnostic(result).code, "execution-component-missing");
    EXPECT_EQ(fake.capability_count, 1);
    EXPECT_EQ(fake.compile_count, 0);
}

TEST(RepositoryInvokeCompilerTest, ExecutionCompileBuildsChildSnapshot)
{
    FakeCompilerServices fake;
    fake.entry = MakeAvailableEntry();
    fake.compile_result_json = R"json({
        "ok": true,
        "executionInput": {
            "key1": "compiled",
            "providerPrivate": {"keepExactKey": true}
        }
    })json";
    auto ref = MakeRef();
    ref.expected_revision = 7;
    ref.source_fingerprint = "source-a";
    auto services = fake.Bind();

    auto result = ResolveRepositoryInvokeSnapshot(services, ref);

    ASSERT_TRUE(result.ok);
    ASSERT_TRUE(result.snapshot.has_value());
    EXPECT_TRUE(result.diagnostics.empty());
    EXPECT_EQ(fake.compile_count, 1);
    EXPECT_EQ(fake.last_compile_purpose, "execution");

    const auto& snapshot = *result.snapshot;
    EXPECT_EQ(snapshot.source_entry_id, 42);
    EXPECT_EQ(snapshot.source_revision, 7);
    EXPECT_EQ(snapshot.source_fingerprint, "source-a");
    EXPECT_EQ(snapshot.plugin_guid, kPluginGuid);
    EXPECT_EQ(snapshot.task_type_guid, kTaskGuid);
    EXPECT_EQ(snapshot.component_guid, kComponentGuid);

    auto execution_input = snapshot.execution_input.as_object();
    ASSERT_TRUE(execution_input.has_value());
    EXPECT_EQ(
        (*execution_input)[std::string_view("key1")].as_string().value_or(""),
        std::string_view("compiled"));
    auto private_payload =
        (*execution_input)[std::string_view("providerPrivate")].as_object();
    ASSERT_TRUE(private_payload.has_value());
    EXPECT_TRUE(
        (*private_payload)[std::string_view("keepExactKey")]
            .as_bool()
            .value_or(false));
}

TEST(RepositoryInvokeCompilerTest, ProviderFailedReturnsDiagnostic)
{
    FakeCompilerServices fake;
    fake.entry = MakeAvailableEntry();
    fake.provider_ok = false;
    auto services = fake.Bind();

    auto result = ResolveRepositoryInvokeSnapshot(services, MakeRef());

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.snapshot.has_value());
    EXPECT_EQ(FirstDiagnostic(result).code, "provider-compile-failed");
    EXPECT_EQ(fake.compile_count, 1);
}

TEST(RepositoryInvokeCompilerTest, ProviderFailedWhenCompileReturnsOkFalse)
{
    FakeCompilerServices fake;
    fake.entry = MakeAvailableEntry();
    fake.compile_result_json = R"json({"ok":false})json";
    auto services = fake.Bind();

    auto result = ResolveRepositoryInvokeSnapshot(services, MakeRef());

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.snapshot.has_value());
    EXPECT_EQ(FirstDiagnostic(result).code, "provider-compile-failed");
    EXPECT_EQ(fake.compile_count, 1);
}

TEST(RepositoryInvokeCompilerTest, MissingExecutionInputReturnsDiagnostic)
{
    FakeCompilerServices fake;
    fake.entry = MakeAvailableEntry();
    fake.compile_result_json = R"json({"ok":true,"summary":{}})json";
    auto services = fake.Bind();

    auto result = ResolveRepositoryInvokeSnapshot(services, MakeRef());

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.snapshot.has_value());
    EXPECT_EQ(FirstDiagnostic(result).code, "execution-input-missing");
    EXPECT_EQ(fake.compile_count, 1);
}

TEST(RepositoryInvokeCompilerTest, ExtractsRepositoryRefEdgesFromSourceNodes)
{
    auto graph = ParseJson(R"json({
        "nodes": [
            {
                "id": "node-a",
                "label": "Invoke child",
                "taskRepositoryRef": {
                    "kind": "taskRepositoryRef",
                    "entryId": 20
                }
            }
        ]
    })json");

    auto edges = ExtractRepositoryInvokeDependencyEdges(10, graph);

    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0].source_entry_id, 10);
    EXPECT_EQ(edges[0].target_entry_id, 20);
    EXPECT_EQ(edges[0].source_node_id, "node-a");
    EXPECT_EQ(edges[0].source_node_label, "Invoke child");
}

TEST(RepositoryInvokeCompilerTest, DirectCycleReturnsDiagnosticBeforeSnapshot)
{
    FakeCompilerServices fake;
    fake.entry = MakeAvailableEntry();
    RepositoryInvokeSourceContext context;
    context.source_entry_id = 42;
    context.source_graph = ParseJson(R"json({
        "nodes": [
            {
                "id": "node-self",
                "label": "Invoke self",
                "taskRepositoryRef": {
                    "kind": "taskRepositoryRef",
                    "entryId": 42
                }
            }
        ]
    })json");
    auto services = fake.Bind();

    auto result = ResolveRepositoryInvokeSnapshot(services, MakeRef(42), context);

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.snapshot.has_value());
    EXPECT_EQ(FirstDiagnostic(result).code, "repository-invoke-cycle");
    EXPECT_EQ(CycleEntryIds(result.cycle_path), (std::vector<int64_t>{42, 42}));
    ASSERT_FALSE(result.cycle_path.empty());
    EXPECT_EQ(result.cycle_path.front().source_node_label, "Invoke self");
    EXPECT_EQ(fake.load_count, 0);
    EXPECT_EQ(fake.compile_count, 0);
}

TEST(RepositoryInvokeCompilerTest, IndirectCycleReturnsFullCyclePath)
{
    FakeCompilerServices fake;
    fake.entry = MakeAvailableEntry();
    RepositoryInvokeSourceContext context;
    context.source_entry_id = 1;
    context.source_graph = ParseJson(R"json({
        "nodes": [
            {
                "sourceEntryId": 1,
                "id": "node-1",
                "label": "Invoke two",
                "taskRepositoryRef": {
                    "kind": "taskRepositoryRef",
                    "entryId": 2
                }
            },
            {
                "sourceEntryId": 2,
                "id": "node-2",
                "label": "Invoke one",
                "settings": {
                    "repositoryRef": {
                        "kind": "taskRepositoryRef",
                        "entryId": 1
                    }
                }
            }
        ]
    })json");
    auto services = fake.Bind();

    auto result = ResolveRepositoryInvokeSnapshot(services, MakeRef(2), context);

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.snapshot.has_value());
    EXPECT_EQ(FirstDiagnostic(result).code, "repository-invoke-cycle");
    EXPECT_EQ(
        CycleEntryIds(result.cycle_path),
        (std::vector<int64_t>{1, 2, 1}));
    ASSERT_GE(result.cycle_path.size(), 3u);
    EXPECT_EQ(result.cycle_path[0].source_node_label, "Invoke two");
    EXPECT_EQ(result.cycle_path[1].source_node_label, "Invoke one");
    EXPECT_EQ(fake.load_count, 0);
    EXPECT_EQ(fake.compile_count, 0);
}
