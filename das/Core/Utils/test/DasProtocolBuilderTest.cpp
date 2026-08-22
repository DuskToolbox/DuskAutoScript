#include <cassert>
#include <cpp_yyjson.hpp>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/DasJson.h>
#include <das/_autogen/idl/abi/IDasAuthoringChangeBuilder.h>
#include <das/_autogen/idl/abi/IDasAuthoringFieldBuilder.h>
#include <das/_autogen/idl/abi/IDasSettingsChangeBuilder.h>
#include <das/_autogen/idl/header/IDasAuthoringChangeBuilder.generated.h>
#include <das/_autogen/idl/header/IDasAuthoringFieldBuilder.generated.h>
#include <das/_autogen/idl/header/IDasSettingsChangeBuilder.generated.h>
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

DAS_NS_ANONYMOUS_DETAILS_BEGIN

using IDasJsonPtr = DAS::DasPtr<Das::ExportInterface::IDasJson>;

// 从 IDasJson 取紧凑序列化文本（indent < 0 即紧凑，与 yyjson 默认写同形）。
std::string ToCompactString(Das::ExportInterface::IDasJson* json)
{
    DAS::DasPtr<IDasReadOnlyString> serialized;
    const auto serialize_result = json->ToString(-1, serialized.Put());
    if (DAS::IsFailed(serialize_result))
    {
        return {};
    }
    const char* utf8_text = nullptr;
    if (DAS::IsFailed(serialized->GetUtf8(&utf8_text)) || utf8_text == nullptr)
    {
        return {};
    }
    return std::string{utf8_text};
}

// 解析 JSON 字面量为 yyjson 值（对照侧手拼 wire 的输入）。
yyjson::value ParseYyjsonValue(std::string_view literal)
{
    auto parsed = Das::Utils::ParseYyjsonFromString(literal);
    return parsed ? std::move(*parsed) : Das::Utils::MakeYyjsonObject();
}

DAS_NS_ANONYMOUS_DETAILS_END

using Details::IDasJsonPtr;

// ---------------------------------------------------------------------------
// IDasSettingsChangeBuilder — 组件 settings-change wire 字节等价
// ---------------------------------------------------------------------------

TEST(DasProtocolBuilderTest, SettingsChangeBuilderMatchesHandAssembledWire)
{
    constexpr std::string_view SETTINGS_LITERAL =
        R"({"delay_ms":250,"label":"delay"})";
    constexpr std::string_view PAYLOAD_LITERAL = R"({"note":"编排载荷"})";

    DAS::DasPtr<Das::ExportInterface::IDasSettingsChangeBuilder> builder;
    ASSERT_EQ(CreateIDasSettingsChangeBuilder(builder.Put()), DAS_S_OK);

    IDasJsonPtr settings_value;
    ASSERT_EQ(
        ParseDasJsonFromString(
            std::string{SETTINGS_LITERAL}.c_str(),
            settings_value.Put()),
        DAS_S_OK);
    IDasJsonPtr payload_value;
    ASSERT_EQ(
        ParseDasJsonFromString(
            std::string{PAYLOAD_LITERAL}.c_str(),
            payload_value.Put()),
        DAS_S_OK);

    ASSERT_EQ(builder->SetSettings(settings_value.Get()), DAS_S_OK);
    ASSERT_EQ(builder->SetPayload(payload_value.Get()), DAS_S_OK);

    IDasJsonPtr exported;
    ASSERT_EQ(builder->ToJson(exported.Put()), DAS_S_OK);

    auto hand_root = Das::Utils::MakeYyjsonObject();
    auto hand_object_optional = hand_root.as_object();
    ASSERT_TRUE(hand_object_optional.has_value());
    auto hand_object = *hand_object_optional;
    hand_object[std::string_view{"settings"}] = Das::Utils::CloneYyjsonValue(
        Details::ParseYyjsonValue(SETTINGS_LITERAL));
    hand_object[std::string_view{"payload"}] = Das::Utils::CloneYyjsonValue(
        Details::ParseYyjsonValue(PAYLOAD_LITERAL));

    const auto hand_serialized = Das::Utils::SerializeYyjsonValue(hand_root);
    ASSERT_TRUE(hand_serialized.has_value());
    EXPECT_EQ(Details::ToCompactString(exported.Get()), *hand_serialized);
}

TEST(DasProtocolBuilderTest, AcceptedSettingsAloneMatchesWire)
{
    constexpr std::string_view ACCEPTED_LITERAL =
        R"({"pi":{"language":"zh-CN"}})";

    DAS::DasPtr<Das::ExportInterface::IDasSettingsChangeBuilder> builder;
    ASSERT_EQ(CreateIDasSettingsChangeBuilder(builder.Put()), DAS_S_OK);

    IDasJsonPtr accepted_value;
    ASSERT_EQ(
        ParseDasJsonFromString(
            std::string{ACCEPTED_LITERAL}.c_str(),
            accepted_value.Put()),
        DAS_S_OK);

    ASSERT_EQ(builder->SetAcceptedSettings(accepted_value.Get()), DAS_S_OK);

    IDasJsonPtr exported;
    ASSERT_EQ(builder->ToJson(exported.Put()), DAS_S_OK);

    auto hand_root = Das::Utils::MakeYyjsonObject();
    auto hand_object_optional = hand_root.as_object();
    ASSERT_TRUE(hand_object_optional.has_value());
    auto hand_object = *hand_object_optional;
    hand_object[std::string_view{"acceptedSettings"}] =
        Das::Utils::CloneYyjsonValue(
            Details::ParseYyjsonValue(ACCEPTED_LITERAL));

    const auto hand_serialized = Das::Utils::SerializeYyjsonValue(hand_root);
    ASSERT_TRUE(hand_serialized.has_value());
    EXPECT_EQ(Details::ToCompactString(exported.Get()), *hand_serialized);
}

// ---------------------------------------------------------------------------
// IDasAuthoringChangeBuilder — 变更信封 {kind, payload} 字节等价
// ---------------------------------------------------------------------------

TEST(DasProtocolBuilderTest, MakeSetValueChangeProducesEnvelope)
{
    constexpr std::string_view VALUE_LITERAL =
        R"({"nested":[1,2,{"key":"值"}]})";

    DAS::DasPtr<Das::ExportInterface::IDasAuthoringChangeBuilder> builder;
    ASSERT_EQ(CreateIDasAuthoringChangeBuilder(builder.Put()), DAS_S_OK);

    IDasJsonPtr change_value;
    ASSERT_EQ(
        ParseDasJsonFromString(
            std::string{VALUE_LITERAL}.c_str(),
            change_value.Put()),
        DAS_S_OK);

    const auto change_path =
        DasReadOnlyString::FromUtf8("path.to.field", nullptr);
    ASSERT_EQ(
        builder->MakeSetValueChange(change_path.Get(), change_value.Get()),
        DAS_S_OK);

    IDasJsonPtr exported;
    ASSERT_EQ(builder->ToJson(exported.Put()), DAS_S_OK);

    auto hand_root = Das::Utils::MakeYyjsonObject();
    auto hand_object_optional = hand_root.as_object();
    ASSERT_TRUE(hand_object_optional.has_value());
    auto hand_object = *hand_object_optional;
    hand_object[std::string_view{"kind"}] =
        std::make_pair(std::string_view{"setValue"}, yyjson::copy_string);

    auto hand_payload = Das::Utils::MakeYyjsonObject();
    auto hand_payload_optional = hand_payload.as_object();
    ASSERT_TRUE(hand_payload_optional.has_value());
    auto hand_payload_object = *hand_payload_optional;
    hand_payload_object[std::string_view{"path"}] =
        std::make_pair(std::string_view{"path.to.field"}, yyjson::copy_string);
    hand_payload_object[std::string_view{"value"}] =
        Das::Utils::CloneYyjsonValue(Details::ParseYyjsonValue(VALUE_LITERAL));
    hand_object[std::string_view{"payload"}] = std::move(hand_payload);

    const auto hand_serialized = Das::Utils::SerializeYyjsonValue(hand_root);
    ASSERT_TRUE(hand_serialized.has_value());
    EXPECT_EQ(Details::ToCompactString(exported.Get()), *hand_serialized);
}

// ---------------------------------------------------------------------------
// IDasAuthoringFieldBuilder — FieldDescriptor wire 字节等价
// ---------------------------------------------------------------------------

TEST(DasProtocolBuilderTest, FieldBuilderEmitsFieldDescriptorWire)
{
    constexpr std::string_view FIELD_VALUE_LITERAL = R"([1,2,3])";

    DAS::DasPtr<Das::ExportInterface::IDasAuthoringFieldBuilder> builder;
    ASSERT_EQ(CreateIDasAuthoringFieldBuilder(builder.Put()), DAS_S_OK);

    IDasJsonPtr field_value;
    ASSERT_EQ(
        ParseDasJsonFromString(
            std::string{FIELD_VALUE_LITERAL}.c_str(),
            field_value.Put()),
        DAS_S_OK);

    const auto field_name = DasReadOnlyString::FromUtf8("任务名", nullptr);
    const auto field_label = DasReadOnlyString::FromUtf8("label text", nullptr);
    const auto field_description =
        DasReadOnlyString::FromUtf8("字段说明", nullptr);
    ASSERT_EQ(builder->SetName(field_name.Get()), DAS_S_OK);
    ASSERT_EQ(builder->SetLabel(field_label.Get()), DAS_S_OK);
    ASSERT_EQ(builder->SetValueType(4), DAS_S_OK);
    ASSERT_EQ(builder->SetInputType(0), DAS_S_OK);
    ASSERT_EQ(builder->SetValue(field_value.Get()), DAS_S_OK);
    ASSERT_EQ(builder->SetDescription(field_description.Get()), DAS_S_OK);
    ASSERT_EQ(builder->SetRequired(true), DAS_S_OK);
    ASSERT_EQ(builder->SetReadonly(false), DAS_S_OK);

    IDasJsonPtr exported;
    ASSERT_EQ(builder->ToJson(exported.Put()), DAS_S_OK);

    // 对照侧照 authoring doc 投影的字段名与写出顺序（name / label /
    // valueType / inputType / value / description / required / readonly）。
    auto hand_root = Das::Utils::MakeYyjsonObject();
    auto hand_object_optional = hand_root.as_object();
    ASSERT_TRUE(hand_object_optional.has_value());
    auto hand_object = *hand_object_optional;
    hand_object["name"] =
        std::make_pair(std::string_view{"任务名"}, yyjson::copy_string);
    hand_object["label"] =
        std::make_pair(std::string_view{"label text"}, yyjson::copy_string);
    hand_object["valueType"] = static_cast<int64_t>(4);
    hand_object["inputType"] = static_cast<int64_t>(0);
    hand_object["value"] = Das::Utils::CloneYyjsonValue(
        Details::ParseYyjsonValue(FIELD_VALUE_LITERAL));
    hand_object["description"] =
        std::make_pair(std::string_view{"字段说明"}, yyjson::copy_string);
    hand_object["required"] = true;
    hand_object["readonly"] = false;

    const auto hand_serialized = Das::Utils::SerializeYyjsonValue(hand_root);
    ASSERT_TRUE(hand_serialized.has_value());
    EXPECT_EQ(Details::ToCompactString(exported.Get()), *hand_serialized);
}

// ---------------------------------------------------------------------------
// Clear 与未设字段语义
// ---------------------------------------------------------------------------

TEST(DasProtocolBuilderTest, ClearYieldsEmptyObjectAndUnsetFieldsAbsent)
{
    DAS::DasPtr<Das::ExportInterface::IDasSettingsChangeBuilder> builder;
    ASSERT_EQ(CreateIDasSettingsChangeBuilder(builder.Put()), DAS_S_OK);

    IDasJsonPtr empty_export;
    ASSERT_EQ(builder->ToJson(empty_export.Put()), DAS_S_OK);
    EXPECT_EQ(Details::ToCompactString(empty_export.Get()), "{}");

    IDasJsonPtr settings_value;
    ASSERT_EQ(
        ParseDasJsonFromString(R"({"a":1})", settings_value.Put()),
        DAS_S_OK);
    ASSERT_EQ(builder->SetSettings(settings_value.Get()), DAS_S_OK);

    IDasJsonPtr exported;
    ASSERT_EQ(builder->ToJson(exported.Put()), DAS_S_OK);
    // 未设置的 payload / acceptedSettings 不出现在导出物中。
    EXPECT_EQ(
        Details::ToCompactString(exported.Get()),
        R"({"settings":{"a":1}})");

    ASSERT_EQ(builder->Clear(), DAS_S_OK);

    IDasJsonPtr cleared_export;
    ASSERT_EQ(builder->ToJson(cleared_export.Put()), DAS_S_OK);
    EXPECT_EQ(Details::ToCompactString(cleared_export.Get()), "{}");

    // Clear 之后 builder 仍可继续组装。
    ASSERT_EQ(builder->SetSettings(settings_value.Get()), DAS_S_OK);
    IDasJsonPtr reused_export;
    ASSERT_EQ(builder->ToJson(reused_export.Put()), DAS_S_OK);
    EXPECT_EQ(
        Details::ToCompactString(reused_export.Get()),
        R"({"settings":{"a":1}})");
}
