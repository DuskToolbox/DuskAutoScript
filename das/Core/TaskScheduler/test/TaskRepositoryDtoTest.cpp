#include <das/Core/TaskScheduler/TaskRepositoryDtos.h>

#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <string_view>

namespace
{
    using namespace Das::Core::TaskScheduler::Repository::Dto;

    yyjson::value MakePayload()
    {
        auto payload = Das::Utils::MakeYyjsonObject();
        auto obj = *payload.as_object();
        obj[std::string_view("providerFlag")] = true;
        obj[std::string_view("providerText")] = "kept-raw";
        return payload;
    }
} // namespace

TEST(
    TaskRepositoryDtoTest,
    RepositoryDtoRoundTripUsesLowerCamelCaseAndRawProviderPayloads)
{
    RepositoryEntryDto entry;
    entry.entry_id = 42;
    entry.display_name = "MAA daily";
    entry.plugin_guid = "plugin-guid";
    entry.task_type_guid = "task-type-guid";
    entry.authoring.revision = 7;
    entry.authoring.kind = "formSequence";
    entry.authoring.source_fingerprint = "source-hash";
    entry.accepted_properties = MakePayload();
    entry.availability.state = "available";

    auto serialized = yyjson::object(entry);
    auto obj = serialized.as_object();
    ASSERT_TRUE(obj.has_value());

    EXPECT_TRUE(obj->contains(std::string_view("entryId")));
    EXPECT_TRUE(obj->contains(std::string_view("displayName")));
    EXPECT_TRUE(obj->contains(std::string_view("pluginGuid")));
    EXPECT_TRUE(obj->contains(std::string_view("taskTypeGuid")));
    EXPECT_TRUE(obj->contains(std::string_view("acceptedProperties")));
    EXPECT_TRUE(obj->contains(std::string_view("sourceFingerprint")));
    EXPECT_FALSE(obj->contains(std::string_view("entry_id")));
    EXPECT_FALSE(obj->contains(std::string_view("display_name")));

    auto accepted =
        (*obj)[std::string_view("acceptedProperties")].as_object();
    ASSERT_TRUE(accepted.has_value());
    EXPECT_TRUE(
        (*accepted)[std::string_view("providerFlag")].as_bool().value());
    EXPECT_EQ(
        (*accepted)[std::string_view("providerText")]
            .as_string()
            .value(),
        std::string_view("kept-raw"));

    auto round_tripped = yyjson::cast<RepositoryEntryDto>(serialized);
    EXPECT_EQ(round_tripped.entry_id, 42);
    EXPECT_EQ(round_tripped.display_name, "MAA daily");
    EXPECT_EQ(round_tripped.plugin_guid, "plugin-guid");
    EXPECT_EQ(round_tripped.task_type_guid, "task-type-guid");
    EXPECT_EQ(round_tripped.authoring.revision, 7);
    EXPECT_EQ(round_tripped.authoring.source_fingerprint, "source-hash");

    auto raw_payload = round_tripped.accepted_properties.as_object();
    ASSERT_TRUE(raw_payload.has_value());
    EXPECT_TRUE(
        (*raw_payload)[std::string_view("providerFlag")].as_bool().value());

    RepositoryAuthoringResultDto authoring;
    authoring.entry_id = entry.entry_id;
    authoring.authoring = entry.authoring;
    authoring.accepted_properties = MakePayload();
    authoring.document = MakePayload();
    authoring.diagnostics.push_back(MakePayload());

    auto authoring_json = yyjson::object(authoring);
    auto authoring_obj = authoring_json.as_object();
    ASSERT_TRUE(authoring_obj.has_value());
    EXPECT_TRUE(authoring_obj->contains(std::string_view("document")));
    EXPECT_TRUE(authoring_obj->contains(std::string_view("diagnostics")));

    RepositoryCompileResultDto compile;
    compile.can_execute = true;
    compile.summary.can_execute = true;
    compile.summary.task_names.push_back("Start");
    compile.summary.requires_agent_runtime = true;
    compile.diagnostics.push_back(MakePayload());
    compile.debug_compile = MakePayload();

    auto compile_json = yyjson::object(compile);
    auto compile_obj = compile_json.as_object();
    ASSERT_TRUE(compile_obj.has_value());
    EXPECT_TRUE(compile_obj->contains(std::string_view("canExecute")));
    EXPECT_TRUE(compile_obj->contains(std::string_view("debugCompile")));
    EXPECT_TRUE(compile_obj->contains(std::string_view("diagnostics")));
}
