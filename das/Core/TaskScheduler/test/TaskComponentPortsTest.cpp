#include <das/Utils/TaskComponentPorts.h>
#include <gtest/gtest.h>

#include <das/DasString.hpp>
#include <das/_autogen/idl/abi/IDasPortMap.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace
{
    using Das::DasPtr;
    using Das::ExportInterface::IDasPortMap;
    using Das::ExportInterface::IDasReadOnlyPortMap;
    using Das::Utils::BuildResultPortMap;
    using Das::Utils::CloneJson;
    using Das::Utils::CopyTypedPort;
    using Das::Utils::EmitSignal;
    using Das::Utils::GetPortBool;
    using Das::Utils::GetPortInt;
    using Das::Utils::GetPortJson;
    using Das::Utils::GetPortString;
    using Das::Utils::JsonString;
    using Das::Utils::ParseYyjsonFromString;
    using Das::Utils::ReadJson;
    using Das::Utils::SerializeYyjsonValue;
    using Das::Utils::SetPortInt;
    using Das::Utils::SetPortJson;
    using Das::Utils::SetPortString;
    using Das::Utils::WrapJson;

    DasPtr<IDasPortMap> MakePortMap()
    {
        DasPtr<IDasPortMap> map;
        EXPECT_EQ(CreateIDasPortMap(map.Put()), DAS_S_OK);
        return map;
    }

    yyjson::value ParseOrDie(std::string_view text)
    {
        auto parsed = ParseYyjsonFromString(text);
        if (!parsed)
        {
            ADD_FAILURE() << "fixture JSON failed to parse: " << text;
            return yyjson::value{};
        }
        return std::move(*parsed);
    }

    void SetBoolPort(IDasPortMap* map, std::string_view port_id, bool value)
    {
        const DasReadOnlyString key{std::string{port_id}.c_str()};
        map->SetBool(key.Get(), value);
    }

    void SetFloatPort(IDasPortMap* map, std::string_view port_id, double value)
    {
        const DasReadOnlyString key{std::string{port_id}.c_str()};
        map->SetFloat(key.Get(), value);
    }

    void SetStringPort(
        IDasPortMap*     map,
        std::string_view port_id,
        std::string_view value)
    {
        const DasReadOnlyString key{std::string{port_id}.c_str()};
        const DasReadOnlyString val{std::string{value}.c_str()};
        map->SetString(key.Get(), val.Get());
    }

    double GetFloatPort(IDasReadOnlyPortMap* map, std::string_view port_id)
    {
        double                  value = 0.0;
        const DasReadOnlyString key{std::string{port_id}.c_str()};
        map->GetFloat(key.Get(), &value);
        return value;
    }
} // namespace

TEST(TaskComponentPortsTest, PortStringRoundTripAndDefaultValueBranch)
{
    auto map = MakePortMap();

    SetPortString(map.Get(), "status", "completed");
    EXPECT_EQ(GetPortString(map.Get(), "status"), "completed");

    // Missing port falls back to the caller-provided default.
    EXPECT_EQ(GetPortString(map.Get(), "missing", "fallback"), "fallback");

    // Written empty string also falls back to the default.
    SetPortString(map.Get(), "empty", "");
    EXPECT_EQ(GetPortString(map.Get(), "empty", "fallback"), "fallback");
}

TEST(TaskComponentPortsTest, PortJsonRoundTripForObjectArrayAndNull)
{
    auto map = MakePortMap();

    SetPortJson(
        map.Get(),
        "object",
        ParseOrDie(R"({"name":"demo","answer":42})"));
    auto object_value = GetPortJson(map.Get(), "object");
    ASSERT_TRUE(object_value.has_value());
    ASSERT_TRUE(object_value->is_object());
    auto object_ref = object_value->as_object();
    ASSERT_TRUE(object_ref.has_value());
    EXPECT_EQ(
        (*object_ref)[std::string_view("name")].as_string().value_or(""),
        "demo");
    EXPECT_EQ(
        (*object_ref)[std::string_view("answer")].as_int().value_or(0),
        42);

    SetPortJson(map.Get(), "array", ParseOrDie("[1,2,3]"));
    auto array_value = GetPortJson(map.Get(), "array");
    ASSERT_TRUE(array_value.has_value());
    ASSERT_TRUE(array_value->is_array());
    ASSERT_TRUE(array_value->as_array().has_value());
    EXPECT_EQ(array_value->as_array()->size(), 3U);

    SetPortJson(map.Get(), "null_value", ParseOrDie("null"));
    auto null_value = GetPortJson(map.Get(), "null_value");
    ASSERT_TRUE(null_value.has_value());
    EXPECT_TRUE(null_value->is_null());

    // Empty port string and missing port both yield nullopt.
    SetPortString(map.Get(), "blank", "");
    EXPECT_FALSE(GetPortJson(map.Get(), "blank").has_value());
    EXPECT_FALSE(GetPortJson(map.Get(), "missing").has_value());
}

TEST(TaskComponentPortsTest, TypedReadersFallBackToDefaultOnMissingOrMismatch)
{
    auto map = MakePortMap();

    EXPECT_TRUE(GetPortBool(map.Get(), "missing", true));
    EXPECT_EQ(GetPortInt(map.Get(), "missing", -5), -5);

    SetPortString(map.Get(), "text", "not-a-number");
    EXPECT_FALSE(GetPortBool(map.Get(), "text", false));
    EXPECT_EQ(GetPortInt(map.Get(), "text", 7), 7);
}

TEST(TaskComponentPortsTest, SetPortIntRoundTripsThroughGetPortInt)
{
    auto map = MakePortMap();

    SetPortInt(map.Get(), "count", 1234567);
    EXPECT_EQ(GetPortInt(map.Get(), "count"), 1234567);
    EXPECT_EQ(GetPortInt(map.Get(), "count", 9), 1234567);
}

TEST(TaskComponentPortsTest, EmitSignalWritesSingleElementSignalArray)
{
    auto map = MakePortMap();

    EmitSignal(map.Get(), "on_true");
    EXPECT_EQ(GetPortString(map.Get(), "signals"), R"(["on_true"])");
}

TEST(TaskComponentPortsTest, CopyTypedPortPreservesScalarKinds)
{
    auto source = MakePortMap();
    auto destination = MakePortMap();

    SetPortInt(source.Get(), "port_int", 42);
    SetBoolPort(source.Get(), "port_bool", true);
    SetFloatPort(source.Get(), "port_float", 1.5);
    SetStringPort(source.Get(), "port_string", "hello");

    EXPECT_TRUE(
        CopyTypedPort(source.Get(), "port_int", destination.Get(), "port_int"));
    EXPECT_TRUE(CopyTypedPort(
        source.Get(),
        "port_bool",
        destination.Get(),
        "port_bool"));
    EXPECT_TRUE(CopyTypedPort(
        source.Get(),
        "port_float",
        destination.Get(),
        "port_float"));
    EXPECT_TRUE(CopyTypedPort(
        source.Get(),
        "port_string",
        destination.Get(),
        "port_string"));

    EXPECT_EQ(GetPortInt(destination.Get(), "port_int"), 42);
    EXPECT_TRUE(GetPortBool(destination.Get(), "port_bool"));
    EXPECT_DOUBLE_EQ(GetFloatPort(destination.Get(), "port_float"), 1.5);
    EXPECT_EQ(GetPortString(destination.Get(), "port_string"), "hello");

    // The copied ports keep their runtime variant kinds.
    const Das::ExportInterface::DasVariantType expected_kinds[] = {
        Das::ExportInterface::DAS_VARIANT_TYPE_INT,
        Das::ExportInterface::DAS_VARIANT_TYPE_BOOL,
        Das::ExportInterface::DAS_VARIANT_TYPE_FLOAT,
        Das::ExportInterface::DAS_VARIANT_TYPE_STRING,
    };
    const std::string_view copied_ports[] =
        {"port_int", "port_bool", "port_float", "port_string"};
    for (size_t index = 0; index < 4; ++index)
    {
        Das::ExportInterface::DasVariantType kind =
            Das::ExportInterface::DAS_VARIANT_TYPE_NULL;
        const DasReadOnlyString key{std::string{copied_ports[index]}.c_str()};
        ASSERT_EQ(destination->GetType(key.Get(), &kind), DAS_S_OK);
        EXPECT_EQ(kind, expected_kinds[index])
            << "port: " << copied_ports[index];
    }
}

TEST(TaskComponentPortsTest, CopyTypedPortReturnsFalseForMissingSource)
{
    auto source = MakePortMap();
    auto destination = MakePortMap();

    EXPECT_FALSE(
        CopyTypedPort(source.Get(), "absent", destination.Get(), "out"));
}

TEST(TaskComponentPortsTest, BuildResultPortMapProducesStatusOutputsAndSignals)
{
    DasPtr<IDasPortMap> result;
    const auto          hr = BuildResultPortMap(
        "failed",
        ParseOrDie(R"({"items":[]})"),
        ParseOrDie(R"(["on_error"])"),
        result.Put());
    ASSERT_EQ(hr, DAS_S_OK);
    ASSERT_NE(result.Get(), nullptr);

    EXPECT_EQ(GetPortString(result.Get(), "status"), "failed");

    auto outputs = GetPortJson(result.Get(), "outputs");
    ASSERT_TRUE(outputs.has_value());
    EXPECT_TRUE(outputs->is_object());

    auto signals = GetPortJson(result.Get(), "signals");
    ASSERT_TRUE(signals.has_value());
    ASSERT_TRUE(signals->is_array());
    ASSERT_TRUE(signals->as_array().has_value());
    ASSERT_EQ(signals->as_array()->size(), 1U);
    EXPECT_EQ((*signals->as_array())[0].as_string().value_or(""), "on_error");
}

TEST(TaskComponentPortsTest, WrapJsonReadJsonRoundTripThroughAbiBoundary)
{
    const auto original = ParseOrDie(R"({"name":"demo","values":[1,2,3]})");

    auto wrapped = WrapJson(original);
    ASSERT_NE(wrapped.Get(), nullptr);

    // WrapJson output must serialise back to the same JSON text.
    DasPtr<IDasReadOnlyString> text;
    ASSERT_EQ(wrapped->ToString(0, text.Put()), DAS_S_OK);
    const char* utf8 = nullptr;
    ASSERT_EQ(text->GetUtf8(&utf8), DAS_S_OK);
    const auto wrapped_text = SerializeYyjsonValue(original);
    ASSERT_TRUE(wrapped_text.has_value());
    EXPECT_STREQ(utf8, wrapped_text->c_str());

    // ReadJson inverts WrapJson byte-for-byte.
    auto round_tripped = ReadJson(wrapped.Get());
    ASSERT_TRUE(round_tripped.has_value());
    const auto round_tripped_text = SerializeYyjsonValue(*round_tripped);
    ASSERT_TRUE(round_tripped_text.has_value());
    EXPECT_EQ(*round_tripped_text, *wrapped_text);

    EXPECT_FALSE(ReadJson(nullptr).has_value());
}

TEST(TaskComponentPortsTest, CloneJsonProducesIndependentDeepCopy)
{
    auto original = ParseOrDie(R"({"nested":{"flag":true},"count":1})");
    auto copy = CloneJson(original);

    auto copy_object = copy.as_object();
    ASSERT_TRUE(copy_object.has_value());
    (*copy_object)[std::string_view("count")] = int64_t{99};
    auto nested_copy = (*copy_object)[std::string_view("nested")].as_object();
    ASSERT_TRUE(nested_copy.has_value());
    (*nested_copy)[std::string_view("flag")] = false;

    // Mutating the copy must not touch the original.
    auto original_object = original.as_object();
    ASSERT_TRUE(original_object.has_value());
    EXPECT_EQ(
        (*original_object)[std::string_view("count")].as_int().value_or(0),
        1);
    auto nested_original =
        (*original_object)[std::string_view("nested")].as_object();
    ASSERT_TRUE(nested_original.has_value());
    EXPECT_TRUE(
        (*nested_original)[std::string_view("flag")].as_bool().value_or(false));

    // The copy itself observes the mutations.
    EXPECT_EQ(
        (*copy_object)[std::string_view("count")].as_int().value_or(0),
        99);
}

TEST(TaskComponentPortsTest, JsonStringProducesYyjsonStringValue)
{
    const auto value = JsonString("some text");
    EXPECT_TRUE(value.is_string());
    EXPECT_EQ(value.as_string().value_or(""), "some text");
}
