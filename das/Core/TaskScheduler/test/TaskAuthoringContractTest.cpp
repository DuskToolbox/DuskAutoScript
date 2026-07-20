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

// =============================================================================
// Tracer 1: Provider Get/Apply/Compile result envelopes
// (D-84-08/D-84-10/D-84-11)
// =============================================================================

TEST(TaskAuthoringContractTest, GetResultWithDocumentAndDiagnosticsPasses)
{
    auto result = ParseJson(R"json({
        "document": {
            "contractVersion": 1,
            "viewKind": "formSequence",
            "revision": 4
        },
        "diagnostics": []
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringGetResult(result);
    EXPECT_TRUE(r.valid);
    EXPECT_TRUE(r.error_kind.empty()) << r.message;
}

TEST(TaskAuthoringContractTest, GetResultDiagnosticsOnlyPasses)
{
    auto result = ParseJson(R"json({
        "diagnostics": [
            {"severity": "error", "message": "provider broken"}
        ]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringGetResult(result);
    EXPECT_TRUE(r.valid);
    EXPECT_TRUE(r.error_kind.empty()) << r.message;
}

TEST(TaskAuthoringContractTest, GetResultRejectsMissingDiagnostics)
{
    auto result = ParseJson(R"json({
        "document": {
            "contractVersion": 1,
            "viewKind": "formSequence",
            "revision": 0
        }
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringGetResult(result);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "missingField");
}

TEST(TaskAuthoringContractTest, GetResultRejectsUnknownTopLevelField)
{
    auto result = ParseJson(R"json({
        "diagnostics": [],
        "migration": {}
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringGetResult(result);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "unknownField");
}

TEST(TaskAuthoringContractTest, GetResultRejectsDiagnosticWithSourceField)
{
    auto result = ParseJson(R"json({
        "diagnostics": [
            {"severity": "error", "message": "x", "source": "compiler"}
        ]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringGetResult(result);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "unknownDiagnosticField");
}

TEST(TaskAuthoringContractTest, GetResultRejectsStringDiagnosticCode)
{
    auto result = ParseJson(R"json({
        "diagnostics": [
            {"severity": "error", "message": "x", "code": "E1"}
        ]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringGetResult(result);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "invalidDiagnosticCode");
}

TEST(TaskAuthoringContractTest, GetResultRejectsInvalidSeverity)
{
    auto result = ParseJson(R"json({
        "diagnostics": [
            {"severity": "critical", "message": "x"}
        ]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringGetResult(result);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "invalidDiagnosticSeverity");
}

TEST(TaskAuthoringContractTest, GetResultRejectsEmptyDiagnosticMessage)
{
    auto result = ParseJson(R"json({
        "diagnostics": [
            {"severity": "info", "message": ""}
        ]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringGetResult(result);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "invalidDiagnosticMessage");
}

TEST(
    TaskAuthoringContractTest,
    ApplyResultPassesWhenDocumentRevisionIsBasePlusOne)
{
    auto result = ParseJson(R"json({
        "acceptedProperties": {},
        "document": {
            "contractVersion": 1,
            "viewKind": "formSequence",
            "revision": 5
        },
        "diagnostics": []
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(result, 4);
    EXPECT_TRUE(r.valid);
    EXPECT_TRUE(r.error_kind.empty()) << r.message;
}

TEST(TaskAuthoringContractTest, ApplyResultFailsWhenRevisionIsNotBasePlusOne)
{
    auto result = ParseJson(R"json({
        "acceptedProperties": {},
        "document": {
            "contractVersion": 1,
            "viewKind": "formSequence",
            "revision": 4
        },
        "diagnostics": []
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(result, 4);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "revisionMismatch");
}

TEST(TaskAuthoringContractTest, ApplyResultRequiresAcceptedProperties)
{
    auto result = ParseJson(R"json({
        "document": {
            "contractVersion": 1,
            "viewKind": "formSequence",
            "revision": 5
        },
        "diagnostics": []
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(result, 4);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "missingField");
}

TEST(TaskAuthoringContractTest, CompileResultRequiresCompileAndDiagnostics)
{
    auto ok = ParseJson(R"json({
        "compile": {},
        "diagnostics": []
    })json");
    auto r_ok = Das::Core::TaskScheduler::ValidateAuthoringCompileResult(ok);
    EXPECT_TRUE(r_ok.valid);
    EXPECT_TRUE(r_ok.error_kind.empty()) << r_ok.message;

    auto no_compile = ParseJson(R"json({"diagnostics": []})json");
    auto r_no_compile =
        Das::Core::TaskScheduler::ValidateAuthoringCompileResult(no_compile);
    EXPECT_FALSE(r_no_compile.valid);
    EXPECT_EQ(r_no_compile.error_kind, "missingField");

    auto no_diag = ParseJson(R"json({"compile": {}})json");
    auto r_no_diag =
        Das::Core::TaskScheduler::ValidateAuthoringCompileResult(no_diag);
    EXPECT_FALSE(r_no_diag.valid);
    EXPECT_EQ(r_no_diag.error_kind, "missingField");
}
