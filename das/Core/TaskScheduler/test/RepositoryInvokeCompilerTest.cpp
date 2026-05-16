#include <das/Core/TaskScheduler/RepositoryInvokeCompiler.h>

#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <optional>
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
        int                               load_count = 0;
        int                               capability_count = 0;
        int                               compile_count = 0;

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
                    const yyjson::value&) -> RepositoryInvokeProviderCompileResult
            {
                ++compile_count;
                return RepositoryInvokeProviderCompileResult{};
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
