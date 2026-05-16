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
