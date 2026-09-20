#include <das/Core/TaskScheduler/TaskAuthoringContract.h>

#include <das/Utils/DasJsonCore.h>
#include <das/Utils/PathText.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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
                directory
                / DAS_PATH_LITERAL("TaskAuthoringHttpContract.schema.json");
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

    // 既有用例的输入字面量保持单 change 形态（可读性），经此包装为批量
    // apply 请求 {baseRevision, changes:[{kind,payload}]} 后再校验。缺少
    // kind/payload 的输入按原样缺失透传（缺失类错误码仍由校验器报告），
    // 非对象输入原样透传走 invalidChange 路径。
    yyjson::value AsBatchApplyRequest(const yyjson::value& single_change)
    {
        auto single_object = single_change.as_object();
        if (!single_object)
        {
            return Das::Utils::CloneYyjsonValue(single_change);
        }
        yyjson::value request(Das::Utils::MakeYyjsonObject());
        auto          request_object = request.as_object();
        if (single_object->contains(std::string_view("baseRevision")))
        {
            auto base_revision =
                (*single_object)[std::string_view("baseRevision")].as_sint();
            if (base_revision.has_value())
            {
                (*request_object)[std::string_view("baseRevision")] =
                    *base_revision;
            }
        }
        yyjson::value element(Das::Utils::MakeYyjsonObject());
        auto          element_object = element.as_object();
        if (single_object->contains(std::string_view("kind")))
        {
            auto kind = (*single_object)[std::string_view("kind")].as_string();
            if (kind.has_value())
            {
                (*element_object)[std::string_view("kind")] = std::make_pair(
                    std::string_view(*kind),
                    yyjson::copy_string);
            }
        }
        if (single_object->contains(std::string_view("payload")))
        {
            (*element_object)[std::string_view("payload")] =
                Das::Utils::CloneYyjsonValue(
                    (*single_object)[std::string_view("payload")]);
        }
        auto changes = Das::Utils::MakeYyjsonArray();
        (*changes.as_array()).emplace_back(std::move(element));
        (*request_object)[std::string_view("changes")] = std::move(changes);
        return request;
    }

    Das::Core::TaskScheduler::AuthoringValidationResult
    ValidateBatchAuthoringChange(const yyjson::value& single_change)
    {
        return Das::Core::TaskScheduler::ValidateAuthoringChange(
            AsBatchApplyRequest(single_change));
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

TEST(TaskAuthoringContractTest, GraphNodePortOptionalValueKeyPasses)
{
    // The optional port default-value key: "value" carries the raw settings
    // entry bound to an unconnected input data port — any JSON shape. Ports
    // without the key stay valid (absence is the norm), and an unknown key
    // beyond the six-shape vocabulary still rejects.
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 2,
        "graph": {
            "nodes": [
                {
                    "id": "n1",
                    "ports": [
                        {"portId":"condition","displayLabel":"","portType":"bool","direction":"input","category":"data","value":true},
                        {"portId":"threshold","displayLabel":"","portType":"int","direction":"input","category":"data","value":{"nested":[1,2]}},
                        {"portId":"true","displayLabel":"True","portType":"signal","direction":"output","category":"execution"}
                    ]
                }
            ]
        }
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.error_kind.empty()) << result.message;
}

TEST(TaskAuthoringContractTest, GraphNodePortConnectedFromKeyPasses)
{
    // The optional port source-indicator key: "connectedFrom" carries the
    // backend-generated origin ("source_node::source_port") of a connected
    // input data port — the server re-projection channel, not a
    // client-side lookup. Ports without the key stay valid (absence is the
    // norm), and the shape-violations test keeps rejecting non-string
    // values and unknown extra keys.
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 2,
        "graph": {
            "nodes": [
                {
                    "id": "n1",
                    "ports": [
                        {"portId":"threshold","displayLabel":"","portType":"double","direction":"input","category":"data","connectedFrom":"n2::o"},
                        {"portId":"result","displayLabel":"","portType":"double","direction":"output","category":"data"}
                    ]
                }
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
        // non-string source-indicator key
        R"([{"portId":"x","displayLabel":"","portType":"bool","direction":"input","category":"data","connectedFrom":5}])",
        // empty source-indicator string (projection never emits empty)
        R"([{"portId":"x","displayLabel":"","portType":"bool","direction":"input","category":"data","connectedFrom":""}])",
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

// ===== 根文档 ports/types section（文档协议扩展，engaged-only）=====
//
// 动态口是 authoring 文档机制的能力（opt-in）：文档声明口（ports[]）、
// 文档编辑口（formSequence items）、编译消费口（nodeFaces 注入）。
// 两 section 缺席合法（既有文档零破坏），出现即按形状定稿逐字段校验。

TEST(TaskAuthoringContractTest, PortsTypesSectionsAbsentDocumentPasses)
{
    // engaged-only：缺 ports/types 两键的既有文档形状照常通过（零破坏）。
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 3,
        "formSequence": {"items": []}
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.error_kind.empty()) << result.message;
}

TEST(TaskAuthoringContractTest, PortsSectionValidEntriesPass)
{
    // ports[] 合法条目：最小三键 {id,type,direction}；label 可选缺省=id；
    // 空数组 engaged 合法。type 词汇表=九词内建 ∪ 同文档 types 注册名。
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "ports": [
            {"id": "x", "type": "string", "direction": "input"},
            {"id": "shot", "label": "截图", "type": "image", "direction": "output"}
        ],
        "types": []
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.error_kind.empty()) << result.message;
}

TEST(
    TaskAuthoringContractTest,
    PortsSectionTypeVocabularyIsNineWordsPlusRegistrations)
{
    // type="signal" 拒绝——九词 NODE_PORT_VALUE_TYPES 是唯一内建词表，
    // 不是 GetKnownTypes() 的 13 词超集（signal/json/object/null 禁入）。
    auto signal = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "ports": [{"id": "x", "type": "signal", "direction": "input"}]
    })json");
    auto signal_r = Das::Core::TaskScheduler::ValidateAuthoringDocument(signal);
    EXPECT_FALSE(signal_r.valid);
    EXPECT_EQ(signal_r.error_kind, "invalidValue") << signal_r.message;

    // type=同文档 types[].name 注册名：通过；types 无此名：拒绝。
    auto registered = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "ports": [{"id": "x", "type": "MyType", "direction": "input"}],
        "types": [{"name": "MyType"}]
    })json");
    auto registered_r =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(registered);
    EXPECT_TRUE(registered_r.valid) << registered_r.message;

    auto unregistered = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "ports": [{"id": "x", "type": "MyType", "direction": "input"}]
    })json");
    auto unregistered_r =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(unregistered);
    EXPECT_FALSE(unregistered_r.valid);
    EXPECT_EQ(unregistered_r.error_kind, "invalidValue")
        << unregistered_r.message;
}

TEST(TaskAuthoringContractTest, PortsSectionIdFieldDisciplineRejectsViolations)
{
    // ports[].id 照抄 NodePortNameViolation 字段纪律：非空、≤100 UTF-8
    // 字节、禁 "$" 前缀（AuthoringDocContract.cpp:561-577 lockstep）。
    auto empty_id = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "ports": [{"id": "", "type": "string", "direction": "input"}]
    })json");
    auto empty_r =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(empty_id);
    EXPECT_FALSE(empty_r.valid);
    EXPECT_EQ(empty_r.error_kind, "invalidValue") << empty_r.message;

    const std::string oversized_id(101, 'a');
    auto              oversize = ParseJson(
        "{\"contractVersion\":1,\"viewKind\":\"formSequence\",\"revision\":0,"
        "\"ports\":[{\"id\":\""
        + oversized_id + "\",\"type\":\"string\",\"direction\":\"input\"}]}");
    auto oversize_r =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(oversize);
    EXPECT_FALSE(oversize_r.valid);
    EXPECT_EQ(oversize_r.error_kind, "invalidValue") << oversize_r.message;

    auto dollar_id = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "ports": [{"id": "$x", "type": "string", "direction": "input"}]
    })json");
    auto dollar_r =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(dollar_id);
    EXPECT_FALSE(dollar_r.valid);
    EXPECT_EQ(dollar_r.error_kind, "invalidValue") << dollar_r.message;
}

TEST(TaskAuthoringContractTest, PortsSectionDirectionMustBeInputOrOutput)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "ports": [{"id": "x", "type": "string", "direction": "inout"}]
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "invalidValue") << result.message;
}

TEST(TaskAuthoringContractTest, PortsSectionUnknownKeyRejected)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "ports": [{"id": "x", "type": "string", "direction": "input", "foo": 1}]
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "unknownField") << result.message;
}

TEST(TaskAuthoringContractTest, PortsSectionDuplicateIdRejected)
{
    // id 无重复——重复口 id = 连线坐标歧义。
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "ports": [
            {"id": "x", "type": "string", "direction": "input"},
            {"id": "x", "type": "int", "direction": "output"}
        ]
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "duplicatePortId") << result.message;
}

TEST(
    TaskAuthoringContractTest,
    PortsSectionLabelMustBeNonEmptyStringWhenPresent)
{
    auto empty_label = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "ports": [{"id": "x", "label": "", "type": "string", "direction": "input"}]
    })json");
    auto empty_r =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(empty_label);
    EXPECT_FALSE(empty_r.valid);
    EXPECT_EQ(empty_r.error_kind, "invalidValue") << empty_r.message;

    auto numeric_label = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "ports": [{"id": "x", "label": 5, "type": "string", "direction": "input"}]
    })json");
    auto numeric_r =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(numeric_label);
    EXPECT_FALSE(numeric_r.valid);
    EXPECT_EQ(numeric_r.error_kind, "invalidValue") << numeric_r.message;
}

TEST(TaskAuthoringContractTest, PortsSectionContainerMustBeArray)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "ports": {"id": "x"}
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "invalidPorts") << result.message;
}

TEST(TaskAuthoringContractTest, TypesSectionNameDisciplineAndUniqueness)
{
    // 合法形状：name 必填同款纪律（非空/≤100/禁$），compatibleWith 可缺省。
    auto valid = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "types": [
            {"name": "MyType"},
            {"name": "Other", "compatibleWith": ["int"]}
        ]
    })json");
    auto valid_r = Das::Core::TaskScheduler::ValidateAuthoringDocument(valid);
    EXPECT_TRUE(valid_r.valid) << valid_r.message;

    // name 空：拒（同款字段纪律）。
    auto empty_name = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "types": [{"name": ""}]
    })json");
    auto empty_r =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(empty_name);
    EXPECT_FALSE(empty_r.valid);
    EXPECT_EQ(empty_r.error_kind, "invalidValue") << empty_r.message;

    // name "$" 前缀：拒。
    auto dollar_name = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "types": [{"name": "$MyType"}]
    })json");
    auto dollar_r =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(dollar_name);
    EXPECT_FALSE(dollar_r.valid);
    EXPECT_EQ(dollar_r.error_kind, "invalidValue") << dollar_r.message;

    // 同文档 name 重复：拒（注册名歧义=口面歧义）。
    auto duplicate = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "types": [{"name": "MyType"}, {"name": "MyType"}]
    })json");
    auto duplicate_r =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(duplicate);
    EXPECT_FALSE(duplicate_r.valid);
    EXPECT_EQ(duplicate_r.error_kind, "duplicateTypeName")
        << duplicate_r.message;
}

TEST(
    TaskAuthoringContractTest,
    TypesSectionCompatibleWithScopedToBuiltinsAndSameDocument)
{
    // compatibleWith 每项=九词内建或同文档 types[].name（防跨插件类型注入）。
    auto scoped = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "types": [
            {"name": "MyType"},
            {"name": "Other", "compatibleWith": ["int", "MyType"]}
        ]
    })json");
    auto scoped_r = Das::Core::TaskScheduler::ValidateAuthoringDocument(scoped);
    EXPECT_TRUE(scoped_r.valid) << scoped_r.message;

    auto cross_document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "types": [{"name": "MyType", "compatibleWith": ["OtherPluginType"]}]
    })json");
    auto cross_r =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(cross_document);
    EXPECT_FALSE(cross_r.valid);
    EXPECT_EQ(cross_r.error_kind, "invalidValue") << cross_r.message;

    // 非字符串项：拒。
    auto numeric_item = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "types": [{"name": "MyType", "compatibleWith": [7]}]
    })json");
    auto numeric_r =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(numeric_item);
    EXPECT_FALSE(numeric_r.valid);
    EXPECT_EQ(numeric_r.error_kind, "invalidValue") << numeric_r.message;

    // 非数组 compatibleWith：拒。
    auto non_array = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "types": [{"name": "MyType", "compatibleWith": "int"}]
    })json");
    auto non_array_r =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(non_array);
    EXPECT_FALSE(non_array_r.valid);
    EXPECT_EQ(non_array_r.error_kind, "invalidValue") << non_array_r.message;
}

TEST(TaskAuthoringContractTest, TypesSectionUnknownKeyRejected)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "types": [{"name": "MyType", "foo": 1}]
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "unknownField") << result.message;
}

TEST(TaskAuthoringContractTest, PortsSectionMissingRequiredKeysRejected)
{
    auto no_type = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "ports": [{"id": "x", "direction": "input"}]
    })json");
    auto no_type_r =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(no_type);
    EXPECT_FALSE(no_type_r.valid);
    EXPECT_EQ(no_type_r.error_kind, "missingField") << no_type_r.message;

    auto no_direction = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "ports": [{"id": "x", "type": "string"}]
    })json");
    auto no_direction_r =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(no_direction);
    EXPECT_FALSE(no_direction_r.valid);
    EXPECT_EQ(no_direction_r.error_kind, "missingField")
        << no_direction_r.message;
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

// ===== formSequence chrome 三键（engaged-only：addButtonLabel/
// emptyStateHint/listTitle——出现即非空字符串、未知键拒绝、缺省=UI 内置
// 兜底）=====

TEST(TaskAuthoringContractTest, FormSequenceChromeKeysAbsentPasses)
{
    // formSequence 仅 items：通过（MaaPI 现投影不带三键，逐字节不变——
    // 缺省回落 UI 内置文案的零破坏过渡）。
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 5,
        "formSequence": {"items": []}
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.error_kind.empty()) << result.message;
}

TEST(TaskAuthoringContractTest, FormSequenceChromeThreeKeysPass)
{
    // 三键齐带合法值（示例值=UI 三处硬编码现值，缺省回落目标）。
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "formSequence": {
            "items": [],
            "addButtonLabel": "＋ 添加端口",
            "emptyStateHint": "（空）",
            "listTitle": "端口"
        }
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.error_kind.empty()) << result.message;
}

TEST(TaskAuthoringContractTest, FormSequenceChromeEmptyStringRejected)
{
    const auto make_document = [](std::string_view key)
    {
        return ParseJson(
            std::string{
                R"({"contractVersion":1,"viewKind":"formSequence","revision":0,"formSequence":{"items":[],")"}
            + std::string{key} + "\":\"\"}}");
    };
    for (const std::string_view key :
         {"addButtonLabel", "emptyStateHint", "listTitle"})
    {
        auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(
            make_document(key));
        EXPECT_FALSE(result.valid) << key;
        EXPECT_EQ(result.error_kind, "invalidValue")
            << key << ": " << result.message;
    }
}

TEST(TaskAuthoringContractTest, FormSequenceChromeNonStringRejected)
{
    const auto make_document = [](std::string_view key)
    {
        return ParseJson(
            std::string{
                R"({"contractVersion":1,"viewKind":"formSequence","revision":0,"formSequence":{"items":[],")"}
            + std::string{key} + "\":7}}");
    };
    for (const std::string_view key :
         {"addButtonLabel", "emptyStateHint", "listTitle"})
    {
        auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(
            make_document(key));
        EXPECT_FALSE(result.valid) << key;
        EXPECT_EQ(result.error_kind, "invalidValue")
            << key << ": " << result.message;
    }
}

TEST(TaskAuthoringContractTest, FormSequenceChromeUnknownKeyRejected)
{
    auto document = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "formSequence",
        "revision": 0,
        "formSequence": {"items": [], "foo": "bar"}
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringDocument(document);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "unknownField") << result.message;
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

// targetKind 只认 componentRef：entryRef 目标整体拒绝（fail-closed），
// 附带的 entryRef 键同样不再属于 GraphNode 键集。
TEST(TaskAuthoringContractTest, EntryRefTargetRejected)
{
    auto entry_ref_target = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 0,
        "graph": {"nodes": [{"id": "n", "targetKind": "entryRef", "entryRef": {"entryId": 5}}]}
    })json");
    auto r1 =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(entry_ref_target);
    EXPECT_FALSE(r1.valid);
    EXPECT_EQ(r1.error_kind, "invalidTargetKind");

    auto unknown_target = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 0,
        "graph": {"nodes": [{"id": "n", "targetKind": "somethingElse"}]}
    })json");
    auto r2 =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(unknown_target);
    EXPECT_FALSE(r2.valid);
    EXPECT_EQ(r2.error_kind, "invalidTargetKind");
}

// 节点呈现元数据三键（label 用户/默认显示名、description、icon——自
// actionCatalog 条目合流）进 GraphNode 键集：可选字符串（icon 走与条目
// 相同的绝对路径/http(s) 校验），未知键仍然整体拒绝。
TEST(TaskAuthoringContractTest, GraphNodePresentationKeysAccepted)
{
    auto valid = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 0,
        "graph": {"nodes": [{
            "id": "n-1",
            "label": "我的延迟节点",
            "description": "Delays the signal chain",
            "icon": "https://example.com/delay.png",
            "ports": []
        }]}
    })json");
    auto r1 = Das::Core::TaskScheduler::ValidateAuthoringDocument(valid);
    EXPECT_TRUE(r1.valid) << r1.message;

    auto label_not_string = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 0,
        "graph": {"nodes": [{"id": "n", "label": 5}]}
    })json");
    auto r2 =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(label_not_string);
    EXPECT_FALSE(r2.valid);
    EXPECT_EQ(r2.error_kind, "invalidLabel");

    auto description_not_string = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 0,
        "graph": {"nodes": [{"id": "n", "description": true}]}
    })json");
    auto r3 = Das::Core::TaskScheduler::ValidateAuthoringDocument(
        description_not_string);
    EXPECT_FALSE(r3.valid);
    EXPECT_EQ(r3.error_kind, "invalidDescription");

    auto relative_icon = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 0,
        "graph": {"nodes": [{"id": "n", "icon": "icons/delay.png"}]}
    })json");
    auto r4 =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(relative_icon);
    EXPECT_FALSE(r4.valid);
    EXPECT_EQ(r4.error_kind, "invalidIcon");
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

    auto result = ValidateBatchAuthoringChange(change);

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
        {"renameGraphNode", R"({"nodeId":"n_0","label":"我的延迟节点"})"},
        {"addNodePort",
         R"({"nodeId":"n_0","name":"左值","direction":"input","valueType":"int"})"},
        {"removeNodePort", R"({"nodeId":"n_0","portId":"p_1"})"},
        {"renameNodePort", R"({"nodeId":"n_0","portId":"p_1","name":"新名"})"},
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

        auto result = ValidateBatchAuthoringChange(change);

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

        auto result = ValidateBatchAuthoringChange(change);

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

        auto result = ValidateBatchAuthoringChange(change);

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

    auto r = ValidateBatchAuthoringChange(change);
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
    auto r1 = ValidateBatchAuthoringChange(missing_id);
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
    auto r2 = ValidateBatchAuthoringChange(with_extra);
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

    auto r = ValidateBatchAuthoringChange(change);
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
    auto r_ok = ValidateBatchAuthoringChange(ok);
    EXPECT_TRUE(r_ok.valid) << r_ok.message;

    auto empty = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "invokeCommand",
        "payload": {"commandId": ""}
    })json");
    auto r_empty = ValidateBatchAuthoringChange(empty);
    EXPECT_FALSE(r_empty.valid);
    EXPECT_EQ(r_empty.error_kind, "invalidCommandId");

    auto extra = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "invokeCommand",
        "payload": {"commandId": "x", "args": {}}
    })json");
    auto r_extra = ValidateBatchAuthoringChange(extra);
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
    auto r_ok = ValidateBatchAuthoringChange(ok);
    EXPECT_TRUE(r_ok.valid) << r_ok.message;

    auto bad = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "convertView",
        "payload": {"targetViewKind": "custom"}
    })json");
    auto r_bad = ValidateBatchAuthoringChange(bad);
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
    auto r1 = ValidateBatchAuthoringChange(add_with_index);
    EXPECT_TRUE(r1.valid) << r1.message;

    auto move = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "moveFormSequenceItem",
        "payload": {"formSequenceItemId": "i-1", "destinationIndex": 0}
    })json");
    auto r2 = ValidateBatchAuthoringChange(move);
    EXPECT_TRUE(r2.valid) << r2.message;

    auto remove = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "removeFormSequenceItem",
        "payload": {"formSequenceItemId": "i-1"}
    })json");
    auto r3 = ValidateBatchAuthoringChange(remove);
    EXPECT_TRUE(r3.valid) << r3.message;

    auto add_extra = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "addFormSequenceItem",
        "payload": {"actionId": "A", "itemId": "legacy"}
    })json");
    auto r4 = ValidateBatchAuthoringChange(add_extra);
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
    auto r1 = ValidateBatchAuthoringChange(add);
    EXPECT_TRUE(r1.valid) << r1.message;

    auto remove = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "removeGraphNode",
        "payload": {"nodeId": "n-1"}
    })json");
    auto r2 = ValidateBatchAuthoringChange(remove);
    EXPECT_TRUE(r2.valid) << r2.message;

    auto connect = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "connectGraphPorts",
        "payload": {
            "fromNodeId": "a", "fromPortId": "p1",
            "toNodeId": "b", "toPortId": "p2"
        }
    })json");
    auto r3 = ValidateBatchAuthoringChange(connect);
    EXPECT_TRUE(r3.valid) << r3.message;

    auto disconnect = ParseJson(R"json({
        "baseRevision": 1,
        "kind": "disconnectGraphPorts",
        "payload": {
            "fromNodeId": "a", "fromPortId": "p1",
            "toNodeId": "b", "toPortId": "p2"
        }
    })json");
    auto r4 = ValidateBatchAuthoringChange(disconnect);
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
    auto r_ok = ValidateBatchAuthoringChange(ok);
    EXPECT_TRUE(r_ok.valid) << r_ok.message;

    // 整数格式坐标等价放行（C# 序列化器把 100.0 写成 100）。
    auto integer_coordinates = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNode",
        "payload": {"nodeId": "n1", "position": {"x": 30, "y": -40}}
    })json");
    auto r_integers = ValidateBatchAuthoringChange(integer_coordinates);
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
    auto r_extra = ValidateBatchAuthoringChange(extra_payload_key);
    EXPECT_FALSE(r_extra.valid);
    EXPECT_EQ(r_extra.error_kind, "unknownField");

    auto empty_node_id = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNode",
        "payload": {"nodeId": "", "position": {"x": 1.5, "y": 2.5}}
    })json");
    auto r_empty = ValidateBatchAuthoringChange(empty_node_id);
    EXPECT_FALSE(r_empty.valid);
    EXPECT_EQ(r_empty.error_kind, "invalidNodeId");

    auto missing_node_id = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNode",
        "payload": {"position": {"x": 1.5, "y": 2.5}}
    })json");
    auto r_missing = ValidateBatchAuthoringChange(missing_node_id);
    EXPECT_FALSE(r_missing.valid);
    EXPECT_EQ(r_missing.error_kind, "missingField");

    auto string_coordinate = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNode",
        "payload": {"nodeId": "n1", "position": {"x": "1.5", "y": 2.5}}
    })json");
    auto r_string = ValidateBatchAuthoringChange(string_coordinate);
    EXPECT_FALSE(r_string.valid);
    EXPECT_EQ(r_string.error_kind, "invalidPosition");

    auto missing_coordinate = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNode",
        "payload": {"nodeId": "n1", "position": {"y": 2.5}}
    })json");
    auto r_missing_coordinate =
        ValidateBatchAuthoringChange(missing_coordinate);
    EXPECT_FALSE(r_missing_coordinate.valid);
    EXPECT_EQ(r_missing_coordinate.error_kind, "missingField");

    // 非有限坐标（Infinity 经 AllowInfAndNan 语法进入）必须拒绝。
    auto infinite_coordinate = ParseJsonAllowingNonFinite(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNode",
        "payload": {"nodeId": "n1", "position": {"x": Infinity, "y": 2.5}}
    })json");
    auto r_infinite = ValidateBatchAuthoringChange(infinite_coordinate);
    EXPECT_FALSE(r_infinite.valid);
    EXPECT_EQ(r_infinite.error_kind, "invalidPosition");

    auto extra_position_key = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNode",
        "payload": {"nodeId": "n1", "position": {"x": 1.5, "y": 2.5, "z": 0}}
    })json");
    auto r_extra_position = ValidateBatchAuthoringChange(extra_position_key);
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
    auto r_ok = ValidateBatchAuthoringChange(ok);
    EXPECT_TRUE(r_ok.valid) << r_ok.message;

    // nodeIds 单独出现（不带 connections）合法。
    auto node_ids_only = ParseJson(R"json({
        "baseRevision": 3,
        "kind": "removeGraphNodes",
        "payload": {"nodeIds": ["a"]}
    })json");
    auto r_node_ids_only = ValidateBatchAuthoringChange(node_ids_only);
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
    auto r_connections_only = ValidateBatchAuthoringChange(connections_only);
    EXPECT_TRUE(r_connections_only.valid) << r_connections_only.message;

    // nodeIds 空数组且 connections 缺失 → 至少一项非空不成立。
    auto both_empty = ParseJson(R"json({
        "baseRevision": 3,
        "kind": "removeGraphNodes",
        "payload": {"nodeIds": []}
    })json");
    auto r_both_empty = ValidateBatchAuthoringChange(both_empty);
    EXPECT_FALSE(r_both_empty.valid);
    EXPECT_EQ(r_both_empty.error_kind, "invalidPayload");

    // nodeIds 含空串 → invalidNodeId。
    auto empty_node_id = ParseJson(R"json({
        "baseRevision": 3,
        "kind": "removeGraphNodes",
        "payload": {"nodeIds": ["a", ""]}
    })json");
    auto r_empty_node_id = ValidateBatchAuthoringChange(empty_node_id);
    EXPECT_FALSE(r_empty_node_id.valid);
    EXPECT_EQ(r_empty_node_id.error_kind, "invalidNodeId");

    // nodeIds 含非字符串项 → invalidNodeId。
    auto non_string_node_id = ParseJson(R"json({
        "baseRevision": 3,
        "kind": "removeGraphNodes",
        "payload": {"nodeIds": ["a", 7]}
    })json");
    auto r_non_string = ValidateBatchAuthoringChange(non_string_node_id);
    EXPECT_FALSE(r_non_string.valid);
    EXPECT_EQ(r_non_string.error_kind, "invalidNodeId");

    // nodeIds 重复 → invalidPayload。
    auto duplicate_node_ids = ParseJson(R"json({
        "baseRevision": 3,
        "kind": "removeGraphNodes",
        "payload": {"nodeIds": ["a", "a"]}
    })json");
    auto r_duplicate = ValidateBatchAuthoringChange(duplicate_node_ids);
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
    auto r_missing_key = ValidateBatchAuthoringChange(missing_connection_key);
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
    auto r_empty_value = ValidateBatchAuthoringChange(empty_connection_value);
    EXPECT_FALSE(r_empty_value.valid);
    EXPECT_EQ(r_empty_value.error_kind, "missingField");

    // 未知键 → unknownField。
    auto extra_key = ParseJson(R"json({
        "baseRevision": 3,
        "kind": "removeGraphNodes",
        "payload": {"nodeIds": [], "extra": 1}
    })json");
    auto r_extra_key = ValidateBatchAuthoringChange(extra_key);
    EXPECT_FALSE(r_extra_key.valid);
    EXPECT_EQ(r_extra_key.error_kind, "unknownField");

    // nodeIds 缺失且 connections 为空数组 → 至少一项非空不成立。
    auto missing_node_ids = ParseJson(R"json({
        "baseRevision": 3,
        "kind": "removeGraphNodes",
        "payload": {"connections": []}
    })json");
    auto r_missing_node_ids = ValidateBatchAuthoringChange(missing_node_ids);
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
    auto r_ok = ValidateBatchAuthoringChange(ok);
    EXPECT_TRUE(r_ok.valid) << r_ok.message;

    auto empty_moves = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNodes",
        "payload": {"moves": []}
    })json");
    auto r_empty_moves = ValidateBatchAuthoringChange(empty_moves);
    EXPECT_FALSE(r_empty_moves.valid);
    EXPECT_EQ(r_empty_moves.error_kind, "invalidPayload");

    auto missing_moves = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNodes",
        "payload": {}
    })json");
    auto r_missing_moves = ValidateBatchAuthoringChange(missing_moves);
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
    auto r_duplicate = ValidateBatchAuthoringChange(duplicate_node_id);
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
    auto r_string = ValidateBatchAuthoringChange(string_coordinate);
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
    auto r_infinite = ValidateBatchAuthoringChange(infinite_coordinate);
    EXPECT_FALSE(r_infinite.valid);
    EXPECT_EQ(r_infinite.error_kind, "invalidPosition");

    // moves 单项缺 position → missingField。
    auto missing_position = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNodes",
        "payload": {"moves": [{"nodeId": "a"}]}
    })json");
    auto r_missing_position = ValidateBatchAuthoringChange(missing_position);
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
    auto r_extra_move_key = ValidateBatchAuthoringChange(extra_move_key);
    EXPECT_FALSE(r_extra_move_key.valid);
    EXPECT_EQ(r_extra_move_key.error_kind, "unknownField");

    // 顶层多余键 → unknownField。
    auto extra_top_key = ParseJson(R"json({
        "baseRevision": 5,
        "kind": "moveGraphNodes",
        "payload": {"moves": [{"nodeId": "a", "position": {"x": 1, "y": 2}}],
                    "extra": 1}
    })json");
    auto r_extra_top_key = ValidateBatchAuthoringChange(extra_top_key);
    EXPECT_FALSE(r_extra_top_key.valid);
    EXPECT_EQ(r_extra_top_key.error_kind, "unknownField");
}

// renameGraphNode payload: {nodeId, label}。label 是纯显示数据（用户重命
// 名，语义变更 revision+1）：键集锁死、两键非空、label UTF-8 长度 ≤200。
TEST(TaskAuthoringContractTest, RenameGraphNodeChangePayloadStrict)
{
    const auto make_change = [](std::string_view payload)
    {
        return ParseJson(
            std::string{
                R"({"baseRevision":3,"kind":"renameGraphNode","payload":)"}
            + std::string{payload} + "}");
    };

    auto r_ok = ValidateBatchAuthoringChange(
        make_change(R"({"nodeId":"n_0","label":"我的延迟节点"})"));
    EXPECT_TRUE(r_ok.valid) << r_ok.message;

    // 恰 200 字节的 label 放行（上限含）。
    const std::string label_at_limit(200, 'a');
    auto              r_at_limit = ValidateBatchAuthoringChange(
        make_change(R"({"nodeId":"n_0","label":")" + label_at_limit + R"("})"));
    EXPECT_TRUE(r_at_limit.valid) << r_at_limit.message;

    // 201 字节拒绝（防无界文本入档）。
    const std::string label_over_limit(201, 'a');
    auto              r_over_limit = ValidateBatchAuthoringChange(make_change(
        R"({"nodeId":"n_0","label":")" + label_over_limit + R"("})"));
    EXPECT_FALSE(r_over_limit.valid);
    EXPECT_EQ(r_over_limit.error_kind, "invalidPayload");

    auto r_missing_node_id =
        ValidateBatchAuthoringChange(make_change(R"({"label":"名字"})"));
    EXPECT_FALSE(r_missing_node_id.valid);
    EXPECT_EQ(r_missing_node_id.error_kind, "missingField");

    auto r_empty_node_id = ValidateBatchAuthoringChange(
        make_change(R"({"nodeId":"","label":"名字"})"));
    EXPECT_FALSE(r_empty_node_id.valid);
    EXPECT_EQ(r_empty_node_id.error_kind, "invalidNodeId");

    auto r_non_string_node_id = ValidateBatchAuthoringChange(
        make_change(R"({"nodeId":7,"label":"名字"})"));
    EXPECT_FALSE(r_non_string_node_id.valid);
    EXPECT_EQ(r_non_string_node_id.error_kind, "missingField");

    auto r_missing_label =
        ValidateBatchAuthoringChange(make_change(R"({"nodeId":"n_0"})"));
    EXPECT_FALSE(r_missing_label.valid);
    EXPECT_EQ(r_missing_label.error_kind, "missingField");

    auto r_empty_label = ValidateBatchAuthoringChange(
        make_change(R"({"nodeId":"n_0","label":""})"));
    EXPECT_FALSE(r_empty_label.valid);
    EXPECT_EQ(r_empty_label.error_kind, "invalidPayload");

    auto r_non_string_label = ValidateBatchAuthoringChange(
        make_change(R"({"nodeId":"n_0","label":42})"));
    EXPECT_FALSE(r_non_string_label.valid);
    EXPECT_EQ(r_non_string_label.error_kind, "missingField");

    auto r_extra_key = ValidateBatchAuthoringChange(
        make_change(R"({"nodeId":"n_0","label":"名字","extra":1})"));
    EXPECT_FALSE(r_extra_key.valid);
    EXPECT_EQ(r_extra_key.error_kind, "unknownField");

    // 非对象 payload 在信封层被拒（HasObjectField "payload required"，
    // 与其余 kind 同路径），到不了 per-kind 校验器。
    auto r_non_object =
        ValidateBatchAuthoringChange(make_change(R"(["n_0","名字"])"));
    EXPECT_FALSE(r_non_object.valid);
    EXPECT_EQ(r_non_object.error_kind, "missingField");
}

// addNodePort payload：{nodeId, name, direction, valueType}——键集锁死、
// valueType 九词白名单（bool/int/int64/float/double/string/image/base/
// component）、direction 二值 enum、name 非空且 UTF-8 ≤100 字节。方向
// 语义=签名方向（input=子图入参长在 begin、output=子图出参长在 end），
// 角色门（仅 begin/end 可持动态口）在 Core 分派层。
TEST(TaskAuthoringContractTest, AddNodePortChangePayloadStrict)
{
    const auto make_change = [](std::string_view payload)
    {
        return ParseJson(
            std::string{R"({"baseRevision":3,"kind":"addNodePort","payload":)"}
            + std::string{payload} + "}");
    };

    auto r_ok = ValidateBatchAuthoringChange(make_change(
        R"({"nodeId":"n_0","name":"左值","direction":"input","valueType":"int"})"));
    EXPECT_TRUE(r_ok.valid) << r_ok.message;

    // valueType 白名单外拒绝（含自由串与已退役的 json 词）。
    for (const std::string_view bad_type :
         {"json", "object", "signal", "任意词"})
    {
        auto r_bad_type = ValidateBatchAuthoringChange(make_change(
            std::string{
                R"({"nodeId":"n_0","name":"左值","direction":"input","valueType":")"}
            + std::string{bad_type} + R"("})"));
        EXPECT_FALSE(r_bad_type.valid) << bad_type;
        EXPECT_EQ(r_bad_type.error_kind, "invalidPayload") << bad_type;
    }

    // direction enum 外拒绝。
    auto r_bad_direction = ValidateBatchAuthoringChange(make_change(
        R"({"nodeId":"n_0","name":"左值","direction":"both","valueType":"int"})"));
    EXPECT_FALSE(r_bad_direction.valid);
    EXPECT_EQ(r_bad_direction.error_kind, "invalidPayload");

    // 缺 valueType / 非字符串 direction → missingField。
    auto r_missing_type = ValidateBatchAuthoringChange(
        make_change(R"({"nodeId":"n_0","name":"左值","direction":"input"})"));
    EXPECT_FALSE(r_missing_type.valid);
    EXPECT_EQ(r_missing_type.error_kind, "missingField");

    auto r_non_string_direction = ValidateBatchAuthoringChange(make_change(
        R"({"nodeId":"n_0","name":"左值","direction":7,"valueType":"int"})"));
    EXPECT_FALSE(r_non_string_direction.valid);
    EXPECT_EQ(r_non_string_direction.error_kind, "missingField");

    // name 空 / 101 字节拒绝；恰 100 字节放行。
    auto r_empty_name = ValidateBatchAuthoringChange(make_change(
        R"({"nodeId":"n_0","name":"","direction":"input","valueType":"int"})"));
    EXPECT_FALSE(r_empty_name.valid);
    EXPECT_EQ(r_empty_name.error_kind, "invalidPayload");

    const std::string name_over_limit(101, 'a');
    auto              r_over_limit = ValidateBatchAuthoringChange(make_change(
        std::string{R"({"nodeId":"n_0","name":")"} + name_over_limit
        + R"(","direction":"input","valueType":"int"})"));
    EXPECT_FALSE(r_over_limit.valid);
    EXPECT_EQ(r_over_limit.error_kind, "invalidPayload");

    const std::string name_at_limit(100, 'a');
    auto              r_at_limit = ValidateBatchAuthoringChange(make_change(
        std::string{R"({"nodeId":"n_0","name":")"} + name_at_limit
        + R"(","direction":"input","valueType":"int"})"));
    EXPECT_TRUE(r_at_limit.valid) << r_at_limit.message;

    // nodeId 空/缺失/非字符串。
    auto r_empty_node_id = ValidateBatchAuthoringChange(make_change(
        R"({"nodeId":"","name":"左值","direction":"input","valueType":"int"})"));
    EXPECT_FALSE(r_empty_node_id.valid);
    EXPECT_EQ(r_empty_node_id.error_kind, "invalidNodeId");

    auto r_missing_node_id = ValidateBatchAuthoringChange(make_change(
        R"({"name":"左值","direction":"input","valueType":"int"})"));
    EXPECT_FALSE(r_missing_node_id.valid);
    EXPECT_EQ(r_missing_node_id.error_kind, "missingField");

    auto r_non_string_node_id = ValidateBatchAuthoringChange(make_change(
        R"({"nodeId":7,"name":"左值","direction":"input","valueType":"int"})"));
    EXPECT_FALSE(r_non_string_node_id.valid);
    EXPECT_EQ(r_non_string_node_id.error_kind, "missingField");

    // 键集锁死。
    auto r_extra_key = ValidateBatchAuthoringChange(make_change(
        R"({"nodeId":"n_0","name":"左值","direction":"input","valueType":"int","extra":1})"));
    EXPECT_FALSE(r_extra_key.valid);
    EXPECT_EQ(r_extra_key.error_kind, "unknownField");
}

// removeNodePort payload：{nodeId, portId}——两键非空、键集锁死。
TEST(TaskAuthoringContractTest, RemoveNodePortChangePayloadStrict)
{
    const auto make_change = [](std::string_view payload)
    {
        return ParseJson(
            std::string{
                R"({"baseRevision":0,"kind":"removeNodePort","payload":)"}
            + std::string{payload} + "}");
    };

    auto r_ok = ValidateBatchAuthoringChange(
        make_change(R"({"nodeId":"n_0","portId":"p_1"})"));
    EXPECT_TRUE(r_ok.valid) << r_ok.message;

    auto r_missing_port =
        ValidateBatchAuthoringChange(make_change(R"({"nodeId":"n_0"})"));
    EXPECT_FALSE(r_missing_port.valid);
    EXPECT_EQ(r_missing_port.error_kind, "missingField");

    auto r_empty_port = ValidateBatchAuthoringChange(
        make_change(R"({"nodeId":"n_0","portId":""})"));
    EXPECT_FALSE(r_empty_port.valid);
    EXPECT_EQ(r_empty_port.error_kind, "invalidPayload");

    auto r_extra_key = ValidateBatchAuthoringChange(
        make_change(R"({"nodeId":"n_0","portId":"p_1","name":"多余"})"));
    EXPECT_FALSE(r_extra_key.valid);
    EXPECT_EQ(r_extra_key.error_kind, "unknownField");
}

// renameNodePort payload：{nodeId, portId, name}——name 规则与 addNodePort
// 同源（非空、UTF-8 ≤100 字节；保留前缀的查禁在 Core 分派层）。
TEST(TaskAuthoringContractTest, RenameNodePortChangePayloadStrict)
{
    const auto make_change = [](std::string_view payload)
    {
        return ParseJson(
            std::string{
                R"({"baseRevision":2,"kind":"renameNodePort","payload":)"}
            + std::string{payload} + "}");
    };

    auto r_ok = ValidateBatchAuthoringChange(
        make_change(R"({"nodeId":"n_0","portId":"p_1","name":"新名"})"));
    EXPECT_TRUE(r_ok.valid) << r_ok.message;

    auto r_missing_name = ValidateBatchAuthoringChange(
        make_change(R"({"nodeId":"n_0","portId":"p_1"})"));
    EXPECT_FALSE(r_missing_name.valid);
    EXPECT_EQ(r_missing_name.error_kind, "missingField");

    auto r_empty_name = ValidateBatchAuthoringChange(
        make_change(R"({"nodeId":"n_0","portId":"p_1","name":""})"));
    EXPECT_FALSE(r_empty_name.valid);
    EXPECT_EQ(r_empty_name.error_kind, "invalidPayload");

    const std::string name_over_limit(101, 'a');
    auto              r_over_limit = ValidateBatchAuthoringChange(make_change(
        std::string{R"({"nodeId":"n_0","portId":"p_1","name":")"}
        + name_over_limit + R"("})"));
    EXPECT_FALSE(r_over_limit.valid);
    EXPECT_EQ(r_over_limit.error_kind, "invalidPayload");

    auto r_extra_key = ValidateBatchAuthoringChange(make_change(
        R"({"nodeId":"n_0","portId":"p_1","name":"新名","extra":1})"));
    EXPECT_FALSE(r_extra_key.valid);
    EXPECT_EQ(r_extra_key.error_kind, "unknownField");
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
        "moveGraphNodes",
        "renameGraphNode",
        "addNodePort",
        "removeNodePort",
        "renameNodePort"};
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

// schema GraphNode $def 对账：label/description/icon 三个可选呈现键与
// C++ ValidateGraphNode 键集扩展逐项一致（icon 复用 IconString 引用），
// 防 C++/schema/前端三方漂移。
TEST(TaskAuthoringContractTest, SchemaJsonGraphNodePresentationKeysLocked)
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

    auto node_def = (*defs_object)[std::string_view("GraphNode")].as_object();
    ASSERT_TRUE(node_def.has_value());

    // The presentation keys stay optional: "id" alone remains the required
    // set of a GraphNode.
    auto required_array = (*node_def)[std::string_view("required")].as_array();
    ASSERT_TRUE(required_array.has_value());
    ASSERT_EQ(required_array->size(), 1u);
    EXPECT_EQ(
        (*required_array)[0].as_string().value_or(""),
        std::string_view("id"));

    auto properties = (*node_def)[std::string_view("properties")].as_object();
    ASSERT_TRUE(properties.has_value());
    for (const std::string_view key : {"label", "description"})
    {
        ASSERT_TRUE(properties->contains(key)) << key;
        auto key_definition = (*properties)[key].as_object();
        ASSERT_TRUE(key_definition.has_value()) << key;
        EXPECT_EQ(
            (*key_definition)[std::string_view("type")].as_string().value(),
            "string")
            << key;
    }
    ASSERT_TRUE(properties->contains(std::string_view("icon")));
    auto icon_definition = (*properties)[std::string_view("icon")].as_object();
    ASSERT_TRUE(icon_definition.has_value());
    EXPECT_EQ(
        (*icon_definition)[std::string_view("$ref")].as_string().value(),
        "#/$defs/IconString");
}

// schema RenameGraphNode $def 对账：required {nodeId, label}、键集锁死
// （additionalProperties:false）、两键 minLength 1、label maxLength 200——
// 与 C++ ValidateRenameGraphNodePayload 逐项一致；payload oneOf 含 $ref。
TEST(TaskAuthoringContractTest, SchemaJsonRenameGraphNodeLocked)
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

    auto rename_def =
        (*defs_object)[std::string_view("RenameGraphNode")].as_object();
    ASSERT_TRUE(rename_def.has_value());

    auto no_extra_fields =
        (*rename_def)[std::string_view("additionalProperties")].as_bool();
    ASSERT_TRUE(no_extra_fields.has_value());
    EXPECT_FALSE(*no_extra_fields);

    std::set<std::string> required_fields;
    auto                  required_array =
        (*rename_def)[std::string_view("required")].as_array();
    ASSERT_TRUE(required_array.has_value());
    for (const auto& item : *required_array)
    {
        auto text = item.as_string();
        ASSERT_TRUE(text.has_value());
        required_fields.insert(std::string(*text));
    }
    const std::set<std::string> expected_required{"nodeId", "label"};
    EXPECT_EQ(required_fields, expected_required);

    auto properties = (*rename_def)[std::string_view("properties")].as_object();
    ASSERT_TRUE(properties.has_value());
    auto node_id = (*properties)[std::string_view("nodeId")].as_object();
    ASSERT_TRUE(node_id.has_value());
    EXPECT_EQ(
        (*node_id)[std::string_view("type")].as_string().value(),
        "string");
    EXPECT_EQ((*node_id)[std::string_view("minLength")].as_sint().value(), 1);

    auto label = (*properties)[std::string_view("label")].as_object();
    ASSERT_TRUE(label.has_value());
    EXPECT_EQ((*label)[std::string_view("type")].as_string().value(), "string");
    EXPECT_EQ((*label)[std::string_view("minLength")].as_sint().value(), 1);
    EXPECT_EQ((*label)[std::string_view("maxLength")].as_sint().value(), 200);

    // AuthoringChangePayload 的 oneOf 分支集含 RenameGraphNode $ref。
    auto payload_definition =
        (*defs_object)[std::string_view("AuthoringChangePayload")].as_object();
    ASSERT_TRUE(payload_definition.has_value());
    auto payload_branches =
        (*payload_definition)[std::string_view("oneOf")].as_array();
    ASSERT_TRUE(payload_branches.has_value());
    bool found_rename_branch = false;
    for (const auto& branch : *payload_branches)
    {
        auto branch_obj = branch.as_object();
        if (!branch_obj)
        {
            continue;
        }
        if (branch_obj->contains(std::string_view("$ref")))
        {
            auto ref = (*branch_obj)[std::string_view("$ref")].as_string();
            if (ref == std::string_view("#/$defs/RenameGraphNode"))
            {
                found_rename_branch = true;
                break;
            }
        }
    }
    EXPECT_TRUE(found_rename_branch);
}

// schema 三口 payload def + GraphNodePort.dynamic 对账：AddNodePort required
// 四键 + valueType 九词 enum + name maxLength 100；RemoveNodePort/
// RenameNodePort 键集锁死；GraphNodePort.dynamic 可选布尔；payload oneOf
// 含三 $ref——与 C++ 校验器逐项一致（法则8 lockstep）。
TEST(TaskAuthoringContractTest, SchemaJsonNodePortDefsLocked)
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

    const auto read_required_keys =
        [&defs_object](
            std::string_view       definition_name,
            std::set<std::string>& required_keys_out)
    {
        auto definition =
            (*defs_object)[std::string_view(definition_name)].as_object();
        if (!definition.has_value())
        {
            return false;
        }
        required_keys_out.clear();
        auto required_array =
            (*definition)[std::string_view("required")].as_array();
        if (!required_array.has_value())
        {
            return false;
        }
        for (const auto& item : *required_array)
        {
            auto text = item.as_string();
            if (!text.has_value())
            {
                return false;
            }
            required_keys_out.insert(std::string(*text));
        }
        auto additional =
            (*definition)[std::string_view("additionalProperties")].as_bool();
        if (!additional.has_value() || *additional)
        {
            return false;
        }
        return true;
    };

    // AddNodePort：required 恰四键、additionalProperties:false。
    std::set<std::string> add_required;
    ASSERT_TRUE(read_required_keys("AddNodePort", add_required));
    EXPECT_EQ(
        add_required,
        (std::set<std::string>{"nodeId", "name", "direction", "valueType"}));

    auto add_definition =
        (*defs_object)[std::string_view("AddNodePort")].as_object();
    ASSERT_TRUE(add_definition.has_value());
    auto add_properties =
        (*add_definition)[std::string_view("properties")].as_object();
    ASSERT_TRUE(add_properties.has_value());

    auto value_type_definition =
        (*add_properties)[std::string_view("valueType")].as_object();
    ASSERT_TRUE(value_type_definition.has_value());
    auto value_type_enum =
        (*value_type_definition)[std::string_view("enum")].as_array();
    ASSERT_TRUE(value_type_enum.has_value());
    std::set<std::string> value_type_words;
    for (const auto& item : *value_type_enum)
    {
        auto text = item.as_string();
        ASSERT_TRUE(text.has_value());
        value_type_words.insert(std::string(*text));
    }
    EXPECT_EQ(
        value_type_words,
        (std::set<std::string>{
            "bool",
            "int",
            "int64",
            "float",
            "double",
            "string",
            "image",
            "base",
            "component"}));

    auto direction_definition =
        (*add_properties)[std::string_view("direction")].as_object();
    ASSERT_TRUE(direction_definition.has_value());
    auto direction_enum =
        (*direction_definition)[std::string_view("enum")].as_array();
    ASSERT_TRUE(direction_enum.has_value());
    EXPECT_EQ(direction_enum->size(), 2u);

    auto name_definition =
        (*add_properties)[std::string_view("name")].as_object();
    ASSERT_TRUE(name_definition.has_value());
    EXPECT_EQ(
        (*name_definition)[std::string_view("maxLength")].as_sint().value(),
        100);
    EXPECT_EQ(
        (*name_definition)[std::string_view("minLength")].as_sint().value(),
        1);

    // RemoveNodePort / RenameNodePort：键集锁死。
    std::set<std::string> remove_required;
    ASSERT_TRUE(read_required_keys("RemoveNodePort", remove_required));
    EXPECT_EQ(remove_required, (std::set<std::string>{"nodeId", "portId"}));

    std::set<std::string> rename_required;
    ASSERT_TRUE(read_required_keys("RenameNodePort", rename_required));
    EXPECT_EQ(
        rename_required,
        (std::set<std::string>{"nodeId", "portId", "name"}));

    // GraphNodePort.dynamic：可选布尔键（engaged-only，不在 required）。
    auto port_definition =
        (*defs_object)[std::string_view("GraphNodePort")].as_object();
    ASSERT_TRUE(port_definition.has_value());
    auto port_properties =
        (*port_definition)[std::string_view("properties")].as_object();
    ASSERT_TRUE(port_properties.has_value());
    ASSERT_TRUE(port_properties->contains(std::string_view("dynamic")));
    auto dynamic_definition =
        (*port_properties)[std::string_view("dynamic")].as_object();
    ASSERT_TRUE(dynamic_definition.has_value());
    EXPECT_EQ(
        (*dynamic_definition)[std::string_view("type")].as_string().value(),
        "boolean");

    // payload oneOf 分支集含三个 $ref。
    auto payload_definition =
        (*defs_object)[std::string_view("AuthoringChangePayload")].as_object();
    ASSERT_TRUE(payload_definition.has_value());
    auto payload_branches =
        (*payload_definition)[std::string_view("oneOf")].as_array();
    ASSERT_TRUE(payload_branches.has_value());
    std::set<std::string> wanted_refs{
        "#/$defs/AddNodePort",
        "#/$defs/RemoveNodePort",
        "#/$defs/RenameNodePort"};
    for (const auto& branch : *payload_branches)
    {
        auto branch_obj = branch.as_object();
        if (!branch_obj || !branch_obj->contains(std::string_view("$ref")))
        {
            continue;
        }
        auto ref = (*branch_obj)[std::string_view("$ref")].as_string();
        if (ref.has_value())
        {
            wanted_refs.erase(std::string(*ref));
        }
    }
    EXPECT_TRUE(wanted_refs.empty());
}

// schema 根文档 ports/types section 对账（Core/schema 两层 lockstep）：
// AuthoringDocument.properties 增两键；PortDeclaration/TypeDeclaration
// 形状与 C++ ValidatePortsSection/ValidateTypesSection 逐字段一致——
// required 集、additionalProperties:false、id/name 的 maxLength 100 + 禁$
// pattern、type 词汇表封闭说明。
TEST(TaskAuthoringContractTest, SchemaJsonPortsTypesSectionsLocked)
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

    // AuthoringDocument.properties 含 ports/types 两键（数组引用）。
    auto document_def =
        (*defs_object)[std::string_view("AuthoringDocument")].as_object();
    ASSERT_TRUE(document_def.has_value());
    auto document_properties =
        (*document_def)[std::string_view("properties")].as_object();
    ASSERT_TRUE(document_properties.has_value());
    for (const std::string_view section : {"ports", "types"})
    {
        ASSERT_TRUE(document_properties->contains(std::string_view(section)))
            << section;
        auto section_definition =
            (*document_properties)[std::string_view(section)].as_object();
        ASSERT_TRUE(section_definition.has_value()) << section;
        EXPECT_EQ(
            (*section_definition)[std::string_view("type")].as_string().value(),
            "array")
            << section;
        auto items_definition =
            (*section_definition)[std::string_view("items")].as_object();
        ASSERT_TRUE(items_definition.has_value()) << section;
        EXPECT_EQ(
            (*items_definition)[std::string_view("$ref")].as_string().value(),
            section == std::string_view("ports") ? "#/$defs/PortDeclaration"
                                                 : "#/$defs/TypeDeclaration")
            << section;
    }

    // PortDeclaration：required {id,type,direction}，label 可选。
    auto port_def =
        (*defs_object)[std::string_view("PortDeclaration")].as_object();
    ASSERT_TRUE(port_def.has_value());
    auto port_no_extra =
        (*port_def)[std::string_view("additionalProperties")].as_bool();
    ASSERT_TRUE(port_no_extra.has_value());
    EXPECT_FALSE(*port_no_extra);
    std::set<std::string> port_required;
    auto                  port_required_array =
        (*port_def)[std::string_view("required")].as_array();
    ASSERT_TRUE(port_required_array.has_value());
    for (const auto& item : *port_required_array)
    {
        port_required.insert(std::string(item.as_string().value()));
    }
    EXPECT_EQ(
        port_required,
        (std::set<std::string>{"id", "type", "direction"}));

    auto port_properties =
        (*port_def)[std::string_view("properties")].as_object();
    ASSERT_TRUE(port_properties.has_value());
    auto id_definition = (*port_properties)[std::string_view("id")].as_object();
    ASSERT_TRUE(id_definition.has_value());
    EXPECT_EQ(
        (*id_definition)[std::string_view("type")].as_string().value(),
        "string");
    EXPECT_EQ(
        (*id_definition)[std::string_view("minLength")].as_sint().value(),
        1);
    EXPECT_EQ(
        (*id_definition)[std::string_view("maxLength")].as_sint().value(),
        100);
    EXPECT_EQ(
        (*id_definition)[std::string_view("pattern")].as_string().value(),
        "^[^$].*$");

    auto label_definition =
        (*port_properties)[std::string_view("label")].as_object();
    ASSERT_TRUE(label_definition.has_value());
    EXPECT_EQ(
        (*label_definition)[std::string_view("type")].as_string().value(),
        "string");
    EXPECT_EQ(
        (*label_definition)[std::string_view("minLength")].as_sint().value(),
        1);

    auto port_type_definition =
        (*port_properties)[std::string_view("type")].as_object();
    ASSERT_TRUE(port_type_definition.has_value());
    EXPECT_EQ(
        (*port_type_definition)[std::string_view("type")].as_string().value(),
        "string");
    EXPECT_EQ(
        (*port_type_definition)[std::string_view("minLength")]
            .as_sint()
            .value(),
        1);

    auto direction_definition =
        (*port_properties)[std::string_view("direction")].as_object();
    ASSERT_TRUE(direction_definition.has_value());
    auto direction_enum =
        (*direction_definition)[std::string_view("enum")].as_array();
    ASSERT_TRUE(direction_enum.has_value());
    std::set<std::string> direction_values;
    for (const auto& item : *direction_enum)
    {
        direction_values.insert(std::string(item.as_string().value()));
    }
    EXPECT_EQ(direction_values, (std::set<std::string>{"input", "output"}));

    // TypeDeclaration：required {name}，compatibleWith 可选数组。
    auto type_def =
        (*defs_object)[std::string_view("TypeDeclaration")].as_object();
    ASSERT_TRUE(type_def.has_value());
    auto type_no_extra =
        (*type_def)[std::string_view("additionalProperties")].as_bool();
    ASSERT_TRUE(type_no_extra.has_value());
    EXPECT_FALSE(*type_no_extra);
    std::set<std::string> type_required;
    auto                  type_required_array =
        (*type_def)[std::string_view("required")].as_array();
    ASSERT_TRUE(type_required_array.has_value());
    for (const auto& item : *type_required_array)
    {
        type_required.insert(std::string(item.as_string().value()));
    }
    EXPECT_EQ(type_required, (std::set<std::string>{"name"}));

    auto type_properties =
        (*type_def)[std::string_view("properties")].as_object();
    ASSERT_TRUE(type_properties.has_value());
    auto name_definition =
        (*type_properties)[std::string_view("name")].as_object();
    ASSERT_TRUE(name_definition.has_value());
    EXPECT_EQ(
        (*name_definition)[std::string_view("type")].as_string().value(),
        "string");
    EXPECT_EQ(
        (*name_definition)[std::string_view("minLength")].as_sint().value(),
        1);
    EXPECT_EQ(
        (*name_definition)[std::string_view("maxLength")].as_sint().value(),
        100);
    EXPECT_EQ(
        (*name_definition)[std::string_view("pattern")].as_string().value(),
        "^[^$].*$");

    auto compatible_definition =
        (*type_properties)[std::string_view("compatibleWith")].as_object();
    ASSERT_TRUE(compatible_definition.has_value());
    EXPECT_EQ(
        (*compatible_definition)[std::string_view("type")].as_string().value(),
        "array");
    auto compatible_items =
        (*compatible_definition)[std::string_view("items")].as_object();
    ASSERT_TRUE(compatible_items.has_value());
    EXPECT_EQ(
        (*compatible_items)[std::string_view("type")].as_string().value(),
        "string");
    EXPECT_EQ(
        (*compatible_items)[std::string_view("minLength")].as_sint().value(),
        1);
}

// schema formSequence chrome 三键对账（Core/schema 两层 lockstep）：
// addButtonLabel/emptyStateHint/listTitle 三键进 properties（string、
// minLength 1）且均不在 required 集（engaged-only）；required 仍只有
// items；additionalProperties 保持 false（未知键拒绝）。
TEST(TaskAuthoringContractTest, SchemaJsonFormSequenceChromeKeysLocked)
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

    auto form_def =
        (*defs_object)[std::string_view("FormSequence")].as_object();
    ASSERT_TRUE(form_def.has_value());

    auto no_extra_fields =
        (*form_def)[std::string_view("additionalProperties")].as_bool();
    ASSERT_TRUE(no_extra_fields.has_value());
    EXPECT_FALSE(*no_extra_fields);

    std::set<std::string> required_fields;
    auto required_array = (*form_def)[std::string_view("required")].as_array();
    ASSERT_TRUE(required_array.has_value());
    for (const auto& item : *required_array)
    {
        required_fields.insert(std::string(item.as_string().value()));
    }
    EXPECT_EQ(required_fields, (std::set<std::string>{"items"}));

    auto properties = (*form_def)[std::string_view("properties")].as_object();
    ASSERT_TRUE(properties.has_value());
    for (const std::string_view chrome :
         {"addButtonLabel", "emptyStateHint", "listTitle"})
    {
        ASSERT_TRUE(properties->contains(std::string_view(chrome))) << chrome;
        auto chrome_definition =
            (*properties)[std::string_view(chrome)].as_object();
        ASSERT_TRUE(chrome_definition.has_value()) << chrome;
        EXPECT_EQ(
            (*chrome_definition)[std::string_view("type")].as_string().value(),
            "string")
            << chrome;
        EXPECT_EQ(
            (*chrome_definition)[std::string_view("minLength")]
                .as_sint()
                .value(),
            1)
            << chrome;
        // engaged-only：三键均非 required（缺省=UI 内置兜底）。
        EXPECT_FALSE(required_fields.count(std::string(chrome))) << chrome;
    }
}

// schema AuthoringGetRequest $def 对账：componentContext/componentGuid 两
// 可选请求键与 Apply 侧同批同步（法则8 lockstep）——UI 实发（BuildGetBody
// 携 {componentGuid, componentContext:true}）、服务端实消费（六路由读取
// 两键），schema 不得禁止其实发形状。additionalProperties:false 保持（白
// 名单闭集），两键非 required（缺省=宿主文档自身的 Get，逐字节现状）。
TEST(
    TaskAuthoringContractTest,
    SchemaJsonAuthoringGetRequestComponentKeysLocked)
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

    auto get_def =
        (*defs_object)[std::string_view("AuthoringGetRequest")].as_object();
    ASSERT_TRUE(get_def.has_value());

    auto no_extra_fields =
        (*get_def)[std::string_view("additionalProperties")].as_bool();
    ASSERT_TRUE(no_extra_fields.has_value());
    EXPECT_FALSE(*no_extra_fields);

    // 两键可选（非 required）：required 缺席或不含它们。
    auto required_array = (*get_def)[std::string_view("required")].as_array();
    if (required_array.has_value())
    {
        for (const auto& item : *required_array)
        {
            auto text = item.as_string();
            ASSERT_TRUE(text.has_value());
            EXPECT_NE(*text, std::string_view("componentContext"));
            EXPECT_NE(*text, std::string_view("componentGuid"));
        }
    }

    auto properties = (*get_def)[std::string_view("properties")].as_object();
    ASSERT_TRUE(properties.has_value());
    ASSERT_TRUE(properties->contains(std::string_view("componentContext")));
    {
        auto definition =
            (*properties)[std::string_view("componentContext")].as_object();
        ASSERT_TRUE(definition.has_value());
        EXPECT_EQ(
            (*definition)[std::string_view("type")].as_string().value(),
            "boolean");
    }
    ASSERT_TRUE(properties->contains(std::string_view("componentGuid")));
    {
        auto definition =
            (*properties)[std::string_view("componentGuid")].as_object();
        ASSERT_TRUE(definition.has_value());
        EXPECT_EQ(
            (*definition)[std::string_view("type")].as_string().value(),
            "string");
    }
}

// GraphNodePort 可选 dynamic 键：布尔真值放行（engaged-only——呈现层只在
// 动态口上出该键）、非布尔值拒绝。
TEST(TaskAuthoringContractTest, GraphNodePortDynamicKeyValidation)
{
    const auto make_document = [](std::string_view port_entry)
    {
        return ParseJson(
            std::string{
                R"({"contractVersion":1,"viewKind":"graph","revision":0,"graph":{"nodes":[{"id":"n","ports":[)"}
            + std::string{port_entry} + R"(]}]}})");
    };

    auto with_dynamic = make_document(
        R"({"portId":"p_1","displayLabel":"左值","portType":"int","direction":"output","category":"data","dynamic":true})");
    auto r_dynamic =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(with_dynamic);
    EXPECT_TRUE(r_dynamic.valid) << r_dynamic.message;

    auto with_bad_dynamic = make_document(
        R"({"portId":"p_1","displayLabel":"左值","portType":"int","direction":"output","category":"data","dynamic":"yes"})");
    auto r_bad_dynamic =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(with_bad_dynamic);
    EXPECT_FALSE(r_bad_dynamic.valid);
    EXPECT_EQ(r_bad_dynamic.error_kind, "invalidPortDynamic");
}

// inputType 词表保持 0-8 官方词表：9（曾短暂用于仓库条目选择器）与 10
// 一并拒绝——taskRepositoryRef 的分派走字段 config 类型词（"type" 键），
// 不在 inputType 上扩词。
TEST(TaskAuthoringContractTest, RepositoryRefInputTypeNineRejected)
{
    auto unsupported = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 0,
        "graph": {"nodes": [{"id": "n", "fields": [
            {
                "name": "repositoryRef",
                "label": "repositoryRef",
                "valueType": 16,
                "inputType": 9,
                "value": null
            }
        ]}]}
    })json");
    auto r1 = Das::Core::TaskScheduler::ValidateAuthoringDocument(unsupported);
    EXPECT_FALSE(r1.valid);
    EXPECT_EQ(r1.error_kind, "invalidInputType");

    auto ten = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 0,
        "graph": {"nodes": [{"id": "n", "fields": [
            {
                "name": "repositoryRef",
                "label": "repositoryRef",
                "valueType": 16,
                "inputType": 10,
                "value": null
            }
        ]}]}
    })json");
    auto r2 = Das::Core::TaskScheduler::ValidateAuthoringDocument(ten);
    EXPECT_FALSE(r2.valid);
    EXPECT_EQ(r2.error_kind, "invalidInputType");
}

// 字段 config 类型词（可选 "type" 字符串键）：合法字符串接受；非字符串
// 拒绝；缺省照常接受（未声明 config 的字段不携带该键）。
TEST(TaskAuthoringContractTest, FieldTypeWordOptionalStringKey)
{
    auto with_type = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 0,
        "graph": {"nodes": [{"id": "n", "fields": [
            {
                "name": "repositoryRef",
                "label": "repositoryRef",
                "valueType": 16,
                "inputType": 0,
                "type": "taskRepositoryRef",
                "value": 7
            }
        ]}]}
    })json");
    auto r1 = Das::Core::TaskScheduler::ValidateAuthoringDocument(with_type);
    EXPECT_TRUE(r1.valid) << r1.message;

    auto bad_type = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 0,
        "graph": {"nodes": [{"id": "n", "fields": [
            {
                "name": "repositoryRef",
                "label": "repositoryRef",
                "valueType": 16,
                "inputType": 0,
                "type": 7,
                "value": 7
            }
        ]}]}
    })json");
    auto r2 = Das::Core::TaskScheduler::ValidateAuthoringDocument(bad_type);
    EXPECT_FALSE(r2.valid);
    EXPECT_EQ(r2.error_kind, "invalidFieldType");
}

// 字段可选 taskGuid 键（声明的目标任务类型）：合法字符串接受；非字符串
// 拒绝；缺省照常接受（声明未命名任务类型的字段不携带该键）。
TEST(TaskAuthoringContractTest, FieldTaskGuidOptionalStringKey)
{
    auto with_task_guid = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 0,
        "graph": {"nodes": [{"id": "n", "fields": [
            {
                "name": "repositoryRef",
                "label": "repositoryRef",
                "valueType": 16,
                "inputType": 0,
                "type": "taskRepositoryRef",
                "taskGuid": "69F20001-0000-4000-8000-000000000001",
                "value": 7
            }
        ]}]}
    })json");
    auto r1 =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(with_task_guid);
    EXPECT_TRUE(r1.valid) << r1.message;

    auto bad_task_guid = ParseJson(R"json({
        "contractVersion": 1,
        "viewKind": "graph",
        "revision": 0,
        "graph": {"nodes": [{"id": "n", "fields": [
            {
                "name": "repositoryRef",
                "label": "repositoryRef",
                "valueType": 16,
                "inputType": 0,
                "type": "taskRepositoryRef",
                "taskGuid": 7,
                "value": 7
            }
        ]}]}
    })json");
    auto r2 =
        Das::Core::TaskScheduler::ValidateAuthoringDocument(bad_task_guid);
    EXPECT_FALSE(r2.valid);
    EXPECT_EQ(r2.error_kind, "invalidFieldTaskGuid");
}

// schema FieldDescriptor.inputType enum 不含 9（与 C++ INPUT_TYPES 对账），
// 且 "type"（config 类型词）属性在 schema 内声明为字符串。
TEST(TaskAuthoringContractTest, SchemaJsonInputTypeVocabularyAndTypeWordLocked)
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

    auto field_def =
        (*defs_object)[std::string_view("FieldDescriptor")].as_object();
    ASSERT_TRUE(field_def.has_value());
    auto properties = (*field_def)[std::string_view("properties")].as_object();
    ASSERT_TRUE(properties.has_value());
    ASSERT_TRUE(properties->contains(std::string_view("inputType")));
    auto input_type_definition =
        (*properties)[std::string_view("inputType")].as_object();
    ASSERT_TRUE(input_type_definition.has_value());
    auto input_type_enum =
        (*input_type_definition)[std::string_view("enum")].as_array();
    ASSERT_TRUE(input_type_enum.has_value());
    std::set<int64_t> input_type_values;
    for (const auto& item : *input_type_enum)
    {
        auto value = item.as_sint();
        ASSERT_TRUE(value.has_value());
        input_type_values.insert(*value);
    }
    EXPECT_FALSE(input_type_values.contains(9));
    EXPECT_FALSE(input_type_values.contains(10));
    for (int64_t legal : {0, 1, 2, 3, 4, 5, 6, 7, 8})
    {
        EXPECT_TRUE(input_type_values.contains(legal)) << legal;
    }

    ASSERT_TRUE(properties->contains(std::string_view("type")));
    auto type_word_definition =
        (*properties)[std::string_view("type")].as_object();
    ASSERT_TRUE(type_word_definition.has_value());
    EXPECT_TRUE(type_word_definition->contains(std::string_view("type")));
    auto type_word_json_type =
        (*type_word_definition)[std::string_view("type")].as_string();
    ASSERT_TRUE(type_word_json_type.has_value());
    EXPECT_EQ(*type_word_json_type, std::string_view("string"));

    // taskGuid（声明的目标任务类型）与 C++ validator 同步声明为字符串。
    ASSERT_TRUE(properties->contains(std::string_view("taskGuid")));
    auto task_guid_definition =
        (*properties)[std::string_view("taskGuid")].as_object();
    ASSERT_TRUE(task_guid_definition.has_value());
    EXPECT_TRUE(task_guid_definition->contains(std::string_view("type")));
    auto task_guid_json_type =
        (*task_guid_definition)[std::string_view("type")].as_string();
    ASSERT_TRUE(task_guid_json_type.has_value());
    EXPECT_EQ(*task_guid_json_type, std::string_view("string"));
}

TEST(TaskAuthoringContractTest, ChangeRejectsOpWrapper)
{
    auto change = ParseJson(R"json({
        "baseRevision": 1,
        "change": {"op": "setValue", "path": "x"}
    })json");

    auto r = ValidateBatchAuthoringChange(change);
    EXPECT_FALSE(r.valid);
    EXPECT_EQ(r.error_kind, "missingField");
}

// =============================================================================
// 批量 apply 请求 wire：{baseRevision, changes:[{kind,payload},...]}。
// 单 change 顶层形态（{baseRevision,kind,payload}）退役——项目未发布无
// 兼容包袱，全量迁移 fail-closed。
// =============================================================================

TEST(TaskAuthoringContractTest, ApplyRequestAcceptsBatchChanges)
{
    auto multi = ParseJson(R"json({
        "baseRevision": 4,
        "changes": [
            {"kind": "setValue", "payload": {
                "target": {"kind": "document"},
                "fieldName": "x",
                "value": null}},
            {"kind": "removeGraphNodes", "payload": {
                "nodeIds": ["n-1", "n-2"]}}
        ]
    })json");
    auto multi_result =
        Das::Core::TaskScheduler::ValidateAuthoringChange(multi);
    EXPECT_TRUE(multi_result.valid) << multi_result.message;

    auto single = ParseJson(R"json({
        "baseRevision": 4,
        "changes": [
            {"kind": "invokeCommand", "payload": {"commandId": "do-thing"}}
        ]
    })json");
    auto single_result =
        Das::Core::TaskScheduler::ValidateAuthoringChange(single);
    EXPECT_TRUE(single_result.valid) << single_result.message;
}

TEST(TaskAuthoringContractTest, ApplyRequestRejectsLegacySingleChangeShape)
{
    auto legacy = ParseJson(R"json({
        "baseRevision": 4,
        "kind": "setValue",
        "payload": {
            "target": {"kind": "document"},
            "fieldName": "x",
            "value": null
        }
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringChange(legacy);
    EXPECT_FALSE(result.valid);
}

TEST(TaskAuthoringContractTest, ApplyRequestRejectsMalformedChangesArray)
{
    auto missing = ParseJson(R"json({"baseRevision": 4})json");
    auto missing_result =
        Das::Core::TaskScheduler::ValidateAuthoringChange(missing);
    EXPECT_FALSE(missing_result.valid);
    EXPECT_EQ(missing_result.error_kind, "missingField");

    auto not_array = ParseJson(
        R"json({"baseRevision": 4, "changes": {"kind": "setValue"}})json");
    auto not_array_result =
        Das::Core::TaskScheduler::ValidateAuthoringChange(not_array);
    EXPECT_FALSE(not_array_result.valid);

    auto empty = ParseJson(R"json({"baseRevision": 4, "changes": []})json");
    auto empty_result =
        Das::Core::TaskScheduler::ValidateAuthoringChange(empty);
    EXPECT_FALSE(empty_result.valid);
    EXPECT_EQ(empty_result.error_kind, "invalidChanges");
}

TEST(TaskAuthoringContractTest, ApplyRequestRejectsTooManyChanges)
{
    auto request = ParseJson(R"json({"baseRevision": 0, "changes": []})json");
    auto changes_array = request.as_object();
    (*changes_array)[std::string_view("changes")] = []()
    {
        auto          changes = Das::Utils::MakeYyjsonArray();
        auto          array_view = changes.as_array();
        yyjson::value element(Das::Utils::MakeYyjsonObject());
        auto          element_object = element.as_object();
        (*element_object)[std::string_view("kind")] = std::make_pair(
            std::string_view("moveGraphNode"),
            yyjson::copy_string);
        yyjson::value payload(Das::Utils::MakeYyjsonObject());
        (*payload.as_object())[std::string_view("nodeId")] =
            std::make_pair(std::string_view("n_1"), yyjson::copy_string);
        yyjson::value position(Das::Utils::MakeYyjsonObject());
        (*position.as_object())[std::string_view("x")] = 1.0;
        (*position.as_object())[std::string_view("y")] = 2.0;
        (*payload.as_object())[std::string_view("position")] =
            std::move(position);
        (*element_object)[std::string_view("payload")] = std::move(payload);
        const std::size_t total =
            Das::Core::TaskScheduler::kMaxAuthoringApplyChanges + 1;
        for (std::size_t index = 0; index < total; ++index)
        {
            array_view->emplace_back(Das::Utils::CloneYyjsonValue(element));
        }
        return changes;
    }();

    auto result = Das::Core::TaskScheduler::ValidateAuthoringChange(request);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "tooManyChanges");
}

TEST(TaskAuthoringContractTest, ApplyRequestRejectsUnknownKeyInChangeElement)
{
    auto with_extra = ParseJson(R"json({
        "baseRevision": 4,
        "changes": [
            {"kind": "setValue", "payload": {
                "target": {"kind": "document"},
                "fieldName": "x",
                "value": null},
             "baseRevision": 0}
        ]
    })json");

    auto result = Das::Core::TaskScheduler::ValidateAuthoringChange(with_extra);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "unknownField");
}

TEST(TaskAuthoringContractTest, ApplyRequestRejectsNonObjectChangeElement)
{
    auto with_scalar =
        ParseJson(R"json({"baseRevision": 4, "changes": ["setValue"]})json");

    auto result =
        Das::Core::TaskScheduler::ValidateAuthoringChange(with_scalar);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error_kind, "invalidChange");
}

// 批量 revision 不变量：成功域为 [base + (存在 bump change ? 1 : 0),
// base + bump change 数]（每个语义 change 恰 +1，viewData 轻量通道 0）；
// 失败 envelope 恒 == base。
TEST(TaskAuthoringContractTest, BatchApplyResultAcceptsMultiBumpDomain)
{
    const auto make_result = [](int64_t revision)
    {
        return ParseJson(
            std::string{R"json({
            "acceptedProperties": {},
            "document": {
                "contractVersion": 1,
                "viewKind": "graph",
                "revision": )json"}
            + std::to_string(revision) + R"json(
            },
            "diagnostics": []
        })json");
    };

    // 双 bump 批（setValue + addGraphNode）：base+2 合法，base+1 落在域内
    // （对应混合批的单 bump 场景），base+3 超出 base+len 不合法。
    const std::vector<std::string> two_bumping_kinds{
        "setValue",
        "addGraphNode"};
    auto at_two_bumps = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(
        make_result(6),
        4,
        two_bumping_kinds);
    EXPECT_TRUE(at_two_bumps.valid) << at_two_bumps.message;

    const std::vector<std::string> one_bumping_kinds{
        "setValue",
        "moveGraphNodes"};
    auto at_one_bump = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(
        make_result(5),
        4,
        one_bumping_kinds);
    EXPECT_TRUE(at_one_bump.valid) << at_one_bump.message;

    auto beyond_budget = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(
        make_result(7),
        4,
        two_bumping_kinds);
    EXPECT_FALSE(beyond_budget.valid);
    EXPECT_EQ(beyond_budget.error_kind, "revisionMismatch");
}

TEST(TaskAuthoringContractTest, BatchApplyResultPureViewDataBatchKeepsBase)
{
    const std::vector<std::string> view_data_kinds{
        "moveGraphNode",
        "moveGraphNodes"};
    auto at_base = ParseJson(R"json({
        "acceptedProperties": {},
        "document": {
            "contractVersion": 1,
            "viewKind": "graph",
            "revision": 4
        },
        "diagnostics": []
    })json");
    auto base_result = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(
        at_base,
        4,
        view_data_kinds);
    EXPECT_TRUE(base_result.valid) << base_result.message;

    auto bumped = ParseJson(R"json({
        "acceptedProperties": {},
        "document": {
            "contractVersion": 1,
            "viewKind": "graph",
            "revision": 5
        },
        "diagnostics": []
    })json");
    auto bumped_result = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(
        bumped,
        4,
        view_data_kinds);
    EXPECT_FALSE(bumped_result.valid);
    EXPECT_EQ(bumped_result.error_kind, "revisionMismatch");
}

TEST(TaskAuthoringContractTest, BatchApplyResultFailureKeepsBaseRevision)
{
    const std::vector<std::string> kinds{"setValue", "addGraphNode"};
    auto                           failed_at_base = ParseJson(R"json({
        "acceptedProperties": {},
        "document": {
            "contractVersion": 1,
            "viewKind": "graph",
            "revision": 4
        },
        "diagnostics": [
            {"severity": "error", "message": "unknown nodeId"}
        ]
    })json");
    auto base_result = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(
        failed_at_base,
        4,
        kinds);
    EXPECT_TRUE(base_result.valid) << base_result.message;

    auto failed_beyond_base = ParseJson(R"json({
        "acceptedProperties": {},
        "document": {
            "contractVersion": 1,
            "viewKind": "graph",
            "revision": 5
        },
        "diagnostics": [
            {"severity": "error", "message": "unknown nodeId"}
        ]
    })json");
    auto beyond_result = Das::Core::TaskScheduler::ValidateAuthoringApplyResult(
        failed_beyond_base,
        4,
        kinds);
    EXPECT_FALSE(beyond_result.valid);
    EXPECT_EQ(beyond_result.error_kind, "revisionMismatch");
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

// renameGraphNode 是语义变更：成功必须 revision == baseRevision + 1（与
// viewData 轻量通道两族相反）；未推进时 revisionMismatch。
TEST(
    TaskAuthoringContractTest,
    ApplyResultRenameGraphNodeSuccessRequiresBasePlusOne)
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
        "renameGraphNode");
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
        "renameGraphNode");
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
