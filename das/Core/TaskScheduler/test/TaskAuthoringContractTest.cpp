#include <das/Core/TaskScheduler/TaskAuthoringContract.h>

#include <das/Utils/DasJsonCore.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
    yyjson::value ParseJson(std::string_view json)
    {
        auto parsed = Das::Utils::ParseYyjsonFromString(json);
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : yyjson::value{};
    }

    // 与 ParseJson 相同，但放行 NaN/Infinity 字面量——构造非有限数值
    // 输入，供有限性校验用例使用（默认 flag 下 yyjson 直接拒绝该文档，
    // 非有限值只能经 AllowInfAndNan 语法进入）。
    yyjson::value ParseJsonAllowingNonFinite(std::string_view json)
    {
        auto parsed = Das::Utils::ParseYyjsonFromString(
            json,
            yyjson::ReadFlag::AllowInfAndNan);
        EXPECT_TRUE(parsed.has_value());
        return parsed ? std::move(*parsed) : yyjson::value{};
    }

    // 仓库根 schema 契约文件定位：ctest WORKING_DIRECTORY 位于构建树内，
    // 逐级向上直到命中仓库根的 TaskAuthoringHttpContract.schema.json。
    std::optional<std::filesystem::path> ResolveSchemaJsonPath()
    {
        namespace fs = std::filesystem;
        auto directory = fs::current_path();
        for (int depth = 0; depth < 8; ++depth)
        {
            const auto candidate =
                directory / "TaskAuthoringHttpContract.schema.json";
            std::error_code error_code;
            if (fs::exists(candidate, error_code))
            {
                return candidate;
            }
            const auto parent = directory.parent_path();
            if (parent == directory)
            {
                break;
            }
            directory = parent;
        }
        return std::nullopt;
    }

    std::optional<std::string> ReadFileText(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            return std::nullopt;
        }
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        return buffer.str();
    }
} // namespace

TEST(TaskAuthoringContractTest, ValidFormSequenceDocumentPasses)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 7,
        "formSequence": {"items": []}
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.error_kind.empty()) << result.message;
}

TEST(TaskAuthoringContractTest, ValidGraphDocumentPasses)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 2,
        "graph": {"nodes": []}
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.error_kind.empty()) << result.message;
}

TEST(TaskAuthoringContractTest, ValidGraphDocumentWithConnectionsPasses)
{
    // D-84-37: graph.connections is part of the strict wire — without it,
    // connectGraphPorts/disconnectGraphPorts change kinds could not round-trip
    // real graph topology.
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 2,
        "graph": {
            "nodes": [],
            "connections": [
                {"fromNodeId":"a","fromPortId":"o","toNodeId":"b","toPortId":"i"}
            ]
        }
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.error_kind.empty()) << result.message;
}

TEST(TaskAuthoringContractTest, ValidGraphNodePortsPass)
{
    // Node port projection: each element is exactly the five locked keys
    // {portId, displayLabel, portType, direction, category} with direction ∈
    // {input, output} and category ∈ {execution, data} (drag-link precheck
    // data source; the category carries the execution/data split).
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 2,
        "graph": {
            "nodes": [
                {
                    "id": "n1",
                    "ports": [
                        {"portId":"condition","displayLabel":"","portType":"bool","direction":"input","category":"data"},
                        {"portId":"true","displayLabel":"True","portType":"signal","direction":"output","category":"execution"}
                    ]
                },
                {"id": "n2", "ports": []}
            ]
        }
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.error_kind.empty()) << result.message;
}

TEST(TaskAuthoringContractTest, GraphNodePortShapeViolationsFail)
{
    const std::vector<std::string> invalid_ports = {
        // direction outside the enum
        R"([{"portId":"x","displayLabel":"","portType":"bool","direction":"sideways","category":"data"}])",
        // category outside the enum
        R"([{"portId":"x","displayLabel":"","portType":"bool","direction":"input","category":"foo"}])",
        // missing required key (category)
        R"([{"portId":"x","displayLabel":"","portType":"bool","direction":"input"}])",
        // missing required key (direction)
        R"([{"portId":"x","displayLabel":"","portType":"bool","category":"data"}])",
        // unknown extra key on the element
        R"([{"portId":"x","displayLabel":"","portType":"bool","direction":"input","category":"data","extra":1}])",
        // empty portId
        R"([{"portId":"","displayLabel":"","portType":"bool","direction":"input","category":"data"}])",
        // non-string portType
        R"([{"portId":"x","displayLabel":"","portType":5,"direction":"input","category":"data"}])",
        // element is not an object
        R"(["x"])",
    };
    for (const auto& ports_json : invalid_ports)
    {
        auto document = ParseJson(
            std::string(R"json({
            "contractVersion": 1,
            "viewKind": "graph",
            "revision": 2,
            "graph": {"nodes": [{"id": "n1", "ports": )json")
            + ports_json + "}]} }");
        auto result =
            Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
        EXPECT_FALSE(result.valid)
            << "ports json: " << ports_json << " error: " << result.message;
    }
}

TEST(TaskAuthoringContractTest, DocumentViewDataPartitionPasses)
{
    // Document-level viewData partition (UI-owned canvas coordinates): the
    // strict contract must whitelist the root key so moveGraphNode results
    // survive the provider boundary. Content stays the UI's free partition —
    // the contract only pins the container type.
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 2,
        "graph": {"nodes": []},
        "viewData": {
            "positions": {"n1": {"x": 30, "y": -4.5}}
        }
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.error_kind.empty()) << result.message;
}

TEST(TaskAuthoringContractTest, DocumentViewDataNonObjectFails)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 2,
        "graph": {"nodes": []},
        "viewData": 7
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "invalidViewData");
}

TEST(TaskAuthoringContractTest, MissingContractVersionFails)
{
    auto document = ParseJson(R"json({
        "viewKind": "formSequence",
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
        "viewKind": "formSequence"
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "missingField");
}

TEST(TaskAuthoringContractTest, CustomWebDocumentFails)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "customWeb",
        "revision": 1
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "unsupportedViewKind");
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

TEST(TaskAuthoringContractTest, DocumentRejectsLegacyKindAlias)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "kind": "formSequence",
        "revision": 1
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "missingField");
}

TEST(TaskAuthoringContractTest, DocumentRejectsUnknownTopLevelField)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 1,
        "properties": {}
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "unknownField");
}

TEST(TaskAuthoringContractTest, DocumentRejectsLegacyMigrationField)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 1,
        "migration": {}
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "unknownField");
}

TEST(TaskAuthoringContractTest, DocumentWithRootFieldsPasses)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [
            {
                "name": "interface",
                "label": "接口路径",
                "valueType": 4,
                "inputType": 5,
                "value": "C:/interface.json"
            }
        ]
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.error_kind.empty()) << result.message;
}

TEST(TaskAuthoringContractTest, FormSequenceUsesItemsNotSequence)
{
    auto legacy = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "formSequence": {"sequence": []}
    })json");
    auto legacy_r = Das::Core::TaskScheduler::ValidateAuthoringDocument(legacy);
    EXPECT_FALSE(legacy_r.valid);
    EXPECT_EQ(legacy_r.error_kind, "unknownField");

    auto modern = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "formSequence": {"items": []}
    })json");
    auto modern_r = Das::Core::TaskScheduler::ValidateAuthoringDocument(modern);
    EXPECT_TRUE(modern_r.valid);
    EXPECT_TRUE(modern_r.error_kind.empty()) << modern_r.message;
}

TEST(TaskAuthoringContractTest, FieldDescriptorRejectsLegacyExtensions)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [
            {
                "name": "x",
                "label": "X",
                "valueType": 4,
                "inputType": 0,
                "value": "",
                "group": "main",
                "dependency": {},
                "rules": {},
                "unit": "ms",
                "placeholder": "..."
            }
        ]
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "unknownField");
}

TEST(TaskAuthoringContractTest, CandidatesAllowedOnlyForSelectInputTypes)
{
    auto on_select = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [{
            "name": "lang", "label": "L", "valueType": 4, "inputType": 1,
            "value": "zh",
            "candidates": [{"value": "zh", "label": "中文"}]
        }]
    })json");
    auto r_ok = Das::Core::TaskScheduler::ValidateAuthoringDocument(on_select);
    EXPECT_TRUE(r_ok.valid) << r_ok.message;

    auto on_plain = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [{
            "name": "path", "label": "P", "valueType": 4, "inputType": 0,
            "value": "",
            "candidates": [{"value": "x", "label": "X"}]
        }]
    })json");
    auto r_bad = Das::Core::TaskScheduler::ValidateAuthoringDocument(on_plain);
    EXPECT_FALSE(r_bad.valid);
    EXPECT_EQ(r_bad.error_kind, "candidatesRequireSelect");
}

TEST(TaskAuthoringContractTest, CandidateDescriptorStrictShape)
{
    auto legacy = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [{
            "name": "lang", "label": "L", "valueType": 4, "inputType": 1,
            "value": "zh",
            "candidates": [{"name": "zh", "label": "中文"}]
        }]
    })json");
    auto legacy_r = Das::Core::TaskScheduler::ValidateAuthoringDocument(legacy);
    EXPECT_FALSE(legacy_r.valid);
    EXPECT_EQ(legacy_r.error_kind, "missingField");

    auto null_value = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [{
            "name": "lang", "label": "L", "valueType": 4, "inputType": 1,
            "value": "zh",
            "candidates": [{"value": null, "label": "空"}]
        }]
    })json");
    auto null_r =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(null_value);
    EXPECT_FALSE(null_r.valid);
    EXPECT_EQ(null_r.error_kind, "invalidCandidateValue");
}

TEST(TaskAuthoringContractTest, FieldValidationShapeIsStrict)
{
    auto with_extra = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [{
            "name": "x", "label": "X", "valueType": 4, "inputType": 0, "value": "",
            "validation": {"pattern": "[0-9]+", "message": "digits", "extra": true}
        }]
    })json");
    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(with_extra);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "unknownField");
}

TEST(TaskAuthoringContractTest, IconMustBeAbsolutePathOrHttpUrl)
{
    auto accept_absolute = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [{
            "name": "x", "label": "X", "valueType": 4, "inputType": 0, "value": "",
            "icon": "E:/icons/x.png"
        }]
    })json");
    auto r1 =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(accept_absolute);
    EXPECT_TRUE(r1.valid) << r1.message;

    auto accept_http = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [{
            "name": "x", "label": "X", "valueType": 4, "inputType": 0, "value": "",
            "icon": "https://example.com/icon.png"
        }]
    })json");
    auto r2 = Das::Core::TaskScheduler::ValidateAuthoringDocument(accept_http);
    EXPECT_TRUE(r2.valid) << r2.message;

    auto reject_relative = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [{
            "name": "x", "label": "X", "valueType": 4, "inputType": 0, "value": "",
            "icon": "icons/x.png"
        }]
    })json");
    auto r3 =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(reject_relative);
    EXPECT_FALSE(r3.valid);
    EXPECT_EQ(r3.error_kind, "invalidIcon");

    auto reject_data = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [{
            "name": "x", "label": "X", "valueType": 4, "inputType": 0, "value": "",
            "icon": "data:image/png;base64,AAAA"
        }]
    })json");
    auto r4 = Das::Core::TaskScheduler::ValidateAuthoringDocument(reject_data);
    EXPECT_FALSE(r4.valid);
    EXPECT_EQ(r4.error_kind, "invalidIcon");
}

TEST(TaskAuthoringContractTest, FieldLayoutLeafMustReferenceExistingField)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [
            {"name": "a", "label": "A", "valueType": 4, "inputType": 0, "value": ""}
        ],
        "fieldLayout": [
            {"fieldName": "missing"}
        ]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "layoutFieldNotInContainer");
}

TEST(TaskAuthoringContractTest, FieldLayoutExactCoverageRequired)
{
    auto duplicate = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [
            {"name": "a", "label": "A", "valueType": 4, "inputType": 0, "value": ""}
        ],
        "fieldLayout": [
            {"fieldName": "a"},
            {"fieldName": "a"}
        ]
    })json");
    auto r1 = Das::Core::TaskScheduler::ValidateAuthoringDocument(duplicate);
    EXPECT_FALSE(r1.valid);
    EXPECT_EQ(r1.error_kind, "layoutDuplicateFieldRef");

    auto missing = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [
            {"name": "a", "label": "A", "valueType": 4, "inputType": 0, "value": ""},
            {"name": "b", "label": "B", "valueType": 4, "inputType": 0, "value": ""}
        ],
        "fieldLayout": [
            {"fieldName": "a"}
        ]
    })json");
    auto r2 = Das::Core::TaskScheduler::ValidateAuthoringDocument(missing);
    EXPECT_FALSE(r2.valid);
    EXPECT_EQ(r2.error_kind, "layoutMissingField");
}

TEST(TaskAuthoringContractTest, FieldLayoutSectionRecursiveCoverage)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [
            {"name": "a", "label": "A", "valueType": 4, "inputType": 0, "value": ""},
            {"name": "b", "label": "B", "valueType": 4, "inputType": 0, "value": ""}
        ],
        "fieldLayout": [
            {
                "id": "section-1",
                "label": "Section",
                "entries": [
                    {"fieldName": "a"},
                    {"fieldName": "b"}
                ]
            }
        ]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(r.valid) << r.message;
}

// {commandId} 叶子在 section.entries 内引用 commands[] 已声明的 id
// → PASS。commands 必须含 apply.kind:"invokeCommand" + payload.commandId。
TEST(TaskAuthoringContractTest, FieldLayoutCommandLeafValid)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [
            {"name": "a", "label": "A", "valueType": 4, "inputType": 0, "value": ""}
        ],
        "fieldLayout": [
            {
                "id": "section-1",
                "label": "Section",
                "entries": [
                    {"fieldName": "a"},
                    {"commandId": "checkUpdate"}
                ]
            }
        ],
        "commands": [
            {
                "id": "checkUpdate",
                "label": "Check Update",
                "apply": {"kind": "invokeCommand", "payload": {"commandId": "checkUpdate"}}
            }
        ]
    })json");
    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(r.valid) << r.message;
}

// commandId 引用不在 commands[] 的 id → FAIL
// layoutCommandNotInContainer。
TEST(TaskAuthoringContractTest, FieldLayoutCommandLeafNotInCommands)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [
            {"name": "a", "label": "A", "valueType": 4, "inputType": 0, "value": ""}
        ],
        "fieldLayout": [
            {
                "id": "section-1",
                "label": "Section",
                "entries": [
                    {"fieldName": "a"},
                    {"commandId": "ghost"}
                ]
            }
        ],
        "commands": [
            {
                "id": "checkUpdate",
                "label": "Check Update",
                "apply": {"kind": "invokeCommand", "payload": {"commandId": "checkUpdate"}}
            }
        ]
    })json");
    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "layoutCommandNotInContainer");
}

// row 横向容器在 section.entries 内合法 → PASS。横排两个 fieldName
// 叶子（fieldName 全覆盖不变量保持）。
TEST(TaskAuthoringContractTest, FieldLayoutRowInsideSection)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [
            {"name": "a", "label": "A", "valueType": 4, "inputType": 0, "value": ""},
            {"name": "b", "label": "B", "valueType": 4, "inputType": 0, "value": ""}
        ],
        "fieldLayout": [
            {
                "id": "section-1",
                "label": "Section",
                "entries": [
                    {"row": [{"fieldName": "a"}, {"fieldName": "b"}]}
                ]
            }
        ]
    })json");
    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(r.valid) << r.message;
}

// row 在顶层（无 section 包装）→ FAIL rowOutsideSection。
TEST(TaskAuthoringContractTest, FieldLayoutRowAtTopLevelRejected)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [
            {"name": "a", "label": "A", "valueType": 4, "inputType": 0, "value": ""}
        ],
        "fieldLayout": [
            {"row": [{"fieldName": "a"}]}
        ]
    })json");
    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "rowOutsideSection");
}

// 同一 commandId 被多次引用 → FAIL layoutDuplicateCommandRef。
TEST(TaskAuthoringContractTest, FieldLayoutDuplicateCommandRef)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [
            {"name": "a", "label": "A", "valueType": 4, "inputType": 0, "value": ""}
        ],
        "fieldLayout": [
            {
                "id": "section-1",
                "label": "Section",
                "entries": [
                    {"fieldName": "a"},
                    {"commandId": "checkUpdate"},
                    {"commandId": "checkUpdate"}
                ]
            }
        ],
        "commands": [
            {
                "id": "checkUpdate",
                "label": "Check Update",
                "apply": {"kind": "invokeCommand", "payload": {"commandId": "checkUpdate"}}
            }
        ]
    })json");
    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "layoutDuplicateCommandRef");
}

// layout 同时含 fieldName 叶子和 commandId 叶子，
// fieldName 全覆盖断言仍成立（commandId 不污染 referenced_names）。
TEST(TaskAuthoringContractTest, FieldLayoutMixedLeavesPreservesFieldCoverage)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [
            {"name": "a", "label": "A", "valueType": 4, "inputType": 0, "value": ""},
            {"name": "b", "label": "B", "valueType": 4, "inputType": 0, "value": ""}
        ],
        "fieldLayout": [
            {
                "id": "section-1",
                "label": "Section",
                "entries": [
                    {"row": [{"fieldName": "a"}, {"fieldName": "b"}]},
                    {"commandId": "checkUpdate"}
                ]
            }
        ],
        "commands": [
            {
                "id": "checkUpdate",
                "label": "Check Update",
                "apply": {"kind": "invokeCommand", "payload": {"commandId": "checkUpdate"}}
            }
        ]
    })json");
    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(r.valid) << r.message;
}

TEST(TaskAuthoringContractTest, FormSequenceItemStrictShape)
{
    auto valid = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "formSequence": {
            "items": [
                {
                    "id": "task-1",
                    "componentGuid": "00000000-0000-0000-0000-000000000001",
                    "fields": [],
                    "enabled": true,
                    "debugInfo": {"any": "opaque"}
                }
            ]
        }
    })json");
    auto r1 = Das::Core::TaskScheduler::ValidateAuthoringDocument(valid);
    EXPECT_TRUE(r1.valid) << r1.message;

    auto with_settings = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "formSequence": {
            "items": [{"id": "x", "settings": {}}]
        }
    })json");
    auto r2 =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(with_settings);
    EXPECT_FALSE(r2.valid);
    EXPECT_EQ(r2.error_kind, "unknownField");

    auto with_children = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "formSequence": {
            "items": [{"id": "x", "children": []}]
        }
    })json");
    auto r3 =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(with_children);
    EXPECT_FALSE(r3.valid);
    EXPECT_EQ(r3.error_kind, "unknownField");
}

TEST(TaskAuthoringContractTest, GraphNodeStrictShape)
{
    auto valid = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 0,
        "graph": {
            "nodes": [
                {
                    "id": "node-1",
                    "componentGuid": "00000000-0000-0000-0000-000000000002",
                    "fields": [],
                    "debugInfo": {}
                }
            ]
        }
    })json");
    auto r1 = Das::Core::TaskScheduler::ValidateAuthoringDocument(valid);
    EXPECT_TRUE(r1.valid) << r1.message;

    auto with_settings = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 0,
        "graph": {"nodes": [{"id": "n", "settings": {}}]}
    })json");
    auto r2 =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(with_settings);
    EXPECT_FALSE(r2.valid);
    EXPECT_EQ(r2.error_kind, "unknownField");
}

TEST(TaskAuthoringContractTest, EntryRefOnlyEntryIdAllowed)
{
    auto valid = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 0,
        "graph": {"nodes": [{"id": "n", "targetKind": "entryRef", "entryRef": {"entryId": 5}}]}
    })json");
    auto r1 = Das::Core::TaskScheduler::ValidateAuthoringDocument(valid);
    EXPECT_TRUE(r1.valid) << r1.message;

    auto legacy = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 0,
        "graph": {"nodes": [{"id": "n", "targetKind": "entryRef", "entryRef": {
            "entryId": 5, "expectedRevision": 1, "sourceFingerprint": "abc"
        }}]}
    })json");
    auto r2 = Das::Core::TaskScheduler::ValidateAuthoringDocument(legacy);
    EXPECT_FALSE(r2.valid);
    EXPECT_EQ(r2.error_kind, "unknownField");
}

TEST(TaskAuthoringContractTest, ActionCatalogStrictShape)
{
    auto valid = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "actionCatalog": {
            "entries": [
                {"actionId": "A", "label": "Alpha"},
                {"actionId": "B", "label": "Beta", "description": "d", "icon": "https://x/y.png"}
            ],
            "groups": [
                {"id": "g1", "label": "Group", "actionIds": ["A", "B"]}
            ]
        }
    })json");
    auto r1 = Das::Core::TaskScheduler::ValidateAuthoringDocument(valid);
    EXPECT_TRUE(r1.valid) << r1.message;

    auto duplicate_action = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "actionCatalog": {
            "entries": [
                {"actionId": "A", "label": "Alpha"},
                {"actionId": "A", "label": "Alpha again"}
            ]
        }
    })json");
    auto r2 =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(duplicate_action);
    EXPECT_FALSE(r2.valid);
    EXPECT_EQ(r2.error_kind, "duplicateActionId");

    auto group_unknown_action = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "actionCatalog": {
            "entries": [{"actionId": "A", "label": "Alpha"}],
            "groups": [{"id": "g", "label": "G", "actionIds": ["A", "MISSING"]}]
        }
    })json");
    auto r3 = Das::Core::TaskScheduler::ValidateAuthoringDocument(
        group_unknown_action);
    EXPECT_FALSE(r3.valid);
    EXPECT_EQ(r3.error_kind, "groupActionIdNotFound");
}

TEST(TaskAuthoringContractTest, ChangeRequiresBaseRevision)
{
    auto change = ParseJson(R"json({
        "kind": "setValue",
        "payload": {
            "target": {"kind": "document"},
            "fieldName": "projectFolder",
            "value": "C:/maa"
        }
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringChange(change);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "missingField");
}

TEST(TaskAuthoringContractTest, SupportedChangeKindsPass)
{
    struct Case
    {
        std::string_view kind;
        std::string_view payload;
    };
    constexpr Case cases[] = {
        {"setValue",
         R"({"target":{"kind":"document"},"fieldName":"x","value":null})"},
        {"invokeCommand", R"({"commandId":"do-thing"})"},
        {"convertView", R"({"targetViewKind":"graph"})"},
        {"addFormSequenceItem", R"({"actionId":"A"})"},
        {"addFormSequenceItem", R"({"actionId":"A","insertionIndex":0})"},
        {"moveFormSequenceItem",
         R"({"formSequenceItemId":"item-1","destinationIndex":2})"},
        {"removeFormSequenceItem", R"({"formSequenceItemId":"item-1"})"},
        {"addGraphNode", R"({"actionId":"A"})"},
        {"removeGraphNode", R"({"nodeId":"n-1"})"},
        {"connectGraphPorts",
         R"({"fromNodeId":"a","fromPortId":"p1","toNodeId":"b","toPortId":"p2"})"},
        {"disconnectGraphPorts",
         R"({"fromNodeId":"a","fromPortId":"p1","toNodeId":"b","toPortId":"p2"})"},
        {"moveGraphNode", R"({"nodeId":"n-1","position":{"x":12.5,"y":-3}})"},
        {"removeGraphNodes",
         R"({"nodeIds":["n-1","n-2"],"connections":[{"fromNodeId":"a","fromPortId":"p","toNodeId":"b","toPortId":"q"}]})"},
        {"moveGraphNodes",
         R"({"moves":[{"nodeId":"n-1","position":{"x":1.5,"y":2}}]})"},
        // uiEvent：Core 契约层只放行 kind，payload 语义由 MaaPI
        // ApplyUiEventChange 校验（Core 不解 ui.values 黑盒）。
        {"uiEvent",
         R"({"type":"sectionExpandChanged","target":{"sectionId":"s1"},"data":{"expanded":true}})"},
    };

    for (const auto& c : cases)
    {
        auto change = ParseJson(
            std::string{R"({"baseRevision":1,"kind":")"} + std::string{c.kind}
            + R"(","payload":)" + std::string{c.payload} + "}");

        auto result = Das::Core::TaskScheduler::ValidateAuthoringChange(change);

        EXPECT_TRUE(result.valid) << c.kind << ": " << result.message;
    }
}

TEST(TaskAuthoringContractTest, ChangeRejectsOldKindAliases)
{
    constexpr std::string_view legacy_tag[] = {
        "applyPreset",
        "addSequenceItem",
        "moveSequenceItem",
        "removeSequenceItem",
        "addNode",
        "removeNode",
        "connectPorts",
        "disconnectPorts",
        "updateNodeConfig"};

    for (auto kind : legacy_tag)
    {
        auto change = ParseJson(
            std::string{R"({"baseRevision":1,"kind":")"} + std::string{kind}
            + R"(","payload":{}})");

        auto result = Das::Core::TaskScheduler::ValidateAuthoringChange(change);

        EXPECT_FALSE(result.valid) << kind;
        EXPECT_EQ(result.error_kind, "unsupportedChange") << kind;
    }
}

TEST(TaskAuthoringContractTest, SetValueTargetVariants)
{
    constexpr std::string_view targets[] = {
        R"({"kind":"document"})",
        R"({"kind":"formSequenceItem","id":"item-1"})",
        R"({"kind":"graphNode","id":"node-1"})"};

    for (auto target : targets)
    {
        auto change = ParseJson(
            std::string{R"({"baseRevision":1,"kind":"setValue","payload":)"}
            + "{\"target\":" + std::string{target}
            + R"(,"fieldName":"x","value":null}})");

        auto result = Das::Core::TaskScheduler::ValidateAuthoringChange(change);

        EXPECT_TRUE(result.valid) << target << ": " << result.message;
    }
}

TEST(TaskAuthoringContractTest, SetValueRejectsInvalidTargetKind)
{
    auto change = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "setValue",
        "payload": {
            "target": {"kind": "section", "id": "x"},
            "fieldName": "f",
            "value": null
        }
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringChange(change);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "invalidTargetKind");
}

TEST(TaskAuthoringContractTest, SetValueTargetRequiresIdForItemKind)
{
    auto missing_id = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "setValue",
        "payload": {
            "target": {"kind": "formSequenceItem"},
            "fieldName": "f",
            "value": null
        }
    })json");
    auto r1 = Das::Core::TaskScheduler::ValidateAuthoringChange(missing_id);
    EXPECT_FALSE(r1.valid);
    EXPECT_EQ(r1.error_kind, "missingField");

    auto with_extra = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "setValue",
        "payload": {
            "target": {"kind": "document", "id": "should-not-be-here"},
            "fieldName": "f",
            "value": null
        }
    })json");
    auto r2 = Das::Core::TaskScheduler::ValidateAuthoringChange(with_extra);
    EXPECT_FALSE(r2.valid);
    EXPECT_EQ(r2.error_kind, "unknownField");
}

TEST(TaskAuthoringContractTest, SetValueRejectsExtraPayloadField)
{
    auto change = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "setValue",
        "payload": {
            "target": {"kind": "document"},
            "fieldName": "f",
            "value": null,
            "path": "legacy"
        }
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringChange(change);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "unknownField");
}

TEST(TaskAuthoringContractTest, InvokeCommandPayloadStrict)
{
    auto ok = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "invokeCommand",
        "payload": {"commandId": "new-config"}
    })json");
    auto r_ok = Das::Core::TaskScheduler::ValidateAuthoringChange(ok);
    EXPECT_TRUE(r_ok.valid) << r_ok.message;

    auto empty = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "invokeCommand",
        "payload": {"commandId": ""}
    })json");
    auto r_empty = Das::Core::TaskScheduler::ValidateAuthoringChange(empty);
    EXPECT_FALSE(r_empty.valid);
    EXPECT_EQ(r_empty.error_kind, "invalidCommandId");

    auto extra = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "invokeCommand",
        "payload": {"commandId": "x", "args": {}}
    })json");
    auto r_extra = Das::Core::TaskScheduler::ValidateAuthoringChange(extra);
    EXPECT_FALSE(r_extra.valid);
    EXPECT_EQ(r_extra.error_kind, "unknownField");
}

TEST(TaskAuthoringContractTest, ConvertViewPayloadStrict)
{
    auto ok = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "convertView",
        "payload": {"targetViewKind": "graph"}
    })json");
    auto r_ok = Das::Core::TaskScheduler::ValidateAuthoringChange(ok);
    EXPECT_TRUE(r_ok.valid) << r_ok.message;

    auto bad = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "convertView",
        "payload": {"targetViewKind": "custom"}
    })json");
    auto r_bad = Das::Core::TaskScheduler::ValidateAuthoringChange(bad);
    EXPECT_FALSE(r_bad.valid);
    EXPECT_EQ(r_bad.error_kind, "unsupportedViewKind");
}

TEST(TaskAuthoringContractTest, FormSequenceItemChangePayloadsStrict)
{
    auto add_with_index = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "addFormSequenceItem",
        "payload": {"actionId": "A", "insertionIndex": 3}
    })json");
    auto r1 = Das::Core::TaskScheduler::ValidateAuthoringChange(add_with_index);
    EXPECT_TRUE(r1.valid) << r1.message;

    auto move = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "moveFormSequenceItem",
        "payload": {"formSequenceItemId": "i-1", "destinationIndex": 0}
    })json");
    auto r2 = Das::Core::TaskScheduler::ValidateAuthoringChange(move);
    EXPECT_TRUE(r2.valid) << r2.message;

    auto remove = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "removeFormSequenceItem",
        "payload": {"formSequenceItemId": "i-1"}
    })json");
    auto r3 = Das::Core::TaskScheduler::ValidateAuthoringChange(remove);
    EXPECT_TRUE(r3.valid) << r3.message;

    auto add_extra = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "addFormSequenceItem",
        "payload": {"actionId": "A", "itemId": "legacy"}
    })json");
    auto r4 = Das::Core::TaskScheduler::ValidateAuthoringChange(add_extra);
    EXPECT_FALSE(r4.valid);
    EXPECT_EQ(r4.error_kind, "unknownField");
}

TEST(TaskAuthoringContractTest, GraphNodeChangePayloadsStrict)
{
    auto add = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "addGraphNode",
        "payload": {"actionId": "A"}
    })json");
    auto r1 = Das::Core::TaskScheduler::ValidateAuthoringChange(add);
    EXPECT_TRUE(r1.valid) << r1.message;

    auto remove = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "removeGraphNode",
        "payload": {"nodeId": "n-1"}
    })json");
    auto r2 = Das::Core::TaskScheduler::ValidateAuthoringChange(remove);
    EXPECT_TRUE(r2.valid) << r2.message;

    auto connect = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "connectGraphPorts",
        "payload": {
            "fromNodeId": "a", "fromPortId": "p1",
            "toNodeId": "b", "toPortId": "p2"
        }
    })json");
    auto r3 = Das::Core::TaskScheduler::ValidateAuthoringChange(connect);
    EXPECT_TRUE(r3.valid) << r3.message;

    auto disconnect = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "disconnectGraphPorts",
        "payload": {
            "fromNodeId": "a", "fromPortId": "p1",
            "toNodeId": "b", "toPortId": "p2"
        }
    })json");
    auto r4 = Das::Core::TaskScheduler::ValidateAuthoringChange(disconnect);
    EXPECT_TRUE(r4.valid) << r4.message;
}

// moveGraphNode：节点布局坐标走文档级 viewData 轻量通道。payload 键集
// 锁死 {nodeId, position{x,y}}，多余键/空 id/非数值或非有限坐标一律显式
// 拒绝，防任意 viewData 子树注入。
TEST(TaskAuthoringContractTest, MoveGraphNodeChangePayloadStrict)
{
    auto ok = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNode",
        "payload": {"nodeId": "n1", "position": {"x": 1.5, "y": 2.5}}
    })json");
    auto r_ok = Das::Core::TaskScheduler::ValidateAuthoringChange(ok);
    EXPECT_TRUE(r_ok.valid) << r_ok.message;

    // 整数格式坐标等价放行（C# 序列化器把 100.0 写成 100）。
    auto integer_coordinates = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNode",
        "payload": {"nodeId": "n1", "position": {"x": 30, "y": -40}}
    })json");
    auto r_integers =
        Das::Core::TaskScheduler::ValidateAuthoringChange(integer_coordinates);
    EXPECT_TRUE(r_integers.valid) << r_integers.message;

    auto extra_payload_key = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNode",
        "payload": {
            "nodeId": "n1",
            "position": {"x": 1.5, "y": 2.5},
            "extra": true
        }
    })json");
    auto r_extra =
        Das::Core::TaskScheduler::ValidateAuthoringChange(extra_payload_key);
    EXPECT_FALSE(r_extra.valid);
    EXPECT_EQ(r_extra.error_kind, "unknownField");

    auto empty_node_id = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNode",
        "payload": {"nodeId": "", "position": {"x": 1.5, "y": 2.5}}
    })json");
    auto r_empty =
        Das::Core::TaskScheduler::ValidateAuthoringChange(empty_node_id);
    EXPECT_FALSE(r_empty.valid);
    EXPECT_EQ(r_empty.error_kind, "invalidNodeId");

    auto missing_node_id = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNode",
        "payload": {"position": {"x": 1.5, "y": 2.5}}
    })json");
    auto r_missing =
        Das::Core::TaskScheduler::ValidateAuthoringChange(missing_node_id);
    EXPECT_FALSE(r_missing.valid);
    EXPECT_EQ(r_missing.error_kind, "missingField");

    auto string_coordinate = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNode",
        "payload": {"nodeId": "n1", "position": {"x": "1.5", "y": 2.5}}
    })json");
    auto r_string =
        Das::Core::TaskScheduler::ValidateAuthoringChange(string_coordinate);
    EXPECT_FALSE(r_string.valid);
    EXPECT_EQ(r_string.error_kind, "invalidPosition");

    auto missing_coordinate = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNode",
        "payload": {"nodeId": "n1", "position": {"y": 2.5}}
    })json");
    auto r_missing_coordinate =
        Das::Core::TaskScheduler::ValidateAuthoringChange(missing_coordinate);
    EXPECT_FALSE(r_missing_coordinate.valid);
    EXPECT_EQ(r_missing_coordinate.error_kind, "missingField");

    // 非有限坐标（Infinity 经 AllowInfAndNan 语法进入）必须拒绝。
    auto infinite_coordinate = ParseJsonAllowingNonFinite(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNode",
        "payload": {"nodeId": "n1", "position": {"x": Infinity, "y": 2.5}}
    })json");
    auto r_infinite =
        Das::Core::TaskScheduler::ValidateAuthoringChange(infinite_coordinate);
    EXPECT_FALSE(r_infinite.valid);
    EXPECT_EQ(r_infinite.error_kind, "invalidPosition");

    auto extra_position_key = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNode",
        "payload": {"nodeId": "n1", "position": {"x": 1.5, "y": 2.5, "z": 0}}
    })json");
    auto r_extra_position =
        Das::Core::TaskScheduler::ValidateAuthoringChange(extra_position_key);
    EXPECT_FALSE(r_extra_position.valid);
    EXPECT_EQ(r_extra_position.error_kind, "unknownField");
}

// removeGraphNodes（批量删除）：nodeIds 必须是非空字符串数组且无重复，
// connections 可选（每项四键齐全非空）；nodeIds 与 connections 至少一项
// 非空；多余键 unknownField。原子批量删除的服务端半边入口。
TEST(TaskAuthoringContractTest, RemoveGraphNodesChangePayloadStrict)
{
    auto ok = ParseJson(R"json({
        "baseRevision": 3,
        "kind": "removeGraphNodes",
        "payload": {
            "nodeIds": ["a", "b"],
            "connections": [
                {"fromNodeId": "n", "fromPortId": "p",
                 "toNodeId": "m", "toPortId": "q"}
            ]
        }
    })json");
    auto r_ok = Das::Core::TaskScheduler::ValidateAuthoringChange(ok);
    EXPECT_TRUE(r_ok.valid) << r_ok.message;

    // nodeIds 单独出现（不带 connections）合法。
    auto node_ids_only = ParseJson(R"json({
        "baseRevision": 3,
        "kind": "removeGraphNodes",
        "payload": {"nodeIds": ["a"]}
    })json");
    auto r_node_ids_only =
        Das::Core::TaskScheduler::ValidateAuthoringChange(node_ids_only);
    EXPECT_TRUE(r_node_ids_only.valid) << r_node_ids_only.message;

    // connections 单独出现（连线批量删除，端点节点保留）合法。
    auto connections_only = ParseJson(R"json({
        "baseRevision": 3,
        "kind": "removeGraphNodes",
        "payload": {
            "connections": [
                {"fromNodeId": "n", "fromPortId": "p",
                 "toNodeId": "m", "toPortId": "q"}
            ]
        }
    })json");
    auto r_connections_only =
        Das::Core::TaskScheduler::ValidateAuthoringChange(connections_only);
    EXPECT_TRUE(r_connections_only.valid) << r_connections_only.message;

    // nodeIds 空数组且 connections 缺失 → 至少一项非空不成立。
    auto both_empty = ParseJson(R"json({
        "baseRevision": 3,
        "kind": "removeGraphNodes",
        "payload": {"nodeIds": []}
    })json");
    auto r_both_empty =
        Das::Core::TaskScheduler::ValidateAuthoringChange(both_empty);
    EXPECT_FALSE(r_both_empty.valid);
    EXPECT_EQ(r_both_empty.error_kind, "invalidPayload");

    // nodeIds 含空串 → invalidNodeId。
    auto empty_node_id = ParseJson(R"json({
        "baseRevision": 3,
        "kind": "removeGraphNodes",
        "payload": {"nodeIds": ["a", ""]}
    })json");
    auto r_empty_node_id =
        Das::Core::TaskScheduler::ValidateAuthoringChange(empty_node_id);
    EXPECT_FALSE(r_empty_node_id.valid);
    EXPECT_EQ(r_empty_node_id.error_kind, "invalidNodeId");

    // nodeIds 含非字符串项 → invalidNodeId。
    auto non_string_node_id = ParseJson(R"json({
        "baseRevision": 3,
        "kind": "removeGraphNodes",
        "payload": {"nodeIds": ["a", 7]}
    })json");
    auto r_non_string =
        Das::Core::TaskScheduler::ValidateAuthoringChange(non_string_node_id);
    EXPECT_FALSE(r_non_string.valid);
    EXPECT_EQ(r_non_string.error_kind, "invalidNodeId");

    // nodeIds 重复 → invalidPayload。
    auto duplicate_node_ids = ParseJson(R"json({
        "baseRevision": 3,
        "kind": "removeGraphNodes",
        "payload": {"nodeIds": ["a", "a"]}
    })json");
    auto r_duplicate =
        Das::Core::TaskScheduler::ValidateAuthoringChange(duplicate_node_ids);
    EXPECT_FALSE(r_duplicate.valid);
    EXPECT_EQ(r_duplicate.error_kind, "invalidPayload");

    // connections 单项缺四键之一 → missingField。
    auto missing_connection_key = ParseJson(R"json({
        "baseRevision": 3,
        "kind": "removeGraphNodes",
        "payload": {
            "nodeIds": ["a"],
            "connections": [
                {"fromNodeId": "n", "fromPortId": "p", "toNodeId": "m"}
            ]
        }
    })json");
    auto r_missing_key = Das::Core::TaskScheduler::ValidateAuthoringChange(
        missing_connection_key);
    EXPECT_FALSE(r_missing_key.valid);
    EXPECT_EQ(r_missing_key.error_kind, "missingField");

    // connections 单项值为空串 → missingField。
    auto empty_connection_value = ParseJson(R"json({
        "baseRevision": 3,
        "kind": "removeGraphNodes",
        "payload": {
            "nodeIds": ["a"],
            "connections": [
                {"fromNodeId": "n", "fromPortId": "", "toNodeId": "m",
                 "toPortId": "q"}
            ]
        }
    })json");
    auto r_empty_value = Das::Core::TaskScheduler::ValidateAuthoringChange(
        empty_connection_value);
    EXPECT_FALSE(r_empty_value.valid);
    EXPECT_EQ(r_empty_value.error_kind, "missingField");

    // 未知键 → unknownField。
    auto extra_key = ParseJson(R"json({
        "baseRevision": 3,
        "kind": "removeGraphNodes",
        "payload": {"nodeIds": [], "extra": 1}
    })json");
    auto r_extra_key =
        Das::Core::TaskScheduler::ValidateAuthoringChange(extra_key);
    EXPECT_FALSE(r_extra_key.valid);
    EXPECT_EQ(r_extra_key.error_kind, "unknownField");

    // nodeIds 缺失且 connections 为空数组 → 至少一项非空不成立。
    auto missing_node_ids = ParseJson(R"json({
        "baseRevision": 3,
        "kind": "removeGraphNodes",
        "payload": {"connections": []}
    })json");
    auto r_missing_node_ids =
        Das::Core::TaskScheduler::ValidateAuthoringChange(missing_node_ids);
    EXPECT_FALSE(r_missing_node_ids.valid);
    EXPECT_EQ(r_missing_node_ids.error_kind, "invalidPayload");
}

// moveGraphNodes（批量坐标提交）：moves 必须为非空数组；每项
// OnlyKeys({nodeId, position})，nodeId 非空且整个 moves 内无重复；
// position 复用 moveGraphNode 的 as_num + isfinite 有限性检查。
TEST(TaskAuthoringContractTest, MoveGraphNodesChangePayloadStrict)
{
    auto ok = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNodes",
        "payload": {
            "moves": [
                {"nodeId": "a", "position": {"x": 1.5, "y": 2}},
                {"nodeId": "b", "position": {"x": -3, "y": 40.25}}
            ]
        }
    })json");
    auto r_ok = Das::Core::TaskScheduler::ValidateAuthoringChange(ok);
    EXPECT_TRUE(r_ok.valid) << r_ok.message;

    auto empty_moves = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNodes",
        "payload": {"moves": []}
    })json");
    auto r_empty_moves =
        Das::Core::TaskScheduler::ValidateAuthoringChange(empty_moves);
    EXPECT_FALSE(r_empty_moves.valid);
    EXPECT_EQ(r_empty_moves.error_kind, "invalidPayload");

    auto missing_moves = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNodes",
        "payload": {}
    })json");
    auto r_missing_moves =
        Das::Core::TaskScheduler::ValidateAuthoringChange(missing_moves);
    EXPECT_FALSE(r_missing_moves.valid);
    EXPECT_EQ(r_missing_moves.error_kind, "missingField");

    // moves 单项 nodeId 重复出现 → 拒绝。
    auto duplicate_node_id = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNodes",
        "payload": {
            "moves": [
                {"nodeId": "a", "position": {"x": 1, "y": 2}},
                {"nodeId": "a", "position": {"x": 3, "y": 4}}
            ]
        }
    })json");
    auto r_duplicate =
        Das::Core::TaskScheduler::ValidateAuthoringChange(duplicate_node_id);
    EXPECT_FALSE(r_duplicate.valid);
    EXPECT_EQ(r_duplicate.error_kind, "invalidPayload");

    // position 非数值 → invalidPosition。
    auto string_coordinate = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNodes",
        "payload": {
            "moves": [{"nodeId": "a", "position": {"x": "1.5", "y": 2}}]
        }
    })json");
    auto r_string =
        Das::Core::TaskScheduler::ValidateAuthoringChange(string_coordinate);
    EXPECT_FALSE(r_string.valid);
    EXPECT_EQ(r_string.error_kind, "invalidPosition");

    // 非有限坐标（Infinity 经 AllowInfAndNan 语法进入）→ invalidPosition。
    auto infinite_coordinate = ParseJsonAllowingNonFinite(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNodes",
        "payload": {
            "moves": [{"nodeId": "a", "position": {"x": Infinity, "y": 2}}]
        }
    })json");
    auto r_infinite =
        Das::Core::TaskScheduler::ValidateAuthoringChange(infinite_coordinate);
    EXPECT_FALSE(r_infinite.valid);
    EXPECT_EQ(r_infinite.error_kind, "invalidPosition");

    // moves 单项缺 position → missingField。
    auto missing_position = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNodes",
        "payload": {"moves": [{"nodeId": "a"}]}
    })json");
    auto r_missing_position =
        Das::Core::TaskScheduler::ValidateAuthoringChange(missing_position);
    EXPECT_FALSE(r_missing_position.valid);
    EXPECT_EQ(r_missing_position.error_kind, "missingField");

    // moves 单项多余键 → unknownField。
    auto extra_move_key = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNodes",
        "payload": {
            "moves": [
                {"nodeId": "a", "position": {"x": 1, "y": 2}, "extra": 0}
            ]
        }
    })json");
    auto r_extra_move_key =
        Das::Core::TaskScheduler::ValidateAuthoringChange(extra_move_key);
    EXPECT_FALSE(r_extra_move_key.valid);
    EXPECT_EQ(r_extra_move_key.error_kind, "unknownField");

    // 顶层多余键 → unknownField。
    auto extra_top_key = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNodes",
        "payload": {"moves": [{"nodeId": "a", "position": {"x": 1, "y": 2}}],
                    "extra": 1}
    })json");
    auto r_extra_top_key =
        Das::Core::TaskScheduler::ValidateAuthoringChange(extra_top_key);
    EXPECT_FALSE(r_extra_top_key.valid);
    EXPECT_EQ(r_extra_top_key.error_kind, "unknownField");
}

// schema 契约与 C++ 白名单对账：AuthoringChangeKind enum 集合逐项一致
// （缺项/多项都在此暴露，防 C++/schema/前端三方漂移）；MoveGraphNode
// $def 锁死 {nodeId, position{x,y}} 键集与数值类型。
TEST(TaskAuthoringContractTest, SchemaJsonTracksChangeKindWhitelist)
{
    const auto schema_path = ResolveSchemaJsonPath();
    ASSERT_TRUE(schema_path.has_value())
        << "TaskAuthoringHttpContract.schema.json not found above "
        << std::filesystem::current_path();
    const auto schema_text = ReadFileText(*schema_path);
    ASSERT_TRUE(schema_text.has_value());

    auto schema = ParseJson(*schema_text);
    auto schema_object = schema.as_object();
    ASSERT_TRUE(schema_object.has_value());
    auto defs_value = (*schema_object)[std::string_view("$defs")];
    auto defs_object = defs_value.as_object();
    ASSERT_TRUE(defs_object.has_value());

    auto kind_def =
        (*defs_object)[std::string_view("AuthoringChangeKind")].as_object();
    ASSERT_TRUE(kind_def.has_value());
    auto enum_array = (*kind_def)[std::string_view("enum")].as_array();
    ASSERT_TRUE(enum_array.has_value());
    std::set<std::string> schema_kinds;
    for (const auto& item : *enum_array)
    {
        auto text = item.as_string();
        ASSERT_TRUE(text.has_value());
        schema_kinds.insert(std::string(*text));
    }
    // 与 TaskAuthoringContract.cpp 的 CHANGE_KINDS 白名单逐项一致。
    const std::set<std::string> expected_kinds{
        "setValue",
        "invokeCommand",
        "convertView",
        "addFormSequenceItem",
        "moveFormSequenceItem",
        "removeFormSequenceItem",
        "addGraphNode",
        "removeGraphNode",
        "connectGraphPorts",
        "disconnectGraphPorts",
        "uiEvent",
        "moveGraphNode",
        "removeGraphNodes",
        "moveGraphNodes"};
    EXPECT_EQ(schema_kinds, expected_kinds);

    auto move_def =
        (*defs_object)[std::string_view("MoveGraphNode")].as_object();
    ASSERT_TRUE(move_def.has_value());
    auto no_extra_fields =
        (*move_def)[std::string_view("additionalProperties")].as_bool();
    ASSERT_TRUE(no_extra_fields.has_value());
    EXPECT_FALSE(*no_extra_fields);

    std::set<std::string> required_fields;
    auto required_array = (*move_def)[std::string_view("required")].as_array();
    ASSERT_TRUE(required_array.has_value());
    for (const auto& item : *required_array)
    {
        auto text = item.as_string();
        ASSERT_TRUE(text.has_value());
        required_fields.insert(std::string(*text));
    }
    const std::set<std::string> expected_required{"nodeId", "position"};
    EXPECT_EQ(required_fields, expected_required);

    auto properties = (*move_def)[std::string_view("properties")].as_object();
    ASSERT_TRUE(properties.has_value());
    auto node_id = (*properties)[std::string_view("nodeId")].as_object();
    ASSERT_TRUE(node_id.has_value());
    EXPECT_EQ(
        (*node_id)[std::string_view("type")].as_string().value(),
        "string");
    EXPECT_EQ((*node_id)[std::string_view("minLength")].as_sint().value(), 1);

    auto position = (*properties)[std::string_view("position")].as_object();
    ASSERT_TRUE(position.has_value());
    EXPECT_EQ(
        (*position)[std::string_view("type")].as_string().value(),
        "object");
    auto position_no_extra =
        (*position)[std::string_view("additionalProperties")].as_bool();
    ASSERT_TRUE(position_no_extra.has_value());
    EXPECT_FALSE(*position_no_extra);

    std::set<std::string> position_required;
    auto                  position_required_array =
        (*position)[std::string_view("required")].as_array();
    ASSERT_TRUE(position_required_array.has_value());
    for (const auto& item : *position_required_array)
    {
        auto text = item.as_string();
        ASSERT_TRUE(text.has_value());
        position_required.insert(std::string(*text));
    }
    const std::set<std::string> expected_position_required{"x", "y"};
    EXPECT_EQ(position_required, expected_position_required);

    auto position_properties =
        (*position)[std::string_view("properties")].as_object();
    ASSERT_TRUE(position_properties.has_value());
    for (const std::string_view axis : {"x", "y"})
    {
        auto axis_definition =
            (*position_properties)[std::string_view(axis)].as_object();
        ASSERT_TRUE(axis_definition.has_value()) << axis;
        EXPECT_EQ(
            (*axis_definition)[std::string_view("type")].as_string().value(),
            "number")
            << axis;
    }

    // AuthoringChangePayload 的 oneOf 分支集含 MoveGraphNode $ref。
    auto payload_definition =
        (*defs_object)[std::string_view("AuthoringChangePayload")].as_object();
    ASSERT_TRUE(payload_definition.has_value());
    auto payload_branches =
        (*payload_definition)[std::string_view("oneOf")].as_array();
    ASSERT_TRUE(payload_branches.has_value());
    bool found_move_branch = false;
    for (const auto& branch : *payload_branches)
    {
        auto branch_object = branch.as_object();
        if (!branch_object)
        {
            continue;
        }
        if (branch_object->contains(std::string_view("$ref")))
        {
            auto reference =
                (*branch_object)[std::string_view("$ref")].as_string();
            if (reference
                && *reference == std::string_view("#/$defs/MoveGraphNode"))
            {
                found_move_branch = true;
            }
        }
    }
    EXPECT_TRUE(found_move_branch);

    // RemoveGraphNodes $def：nodeIds minItems 1 字符串数组 + 可选
    // connections 四键数组；additionalProperties:false 两级锁死。
    auto remove_nodes_def =
        (*defs_object)[std::string_view("RemoveGraphNodes")].as_object();
    ASSERT_TRUE(remove_nodes_def.has_value());
    auto remove_nodes_no_extra =
        (*remove_nodes_def)[std::string_view("additionalProperties")].as_bool();
    ASSERT_TRUE(remove_nodes_no_extra.has_value());
    EXPECT_FALSE(*remove_nodes_no_extra);
    auto remove_nodes_properties =
        (*remove_nodes_def)[std::string_view("properties")].as_object();
    ASSERT_TRUE(remove_nodes_properties.has_value());
    auto node_ids_definition =
        (*remove_nodes_properties)[std::string_view("nodeIds")].as_object();
    ASSERT_TRUE(node_ids_definition.has_value());
    EXPECT_EQ(
        (*node_ids_definition)[std::string_view("type")].as_string().value(),
        "array");
    EXPECT_EQ(
        (*node_ids_definition)[std::string_view("minItems")].as_sint().value(),
        1);
    ASSERT_TRUE(
        remove_nodes_properties->contains(std::string_view("connections")));
    ASSERT_TRUE((*remove_nodes_properties)[std::string_view("connections")]
                    .as_object()
                    .has_value());
    bool found_remove_nodes_branch = false;
    for (const auto& branch : *payload_branches)
    {
        auto branch_object = branch.as_object();
        if (branch_object && branch_object->contains(std::string_view("$ref")))
        {
            auto reference =
                (*branch_object)[std::string_view("$ref")].as_string();
            if (reference
                && *reference == std::string_view("#/$defs/RemoveGraphNodes"))
            {
                found_remove_nodes_branch = true;
            }
        }
    }
    EXPECT_TRUE(found_remove_nodes_branch);

    // MoveGraphNodes $def：moves minItems 1，元素 {nodeId, position{x,y}}
    // 键集与数值类型锁死。
    auto move_nodes_def =
        (*defs_object)[std::string_view("MoveGraphNodes")].as_object();
    ASSERT_TRUE(move_nodes_def.has_value());
    auto move_nodes_required =
        (*move_nodes_def)[std::string_view("required")].as_array();
    ASSERT_TRUE(move_nodes_required.has_value());
    std::set<std::string> move_nodes_required_fields;
    for (const auto& item : *move_nodes_required)
    {
        auto text = item.as_string();
        ASSERT_TRUE(text.has_value());
        move_nodes_required_fields.insert(std::string(*text));
    }
    const std::set<std::string> expected_move_nodes_required{"moves"};
    EXPECT_EQ(move_nodes_required_fields, expected_move_nodes_required);
    auto move_nodes_properties =
        (*move_nodes_def)[std::string_view("properties")].as_object();
    ASSERT_TRUE(move_nodes_properties.has_value());
    auto moves_definition =
        (*move_nodes_properties)[std::string_view("moves")].as_object();
    ASSERT_TRUE(moves_definition.has_value());
    EXPECT_EQ(
        (*moves_definition)[std::string_view("type")].as_string().value(),
        "array");
    EXPECT_EQ(
        (*moves_definition)[std::string_view("minItems")].as_sint().value(),
        1);
    auto moves_items =
        (*moves_definition)[std::string_view("items")].as_object();
    ASSERT_TRUE(moves_items.has_value());
    auto moves_item_no_extra =
        (*moves_items)[std::string_view("additionalProperties")].as_bool();
    ASSERT_TRUE(moves_item_no_extra.has_value());
    EXPECT_FALSE(*moves_item_no_extra);
    bool found_move_nodes_branch = false;
    for (const auto& branch : *payload_branches)
    {
        auto branch_object = branch.as_object();
        if (branch_object && branch_object->contains(std::string_view("$ref")))
        {
            auto reference =
                (*branch_object)[std::string_view("$ref")].as_string();
            if (reference
                && *reference == std::string_view("#/$defs/MoveGraphNodes"))
            {
                found_move_nodes_branch = true;
            }
        }
    }
    EXPECT_TRUE(found_move_nodes_branch);
}

// schema GraphNodePort $def 对账：五键锁死（含 category）、category 枚举
// {execution, data}、additionalProperties:false——与 C++ 校验器
// ValidateGraphNodePort 的键集/值域逐项一致，防 C++/schema/前端三方漂移。
TEST(TaskAuthoringContractTest, SchemaJsonGraphNodePortCategoryLocked)
{
    const auto schema_path = ResolveSchemaJsonPath();
    ASSERT_TRUE(schema_path.has_value())
        << "TaskAuthoringHttpContract.schema.json not found above "
        << std::filesystem::current_path();
    const auto schema_text = ReadFileText(*schema_path);
    ASSERT_TRUE(schema_text.has_value());

    auto schema = ParseJson(*schema_text);
    auto schema_object = schema.as_object();
    ASSERT_TRUE(schema_object.has_value());
    auto defs_value = (*schema_object)[std::string_view("$defs")];
    auto defs_object = defs_value.as_object();
    ASSERT_TRUE(defs_object.has_value());

    auto port_def =
        (*defs_object)[std::string_view("GraphNodePort")].as_object();
    ASSERT_TRUE(port_def.has_value());

    auto no_extra_fields =
        (*port_def)[std::string_view("additionalProperties")].as_bool();
    ASSERT_TRUE(no_extra_fields.has_value());
    EXPECT_FALSE(*no_extra_fields);

    std::set<std::string> required_fields;
    auto required_array = (*port_def)[std::string_view("required")].as_array();
    ASSERT_TRUE(required_array.has_value());
    for (const auto& item : *required_array)
    {
        auto text = item.as_string();
        ASSERT_TRUE(text.has_value());
        required_fields.insert(std::string(*text));
    }
    const std::set<std::string> expected_required{
        "portId",
        "displayLabel",
        "portType",
        "direction",
        "category"};
    EXPECT_EQ(required_fields, expected_required);

    auto properties = (*port_def)[std::string_view("properties")].as_object();
    ASSERT_TRUE(properties.has_value());
    ASSERT_TRUE(properties->contains(std::string_view("category")));
    auto category_definition =
        (*properties)[std::string_view("category")].as_object();
    ASSERT_TRUE(category_definition.has_value());
    EXPECT_EQ(
        (*category_definition)[std::string_view("type")].as_string().value(),
        "string");
    auto category_enum =
        (*category_definition)[std::string_view("enum")].as_array();
    ASSERT_TRUE(category_enum.has_value());
    std::set<std::string> category_values;
    for (const auto& item : *category_enum)
    {
        auto text = item.as_string();
        ASSERT_TRUE(text.has_value());
        category_values.insert(std::string(*text));
    }
    const std::set<std::string> expected_category{"execution", "data"};
    EXPECT_EQ(category_values, expected_category);
}

TEST(TaskAuthoringContractTest, ChangeRejectsOpWrapper)
{
    auto change = ParseJson(R"json({
        "baseRevision": 1,
        "change": {"op": "setValue", "path": "x"}
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringChange(change);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "missingField");
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

// uiEvent 等短路 kind 的响应不带 document——前端据有无 document 分流，
// Core 契约层放行（document 可选）。
TEST(TaskAuthoringContractTest, ApplyResultPassesWithoutDocument)
{
    auto result = ParseJson(R"json({
        "acceptedProperties": {},
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

// moveGraphNode 走 viewData 轻量通道：成功也不推进语义 revision（GraphRuntime
// 外壳不 ++version），成功 envelope 须 revision == baseRevision；失败 envelope
// 维持既有「不推进」不变量；语义 kind 的 base+1 强制不变。
TEST(
    TaskAuthoringContractTest,
    ApplyResultMoveGraphNodeSuccessKeepsBaseRevision)
{
    auto result = ParseJson(R"json({
        "acceptedProperties": {},
        "document": {
            "contractVersion": 1,
            "viewKind": "graph",
            "revision": 4
        },
        "diagnostics": []
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(
        result,
        4,
        "moveGraphNode");
    EXPECT_TRUE(r.valid);
    EXPECT_TRUE(r.error_kind.empty()) << r.message;
}

TEST(
    TaskAuthoringContractTest,
    ApplyResultMoveGraphNodeSuccessRejectsBumpedRevision)
{
    auto result = ParseJson(R"json({
        "acceptedProperties": {},
        "document": {
            "contractVersion": 1,
            "viewKind": "graph",
            "revision": 5
        },
        "diagnostics": []
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(
        result,
        4,
        "moveGraphNode");
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "revisionMismatch");
}

TEST(
    TaskAuthoringContractTest,
    ApplyResultMoveGraphNodeFailureKeepsBaseRevision)
{
    auto result = ParseJson(R"json({
        "acceptedProperties": {},
        "document": {
            "contractVersion": 1,
            "viewKind": "graph",
            "revision": 4
        },
        "diagnostics": [
            {"severity": "error", "message": "moveGraphNode.nodeId required"}
        ]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(
        result,
        4,
        "moveGraphNode");
    EXPECT_TRUE(r.valid);
    EXPECT_TRUE(r.error_kind.empty()) << r.message;
}

// moveGraphNodes 与 moveGraphNode 同族：viewData 轻量通道，成功也不推进
// 语义 revision；错误地 +1 反而 revisionMismatch。
TEST(
    TaskAuthoringContractTest,
    ApplyResultMoveGraphNodesSuccessKeepsBaseRevision)
{
    auto result = ParseJson(R"json({
        "acceptedProperties": {},
        "document": {
            "contractVersion": 1,
            "viewKind": "graph",
            "revision": 4
        },
        "diagnostics": []
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(
        result,
        4,
        "moveGraphNodes");
    EXPECT_TRUE(r.valid);
    EXPECT_TRUE(r.error_kind.empty()) << r.message;
}

TEST(
    TaskAuthoringContractTest,
    ApplyResultMoveGraphNodesSuccessRejectsBumpedRevision)
{
    auto result = ParseJson(R"json({
        "acceptedProperties": {},
        "document": {
            "contractVersion": 1,
            "viewKind": "graph",
            "revision": 5
        },
        "diagnostics": []
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(
        result,
        4,
        "moveGraphNodes");
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "revisionMismatch");
}

// removeGraphNodes 是语义变更：成功必须 revision == baseRevision + 1；
// 未推进（== base）时 revisionMismatch（一条消息一次原子删除恰 +1）。
TEST(
    TaskAuthoringContractTest,
    ApplyResultRemoveGraphNodesSuccessRequiresBasePlusOne)
{
    auto bumped = ParseJson(R"json({
        "acceptedProperties": {},
        "document": {
            "contractVersion": 1,
            "viewKind": "graph",
            "revision": 5
        },
        "diagnostics": []
    })json");
    auto r_bumped = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(
        bumped,
        4,
        "removeGraphNodes");
    EXPECT_TRUE(r_bumped.valid) << r_bumped.message;

    auto unbumped = ParseJson(R"json({
        "acceptedProperties": {},
        "document": {
            "contractVersion": 1,
            "viewKind": "graph",
            "revision": 4
        },
        "diagnostics": []
    })json");
    auto r_unbumped = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(
        unbumped,
        4,
        "removeGraphNodes");
    EXPECT_FALSE(r_unbumped.valid);
    EXPECT_EQ(r_unbumped.error_kind, "revisionMismatch");
}

TEST(TaskAuthoringContractTest, ApplyResultSemanticKindStillRequiresBasePlusOne)
{
    auto bumped = ParseJson(R"json({
        "acceptedProperties": {},
        "document": {
            "contractVersion": 1,
            "viewKind": "graph",
            "revision": 5
        },
        "diagnostics": []
    })json");
    auto r_bumped = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(
        bumped,
        4,
        "connectGraphPorts");
    EXPECT_TRUE(r_bumped.valid) << r_bumped.message;

    auto unbumped = ParseJson(R"json({
        "acceptedProperties": {},
        "document": {
            "contractVersion": 1,
            "viewKind": "graph",
            "revision": 4
        },
        "diagnostics": []
    })json");
    auto r_unbumped = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(
        unbumped,
        4,
        "connectGraphPorts");
    EXPECT_FALSE(r_unbumped.valid);
    EXPECT_EQ(r_unbumped.error_kind, "revisionMismatch");
}

TEST(TaskAuthoringContractTest, ApplyResultPassesForProviderRejectedApply)
{
    // A provider-rejected apply is a legitimate failure envelope: error
    // diagnostics carry the real reason and the store must not advance
    // (document.revision stays at baseRevision). The validator must accept it
    // rather than misreport revisionMismatch, so the real reason can surface.
    auto result = ParseJson(R"json({
        "acceptedProperties": {},
        "document": {
            "contractVersion": 1,
            "viewKind": "formSequence",
            "revision": 4
        },
        "diagnostics": [
            {"severity": "error", "message": "Unknown preset name: preset_1"}
        ]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(result, 4);
    EXPECT_TRUE(r.valid);
    EXPECT_TRUE(r.error_kind.empty()) << r.message;
}

TEST(TaskAuthoringContractTest, HasErrorDiagnosticDetectsErrorSeverity)
{
    auto with_error = ParseJson(R"json([
        {"severity": "warning", "message": "m1"},
        {"severity": "error", "message": "Unknown preset name: preset_1"}
    ])json");
    EXPECT_TRUE(Das::Core::TaskScheduler::HasErrorDiagnostic(with_error));
    EXPECT_EQ(
        Das::Core::TaskScheduler::FirstErrorDiagnosticMessage(with_error),
        "Unknown preset name: preset_1");

    auto without_error = ParseJson(R"json([
        {"severity": "info", "message": "m1"},
        {"severity": "warning", "message": "m2"}
    ])json");
    EXPECT_FALSE(Das::Core::TaskScheduler::HasErrorDiagnostic(without_error));
    EXPECT_TRUE(
        Das::Core::TaskScheduler::FirstErrorDiagnosticMessage(without_error)
            .empty());
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

// =============================================================================
// Tracer 3: CommandDescriptor XOR apply/openView + apply template shape
// =============================================================================

TEST(TaskAuthoringContractTest, CommandDescriptorApplyOnlyPasses)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "commands": [
            {
                "id": "cmd-1",
                "label": "保存",
                "apply": {"kind": "setValue", "payload": {}}
            }
        ]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(r.valid) << r.message;
}

TEST(TaskAuthoringContractTest, CommandDescriptorOpenViewOnlyPasses)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "commands": [
            {"id": "open", "label": "目录", "openView": "actionCatalog"}
        ]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(r.valid) << r.message;
}

TEST(TaskAuthoringContractTest, CommandDescriptorRequiresXorAction)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "commands": [{"id": "noop", "label": "Noop"}]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "commandActionXorRequired");
}

TEST(TaskAuthoringContractTest, CommandDescriptorRejectsBothApplyAndOpenView)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "commands": [{
            "id": "x", "label": "X",
            "apply": {"kind": "setValue", "payload": {}},
            "openView": "actionCatalog"
        }]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "commandActionXorRequired");
}

TEST(TaskAuthoringContractTest, CommandDescriptorConfirmationOnlyWithApply)
{
    auto with_apply = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "commands": [{
            "id": "x", "label": "X",
            "confirmation": "确认删除？",
            "apply": {"kind": "setValue", "payload": {}}
        }]
    })json");
    auto r1 = Das::Core::TaskScheduler::ValidateAuthoringDocument(with_apply);
    EXPECT_TRUE(r1.valid) << r1.message;

    auto with_open_view = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "commands": [{
            "id": "x", "label": "X",
            "confirmation": "不应允许",
            "openView": "actionCatalog"
        }]
    })json");
    auto r2 =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(with_open_view);
    EXPECT_FALSE(r2.valid);
    EXPECT_EQ(r2.error_kind, "confirmationRequiresApply");
}

TEST(TaskAuthoringContractTest, CommandDescriptorConfirmationMustBeNonEmpty)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "commands": [{
            "id": "x", "label": "X",
            "confirmation": "",
            "apply": {"kind": "setValue", "payload": {}}
        }]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "invalidConfirmation");
}

TEST(
    TaskAuthoringContractTest,
    CommandDescriptorApplyTemplateRejectsBaseRevision)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "commands": [{
            "id": "x", "label": "X",
            "apply": {"baseRevision": 0, "kind": "setValue", "payload": {}}
        }]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "applyTemplateHasBaseRevision");
}

TEST(
    TaskAuthoringContractTest,
    CommandDescriptorApplyTemplateRequiresKindAndPayload)
{
    auto no_kind = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "commands": [{"id": "x", "label": "X", "apply": {"payload": {}}}]
    })json");
    auto r1 = Das::Core::TaskScheduler::ValidateAuthoringDocument(no_kind);
    EXPECT_FALSE(r1.valid);
    EXPECT_EQ(r1.error_kind, "missingField");

    auto no_payload = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "commands": [{"id": "x", "label": "X", "apply": {"kind": "setValue"}}]
    })json");
    auto r2 = Das::Core::TaskScheduler::ValidateAuthoringDocument(no_payload);
    EXPECT_FALSE(r2.valid);
    EXPECT_EQ(r2.error_kind, "missingField");
}

TEST(TaskAuthoringContractTest, CommandDescriptorOpenViewMustBeActionCatalog)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "commands": [{"id": "x", "label": "X", "openView": "custom"}]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "unsupportedOpenView");
}

TEST(TaskAuthoringContractTest, CommandDescriptorIdUniqueInContainer)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "commands": [
            {"id": "dup", "label": "A", "openView": "actionCatalog"},
            {"id": "dup", "label": "B", "openView": "actionCatalog"}
        ]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "duplicateCommandId");
}

TEST(TaskAuthoringContractTest, CommandDescriptorRejectsUnknownField)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "commands": [{
            "id": "x", "label": "X",
            "apply": {"kind": "setValue", "payload": {}},
            "buttonStyle": "primary"
        }]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "unknownField");
}

TEST(TaskAuthoringContractTest, FieldDescriptorCommandsValidated)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [{
            "name": "f", "label": "F", "valueType": 4, "inputType": 0, "value": "",
            "commands": [{"id": "f-cmd", "label": "F Cmd"}]
        }]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "commandActionXorRequired");
}

// =============================================================================
// markdown inputType=7 (readonly) + optional commandResult on Apply envelope
// =============================================================================

TEST(TaskAuthoringContractTest, MarkdownFieldRequiresReadonlyTrue)
{
    auto ok = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [{
            "name": "releaseNote", "label": "更新日志", "valueType": 4,
            "inputType": 7, "value": "# 标题", "readonly": true
        }]
    })json");
    auto r_ok = Das::Core::TaskScheduler::ValidateAuthoringDocument(ok);
    EXPECT_TRUE(r_ok.valid) << r_ok.message;

    auto missing_readonly = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [{
            "name": "releaseNote", "label": "更新日志", "valueType": 4,
            "inputType": 7, "value": "# 标题"
        }]
    })json");
    auto r_missing =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(missing_readonly);
    EXPECT_FALSE(r_missing.valid);
    EXPECT_EQ(r_missing.error_kind, "invalidReadonly");

    auto readonly_false = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [{
            "name": "releaseNote", "label": "更新日志", "valueType": 4,
            "inputType": 7, "value": "# 标题", "readonly": false
        }]
    })json");
    auto r_false =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(readonly_false);
    EXPECT_FALSE(r_false.valid);
    EXPECT_EQ(r_false.error_kind, "invalidReadonly");
}

TEST(TaskAuthoringContractTest, LegacyInputTypeSixRemainsEditable)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [{
            "name": "dir", "label": "目录", "valueType": 4,
            "inputType": 6, "value": "C:/x", "readonly": false
        }]
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);
    EXPECT_TRUE(r.valid) << r.message;
}

// inputType=8(time): 通用字段类型，强制 valueType=4(string "HH:MM")。
// Core 业务中立，只校验类型契约；HH:MM 语义由投影/渲染层负责。
TEST(TaskAuthoringContractTest, TimeInputTypeEightAcceptedWithValueTypeFour)
{
    auto ok = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [{
            "name": "dailyTime", "label": "定时", "valueType": 4,
            "inputType": 8, "value": "09:30"
        }]
    })json");
    auto r_ok = Das::Core::TaskScheduler::ValidateAuthoringDocument(ok);
    EXPECT_TRUE(r_ok.valid) << r_ok.message;
}

TEST(TaskAuthoringContractTest, TimeInputTypeEightRejectsNonStringValueType)
{
    auto as_int = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [{
            "name": "dailyTime", "label": "定时", "valueType": 0,
            "inputType": 8, "value": 930
        }]
    })json");
    auto r_int = Das::Core::TaskScheduler::ValidateAuthoringDocument(as_int);
    EXPECT_FALSE(r_int.valid);
    EXPECT_EQ(r_int.error_kind, "invalidValueType") << r_int.message;

    auto as_bool = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [{
            "name": "dailyTime", "label": "定时", "valueType": 8,
            "inputType": 8, "value": true
        }]
    })json");
    auto r_bool = Das::Core::TaskScheduler::ValidateAuthoringDocument(as_bool);
    EXPECT_FALSE(r_bool.valid);
    EXPECT_EQ(r_bool.error_kind, "invalidValueType") << r_bool.message;
}

// 回归：新增的 inputType=8 valueType 强制规则只对 8 生效，
// 既有 inputType 0-7 语义零变化（抽样 inputType=7 markdown 仍强制 readonly）。
TEST(TaskAuthoringContractTest, TimeInputTypeRuleScopedBelowEightUnchanged)
{
    auto markdown_without_readonly = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [{
            "name": "releaseNote", "label": "更新日志", "valueType": 4,
            "inputType": 7, "value": "# 标题"
        }]
    })json");
    auto r_md = Das::Core::TaskScheduler::ValidateAuthoringDocument(
        markdown_without_readonly);
    EXPECT_FALSE(r_md.valid);
    EXPECT_EQ(r_md.error_kind, "invalidReadonly");

    auto plain_int = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "fields": [{
            "name": "count", "label": "次数", "valueType": 0,
            "inputType": 0, "value": 5
        }]
    })json");
    auto r_plain =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(plain_int);
    EXPECT_TRUE(r_plain.valid) << r_plain.message;
}

TEST(TaskAuthoringContractTest, ApplyResultPassesWithCommandResult)
{
    auto result = ParseJson(R"json({
        "acceptedProperties": {},
        "document": {
            "contractVersion": 1,
            "viewKind": "formSequence",
            "revision": 5
        },
        "diagnostics": [],
        "commandResult": {
            "commandId": "checkUpdate",
            "data": {"progressKey": "a1b2c3d4"}
        }
    })json");

    auto r = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(result, 4);
    EXPECT_TRUE(r.valid);
    EXPECT_TRUE(r.error_kind.empty()) << r.message;
}

TEST(TaskAuthoringContractTest, ApplyResultRejectsMalformedCommandResult)
{
    auto missing_command_id = ParseJson(R"json({
        "acceptedProperties": {},
        "document": {
            "contractVersion": 1,
            "viewKind": "formSequence",
            "revision": 5
        },
        "diagnostics": [],
        "commandResult": {"data": {"progressKey": "x"}}
    })json");
    auto r1 = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(
        missing_command_id,
        4);
    EXPECT_FALSE(r1.valid);
    EXPECT_EQ(r1.error_kind, "invalidCommandResult");

    auto extra_field = ParseJson(R"json({
        "acceptedProperties": {},
        "document": {
            "contractVersion": 1,
            "viewKind": "formSequence",
            "revision": 5
        },
        "diagnostics": [],
        "commandResult": {
            "commandId": "checkUpdate",
            "data": {},
            "bogus": true
        }
    })json");
    auto r2 =
        Das::Core::TaskScheduler::ValidateAuthoringApplyResult(extra_field, 4);
    EXPECT_FALSE(r2.valid);
    EXPECT_EQ(r2.error_kind, "invalidCommandResult");
}

TEST(TaskAuthoringContractTest, ApplyResultPassesWithoutCommandResult)
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
