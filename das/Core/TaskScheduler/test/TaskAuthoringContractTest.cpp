#include <das/Core/TaskScheduler/TaskAuthoringContract.h>

#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <string_view>

namespace
{
    yyjson::value ParseJson(std::string_view json)
    {
        auto parsed = Das::Utils::ParseYyjsonFromString(json);
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : yyjson::value{};
    }
} // namespace

TEST(TaskAuthoringContractTest, ValidFormSequenceDocumentPasses)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "kind": "formSequence",
        "revision": 7,
        "formSequence": {"fields": [], "sequence": []}
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.error_kind.empty());
}

TEST(TaskAuthoringContractTest, ValidGraphDocumentPasses)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "kind": "graph",
        "revision": 2,
        "graph": {"nodes": [], "connections": []}
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_TRUE(result.valid);
}

TEST(TaskAuthoringContractTest, MissingContractVersionFails)
{
    auto document = ParseJson(R"json({
        "kind": "formSequence",
        "revision": 1
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "missingField");
}

TEST(TaskAuthoringContractTest, MissingRevisionFails)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "kind": "formSequence"
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "missingField");
}

TEST(TaskAuthoringContractTest, CustomWebDocumentFails)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "kind": "customWeb",
        "revision": 1
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "unsupportedKind");
}

TEST(TaskAuthoringContractTest, LegacyFieldSetFails)
{
    auto document = ParseJson(R"json({
        "version": 1,
        "kind": "formSequence",
        "revision": 1,
        "view": {},
        "schema": {},
        "catalog": {},
        "diagnostics": [],
        "migration": {}
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "missingField");
}

TEST(TaskAuthoringContractTest, ChangeRequiresBaseRevision)
{
    auto change = ParseJson(R"json({
        "kind": "setValue",
        "payload": {"path": "projectFolder", "value": "C:/maa"}
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringChange(change);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "missingField");
}

TEST(TaskAuthoringContractTest, SupportedChangeKindsPass)
{
    constexpr std::string_view kKinds[] = {
        "setValue",
        "applyPreset",
        "addSequenceItem",
        "moveSequenceItem",
        "addNode",
        "connectPorts",
        "updateNodeConfig",
        "removeNode",
        "disconnectPorts",
        "removeSequenceItem"};

    for (auto kind : kKinds)
    {
        auto change = ParseJson(
            std::string{"{\"baseRevision\":1,\"kind\":\""} + std::string{kind}
            + "\",\"payload\":{}}");

        auto result = Das::Core::TaskScheduler::ValidateAuthoringChange(change);

        EXPECT_TRUE(result.valid) << kind;
    }
}

TEST(TaskAuthoringContractTest, AuthoringErrorUsesLowerCamelCase)
{
    auto error = Das::Core::TaskScheduler::MakeAuthoringError(
        "invalidRevision",
        "stale");
    auto obj = error.as_object();
    ASSERT_TRUE(obj.has_value());

    EXPECT_TRUE(obj->contains(std::string_view("errorKind")));
    EXPECT_EQ(
        (*obj)[std::string_view("errorKind")].as_string().value(),
        "invalidRevision");
    EXPECT_EQ((*obj)[std::string_view("message")].as_string().value(), "stale");
}
