#include <das/Core/TaskScheduler/RepositoryInvokeDtos.h>

#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <string_view>

namespace
{
    using namespace Das::Core::TaskScheduler::RepositoryInvoke::Dto;

    yyjson::value ParseJson(std::string_view json)
    {
        auto parsed = Das::Utils::ParseYyjsonFromString(json);
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : yyjson::value{};
    }
} // namespace

TEST(RepositoryInvokeDtoTest, RepositoryRefRoundTripUsesLowerCamelFields)
{
    RepositoryTaskRefDto ref;
    ref.entry_id = 42;
    ref.expected_revision = 7;
    ref.source_fingerprint = "source-hash";

    auto serialized = yyjson::object(ref);

    EXPECT_TRUE(serialized.contains(std::string_view("kind")));
    EXPECT_TRUE(serialized.contains(std::string_view("entryId")));
    EXPECT_TRUE(serialized.contains(std::string_view("expectedRevision")));
    EXPECT_TRUE(serialized.contains(std::string_view("sourceFingerprint")));
    EXPECT_FALSE(serialized.contains(std::string_view("entry_id")));
    EXPECT_FALSE(serialized.contains(std::string_view("expected_revision")));
    EXPECT_EQ(
        serialized[std::string_view("kind")].as_string().value(),
        std::string_view("taskRepositoryRef"));

    RepositoryTaskRefDto round_tripped;
    ASSERT_NO_THROW(
        round_tripped = yyjson::cast<RepositoryTaskRefDto>(serialized));
    EXPECT_EQ(round_tripped.entry_id, 42);
    EXPECT_EQ(round_tripped.expected_revision, 7);
    EXPECT_EQ(round_tripped.source_fingerprint, "source-hash");
}

TEST(RepositoryInvokeDtoTest, ChildSnapshotPreservesRawExecutionInput)
{
    ChildExecutionSnapshotDto snapshot;
    snapshot.source_entry_id = 42;
    snapshot.source_revision = 7;
    snapshot.source_fingerprint = "source-hash";
    snapshot.plugin_guid = "plugin-guid";
    snapshot.task_type_guid = "task-type-guid";
    snapshot.component_guid = "component-guid";
    snapshot.execution_input = ParseJson(R"json({
        "controllerName": "adb",
        "maapiPrivateField": {"keepExactKey": true}
    })json");

    auto serialized = yyjson::object(snapshot);

    EXPECT_TRUE(serialized.contains(std::string_view("sourceEntryId")));
    EXPECT_TRUE(serialized.contains(std::string_view("sourceRevision")));
    EXPECT_TRUE(serialized.contains(std::string_view("sourceFingerprint")));
    EXPECT_TRUE(serialized.contains(std::string_view("pluginGuid")));
    EXPECT_TRUE(serialized.contains(std::string_view("taskTypeGuid")));
    EXPECT_TRUE(serialized.contains(std::string_view("componentGuid")));
    EXPECT_TRUE(serialized.contains(std::string_view("executionInput")));
    EXPECT_FALSE(serialized.contains(std::string_view("source_entry_id")));

    auto execution_input =
        serialized[std::string_view("executionInput")].as_object();
    ASSERT_TRUE(execution_input.has_value());
    EXPECT_EQ(
        (*execution_input)[std::string_view("controllerName")]
            .as_string()
            .value(),
        std::string_view("adb"));
    auto private_payload =
        (*execution_input)[std::string_view("maapiPrivateField")].as_object();
    ASSERT_TRUE(private_payload.has_value());
    EXPECT_TRUE(
        (*private_payload)[std::string_view("keepExactKey")]
            .as_bool()
            .value());

    ChildExecutionSnapshotDto round_tripped;
    ASSERT_NO_THROW(
        round_tripped = yyjson::cast<ChildExecutionSnapshotDto>(serialized));
    EXPECT_EQ(round_tripped.source_entry_id, 42);
    EXPECT_EQ(round_tripped.source_revision, 7);
    EXPECT_EQ(round_tripped.source_fingerprint, "source-hash");
    EXPECT_EQ(round_tripped.plugin_guid, "plugin-guid");
    EXPECT_EQ(round_tripped.task_type_guid, "task-type-guid");
    EXPECT_EQ(round_tripped.component_guid, "component-guid");

    auto raw_payload = round_tripped.execution_input.as_object();
    ASSERT_TRUE(raw_payload.has_value());
    EXPECT_TRUE(
        (*(*raw_payload)[std::string_view("maapiPrivateField")].as_object())
            [std::string_view("keepExactKey")]
                .as_bool()
            .value());
}

TEST(RepositoryInvokeDtoTest, SettingsRoundTripContainsRepositoryRef)
{
    InvokeRepositoryTaskSettingsDto settings;
    settings.repository_ref.entry_id = 77;
    settings.repository_ref.expected_revision = 8;
    settings.fail_on_revision_mismatch = false;

    auto serialized = yyjson::object(settings);

    EXPECT_TRUE(serialized.contains(std::string_view("repositoryRef")));
    EXPECT_TRUE(
        serialized.contains(std::string_view("failOnRevisionMismatch")));
    EXPECT_FALSE(serialized.contains(std::string_view("repository_ref")));
    EXPECT_FALSE(
        serialized[std::string_view("failOnRevisionMismatch")]
            .as_bool()
            .value());

    auto ref = serialized[std::string_view("repositoryRef")].as_object();
    ASSERT_TRUE(ref.has_value());
    EXPECT_EQ(
        (*ref)[std::string_view("entryId")].as_int().value(),
        77);
    EXPECT_EQ(
        (*ref)[std::string_view("expectedRevision")].as_int().value(),
        8);

    InvokeRepositoryTaskSettingsDto round_tripped;
    ASSERT_NO_THROW(
        round_tripped =
            yyjson::cast<InvokeRepositoryTaskSettingsDto>(serialized));
    EXPECT_EQ(round_tripped.repository_ref.entry_id, 77);
    EXPECT_EQ(round_tripped.repository_ref.expected_revision, 8);
    EXPECT_FALSE(round_tripped.fail_on_revision_mismatch);
}

TEST(RepositoryInvokeDtoTest, InputRoundTripCarriesSnapshotAndRuntimeInputs)
{
    InvokeRepositoryTaskInputDto input;
    input.compiled_snapshot = ChildExecutionSnapshotDto{};
    input.compiled_snapshot->source_entry_id = 77;
    input.compiled_snapshot->source_revision = 8;
    input.compiled_snapshot->plugin_guid = "plugin-guid";
    input.compiled_snapshot->task_type_guid = "task-type-guid";
    input.compiled_snapshot->component_guid = "component-guid";
    input.compiled_snapshot->execution_input =
        ParseJson(R"json({"providerPrivate": {"exactKey": true}})json");
    input.runtime_inputs =
        ParseJson(R"json({"callerValue": 9, "callerPrivate": "kept"})json");

    auto serialized = yyjson::object(input);

    EXPECT_TRUE(serialized.contains(std::string_view("compiledSnapshot")));
    EXPECT_TRUE(serialized.contains(std::string_view("runtimeInputs")));
    EXPECT_FALSE(serialized.contains(std::string_view("compiled_snapshot")));

    auto snapshot =
        serialized[std::string_view("compiledSnapshot")].as_object();
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_EQ(
        (*snapshot)[std::string_view("componentGuid")]
            .as_string()
            .value(),
        std::string_view("component-guid"));

    auto runtime_inputs =
        serialized[std::string_view("runtimeInputs")].as_object();
    ASSERT_TRUE(runtime_inputs.has_value());
    EXPECT_EQ(
        (*runtime_inputs)[std::string_view("callerValue")].as_int().value(),
        9);
    EXPECT_EQ(
        (*runtime_inputs)[std::string_view("callerPrivate")]
            .as_string()
            .value(),
        std::string_view("kept"));

    InvokeRepositoryTaskInputDto round_tripped;
    ASSERT_NO_THROW(
        round_tripped = yyjson::cast<InvokeRepositoryTaskInputDto>(serialized));
    ASSERT_TRUE(round_tripped.compiled_snapshot.has_value());
    EXPECT_EQ(round_tripped.compiled_snapshot->source_entry_id, 77);
    EXPECT_EQ(round_tripped.compiled_snapshot->component_guid, "component-guid");
    auto round_tripped_runtime = round_tripped.runtime_inputs.as_object();
    ASSERT_TRUE(round_tripped_runtime.has_value());
    EXPECT_EQ(
        (*round_tripped_runtime)[std::string_view("callerPrivate")]
            .as_string()
            .value(),
        std::string_view("kept"));
}

TEST(RepositoryInvokeDtoTest, ResultEnvelopeRoundTripUsesTypedFixedFields)
{
    InvokeRepositoryTaskResultDto result;
    result.status = "failed";
    result.outputs.child_status = "childFailed";
    result.outputs.child_outputs =
        ParseJson(R"json({"childResult": {"rawOutputKey": 5}})json");
    result.diagnostics.push_back(InvokeRepositoryTaskDiagnosticDto{
        .severity = "error",
        .code = "revisionMismatch",
        .message = "Repository entry revision did not match",
        .path = "settings.repositoryRef"});
    result.signals.failed = true;

    auto serialized = yyjson::object(result);

    EXPECT_TRUE(serialized.contains(std::string_view("status")));
    EXPECT_TRUE(serialized.contains(std::string_view("outputs")));
    EXPECT_TRUE(serialized.contains(std::string_view("diagnostics")));
    EXPECT_TRUE(serialized.contains(std::string_view("signals")));

    auto outputs = serialized[std::string_view("outputs")].as_object();
    ASSERT_TRUE(outputs.has_value());
    EXPECT_EQ(
        (*outputs)[std::string_view("childStatus")].as_string().value(),
        std::string_view("childFailed"));
    auto child_outputs =
        (*outputs)[std::string_view("childOutputs")].as_object();
    ASSERT_TRUE(child_outputs.has_value());
    EXPECT_TRUE(child_outputs->contains(std::string_view("childResult")));

    auto diagnostics =
        serialized[std::string_view("diagnostics")].as_array();
    ASSERT_TRUE(diagnostics.has_value());
    ASSERT_EQ(diagnostics->size(), 1u);
    auto diagnostic = (*diagnostics)[0].as_object();
    ASSERT_TRUE(diagnostic.has_value());
    EXPECT_EQ(
        (*diagnostic)[std::string_view("severity")].as_string().value(),
        std::string_view("error"));
    EXPECT_EQ(
        (*diagnostic)[std::string_view("code")].as_string().value(),
        std::string_view("revisionMismatch"));
    EXPECT_EQ(
        (*diagnostic)[std::string_view("message")].as_string().value(),
        std::string_view("Repository entry revision did not match"));
    EXPECT_EQ(
        (*diagnostic)[std::string_view("path")].as_string().value(),
        std::string_view("settings.repositoryRef"));

    auto signals = serialized[std::string_view("signals")].as_object();
    ASSERT_TRUE(signals.has_value());
    EXPECT_TRUE((*signals)[std::string_view("failed")].as_bool().value());
    EXPECT_FALSE((*signals)[std::string_view("succeeded")].as_bool().value());

    InvokeRepositoryTaskResultDto round_tripped;
    ASSERT_NO_THROW(
        round_tripped =
            yyjson::cast<InvokeRepositoryTaskResultDto>(serialized));
    EXPECT_EQ(round_tripped.status, "failed");
    EXPECT_EQ(round_tripped.outputs.child_status, "childFailed");
    ASSERT_EQ(round_tripped.diagnostics.size(), 1u);
    EXPECT_EQ(round_tripped.diagnostics[0].code, "revisionMismatch");
    EXPECT_EQ(round_tripped.diagnostics[0].path, "settings.repositoryRef");
    EXPECT_TRUE(round_tripped.signals.failed);
    EXPECT_FALSE(round_tripped.signals.succeeded);
}
