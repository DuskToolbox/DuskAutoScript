#include <das/Core/GraphRuntime/LegacyJsonAdapter.h>

#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace
{
    using namespace Das::Core::GraphRuntime;

    using IDasPortMap = Das::ExportInterface::IDasPortMap;
    using DasVariantType = Das::ExportInterface::DasVariantType;

    using Das::ExportInterface::DAS_VARIANT_TYPE_BOOL;
    using Das::ExportInterface::DAS_VARIANT_TYPE_FLOAT;
    using Das::ExportInterface::DAS_VARIANT_TYPE_INT;
    using Das::ExportInterface::DAS_VARIANT_TYPE_NULL;
    using Das::ExportInterface::DAS_VARIANT_TYPE_STRING;

    /// Helper: create a fresh PortMap via the C API factory.
    DAS::DasPtr<IDasPortMap> MakePortMap()
    {
        DAS::DasPtr<IDasPortMap> map;
        EXPECT_EQ(CreateIDasPortMap(map.Put()), DAS_S_OK);
        return map;
    }

    /// Helper: read a port's string value as std::string.
    std::string GetStringValue(IDasPortMap* map, const char* key)
    {
        IDasReadOnlyString* p_str = nullptr;
        DasReadOnlyString   k{key};
        auto                result = map->GetString(k.Get(), &p_str);
        if (DAS::IsFailed(result) || p_str == nullptr)
        {
            return {};
        }
        const char* utf8 = nullptr;
        p_str->GetUtf8(&utf8);
        std::string val(utf8 ? utf8 : "");
        p_str->Release();
        return val;
    }

    /// Helper: parse a JSON string into a yyjson value.
    yyjson::value ParseJson(const std::string& json)
    {
        auto opt = Das::Utils::ParseYyjsonFromString(json);
        return opt ? std::move(*opt) : yyjson::value{};
    }

} // namespace

// ===========================================================================
// ConvertJsonToPortMap — basic types
// ===========================================================================

TEST(LegacyJsonAdapterTest, JsonToPortMap_Int)
{
    auto map = MakePortMap();
    std::string err;
    ASSERT_EQ(
        ConvertJsonToPortMap(R"({"x": 42})", map.Get(), &err), DAS_S_OK);

    int64_t           val{};
    DasReadOnlyString key{"x"};
    ASSERT_EQ(map->GetInt(key.Get(), &val), DAS_S_OK);
    EXPECT_EQ(val, 42);
}

TEST(LegacyJsonAdapterTest, JsonToPortMap_Float)
{
    auto map = MakePortMap();
    ASSERT_EQ(
        ConvertJsonToPortMap(R"({"pi": 3.14})", map.Get()), DAS_S_OK);

    double            val{};
    DasReadOnlyString key{"pi"};
    ASSERT_EQ(map->GetFloat(key.Get(), &val), DAS_S_OK);
    EXPECT_NEAR(val, 3.14, 1e-10);
}

TEST(LegacyJsonAdapterTest, JsonToPortMap_Bool)
{
    auto map = MakePortMap();
    ASSERT_EQ(
        ConvertJsonToPortMap(R"({"flag": true})", map.Get()), DAS_S_OK);

    bool              val{};
    DasReadOnlyString key{"flag"};
    ASSERT_EQ(map->GetBool(key.Get(), &val), DAS_S_OK);
    EXPECT_TRUE(val);
}

TEST(LegacyJsonAdapterTest, JsonToPortMap_String)
{
    auto map = MakePortMap();
    ASSERT_EQ(
        ConvertJsonToPortMap(R"({"name": "Dusk"})", map.Get()), DAS_S_OK);

    EXPECT_EQ(GetStringValue(map.Get(), "name"), "Dusk");
}

TEST(LegacyJsonAdapterTest, JsonToPortMap_Null)
{
    auto map = MakePortMap();
    ASSERT_EQ(
        ConvertJsonToPortMap(R"({"nil": null})", map.Get()), DAS_S_OK);

    // Null values are not stored (no SetNull API). The key should not exist.
    DasReadOnlyString key{"nil"};
    bool              has = true;
    ASSERT_EQ(map->Has(key.Get(), &has), DAS_S_OK);
    EXPECT_FALSE(has);
}

TEST(LegacyJsonAdapterTest, JsonToPortMap_NestedObject)
{
    auto map = MakePortMap();
    ASSERT_EQ(
        ConvertJsonToPortMap(R"({"config": {"key": 1}})", map.Get()),
        DAS_S_OK);

    // Nested objects are stored as serialized JSON strings via SetString.
    // GetJson returns the same as GetString in DasPortMapImpl.
    IDasReadOnlyString* p_json = nullptr;
    DasReadOnlyString   key{"config"};
    ASSERT_EQ(map->GetJson(key.Get(), &p_json), DAS_S_OK);
    ASSERT_NE(p_json, nullptr);

    const char* utf8 = nullptr;
    p_json->GetUtf8(&utf8);
    std::string json_str(utf8 ? utf8 : "");
    p_json->Release();

    // The stored value should be a non-empty string.
    ASSERT_FALSE(json_str.empty());

    // Parse the stored string to verify it's valid JSON.
    auto parsed = Das::Utils::ParseYyjsonFromString(json_str);
    ASSERT_TRUE(parsed.has_value());
    auto nested_obj = parsed->as_object();
    ASSERT_TRUE(nested_obj.has_value());
    // Should contain the "key" field from the nested object.
    EXPECT_TRUE(nested_obj->contains(std::string_view("key")));
}

TEST(LegacyJsonAdapterTest, JsonToPortMap_MultiType)
{
    auto map = MakePortMap();
    ASSERT_EQ(
        ConvertJsonToPortMap(
            R"({"a": 1, "b": "hello", "c": true, "d": null})", map.Get()),
        DAS_S_OK);

    int64_t a_val{};
    DasReadOnlyString a_key{"a"};
    ASSERT_EQ(map->GetInt(a_key.Get(), &a_val), DAS_S_OK);
    EXPECT_EQ(a_val, 1);

    EXPECT_EQ(GetStringValue(map.Get(), "b"), "hello");

    bool c_val{};
    DasReadOnlyString c_key{"c"};
    ASSERT_EQ(map->GetBool(c_key.Get(), &c_val), DAS_S_OK);
    EXPECT_TRUE(c_val);

    // d is null → not stored
    DasReadOnlyString d_key{"d"};
    bool              d_has{};
    ASSERT_EQ(map->Has(d_key.Get(), &d_has), DAS_S_OK);
    EXPECT_FALSE(d_has);
}

TEST(LegacyJsonAdapterTest, JsonToPortMap_EmptyObject)
{
    auto map = MakePortMap();
    ASSERT_EQ(ConvertJsonToPortMap("{}", map.Get()), DAS_S_OK);

    // Empty object → no entries. Verify via GetKeys.
    DAS::DasPtr<Das::ExportInterface::IDasStringVector> keys;
    ASSERT_EQ(map->GetKeys(keys.Put()), DAS_S_OK);
    uint64_t count = 0;
    ASSERT_EQ(keys->Size(&count), DAS_S_OK);
    EXPECT_EQ(count, 0u);
}

// ===========================================================================
// ConvertPortMapToJson — basic types
// ===========================================================================

TEST(LegacyJsonAdapterTest, PortMapToJson_Int)
{
    auto map = MakePortMap();
    DasReadOnlyString key{"x"};
    ASSERT_EQ(map->SetInt(key.Get(), 99), DAS_S_OK);

    std::string json;
    ASSERT_EQ(ConvertPortMapToJson(map.Get(), json), DAS_S_OK);

    auto root = ParseJson(json);
    auto obj  = root.as_object();
    ASSERT_TRUE(obj.has_value());

    auto val = (*obj)[std::string_view("x")];
    // yyjson may store positive int64_t as uint.
    ASSERT_TRUE(val.is_sint() || val.is_uint());
    int64_t int_val = val.is_sint() ? val.as_sint().value_or(0)
                                    : static_cast<int64_t>(val.as_uint().value_or(0));
    EXPECT_EQ(int_val, 99);
}

TEST(LegacyJsonAdapterTest, PortMapToJson_String)
{
    auto map = MakePortMap();
    DasReadOnlyString key{"name"};
    DasReadOnlyString val{"Dusk"};
    ASSERT_EQ(map->SetString(key.Get(), val.Get()), DAS_S_OK);

    std::string json;
    ASSERT_EQ(ConvertPortMapToJson(map.Get(), json), DAS_S_OK);

    auto root = ParseJson(json);
    auto obj  = root.as_object();
    ASSERT_TRUE(obj.has_value());
    ASSERT_TRUE((*obj)[std::string_view("name")].is_string());
    EXPECT_EQ((*obj)[std::string_view("name")].as_string(), std::string_view("Dusk"));
}

TEST(LegacyJsonAdapterTest, PortMapToJson_Bool)
{
    auto map = MakePortMap();
    DasReadOnlyString key{"flag"};
    ASSERT_EQ(map->SetBool(key.Get(), true), DAS_S_OK);

    std::string json;
    ASSERT_EQ(ConvertPortMapToJson(map.Get(), json), DAS_S_OK);

    auto root = ParseJson(json);
    auto obj  = root.as_object();
    ASSERT_TRUE(obj.has_value());
    ASSERT_TRUE((*obj)[std::string_view("flag")].is_bool());
    EXPECT_EQ((*obj)[std::string_view("flag")].as_bool(), true);
}

TEST(LegacyJsonAdapterTest, PortMapToJson_Null)
{
    // PortMap has no SetNull. We test that an empty key check returns the
    // correct type from a DasPortMapImpl that was constructed with monostate.
    // For now, verify that a port with DAS_VARIANT_TYPE_NULL maps to JSON null.
    // Since we cannot set null via public API, we skip this test body.
    GTEST_SKIP() << "No SetNull API on IDasPortMap — null mapping tested via "
                    "JsonToPortMap_Null.";
}

TEST(LegacyJsonAdapterTest, PortMapToJson_NestedJson)
{
    auto map = MakePortMap();
    // Store a JSON string via SetString.
    DasReadOnlyString key{"cfg"};
    DasReadOnlyString json_val{R"({"k": 1})"};
    ASSERT_EQ(map->SetString(key.Get(), json_val.Get()), DAS_S_OK);

    std::string json;
    ASSERT_EQ(ConvertPortMapToJson(map.Get(), json), DAS_S_OK);

    auto root = ParseJson(json);
    auto obj  = root.as_object();
    ASSERT_TRUE(obj.has_value());

    // The "cfg" value should be inlined as a nested object (not a string),
    // because ConvertPortMapToJson detects JSON-like strings and inlines them.
    auto cfg_val = (*obj)[std::string_view("cfg")];
    ASSERT_TRUE(cfg_val.is_object());
    auto cfg_obj = cfg_val.as_object();
    ASSERT_TRUE(cfg_obj.has_value());
    EXPECT_TRUE(cfg_obj->contains(std::string_view("k")));
}

TEST(LegacyJsonAdapterTest, PortMapToJson_Float)
{
    auto map = MakePortMap();
    DasReadOnlyString key{"pi"};
    ASSERT_EQ(map->SetFloat(key.Get(), 3.14159), DAS_S_OK);

    std::string json;
    ASSERT_EQ(ConvertPortMapToJson(map.Get(), json), DAS_S_OK);

    auto root = ParseJson(json);
    auto obj  = root.as_object();
    ASSERT_TRUE(obj.has_value());
    ASSERT_TRUE((*obj)[std::string_view("pi")].is_real());
    EXPECT_NEAR(
        (*obj)[std::string_view("pi")].as_real().value_or(0.0), 3.14159, 1e-10);
}

// ===========================================================================
// Round-trip: JSON → PortMap → JSON
// ===========================================================================

TEST(LegacyJsonAdapterTest, JsonToPortMapRoundTrip)
{
    const std::string input = R"({"a": 1, "b": "x", "c": true})";

    // JSON → PortMap
    auto map = MakePortMap();
    ASSERT_EQ(ConvertJsonToPortMap(input, map.Get()), DAS_S_OK);

    // PortMap → JSON
    std::string output;
    ASSERT_EQ(ConvertPortMapToJson(map.Get(), output), DAS_S_OK);

    // Parse output and verify semantically equivalent.
    auto out_root = ParseJson(output);
    auto out_obj  = out_root.as_object();
    ASSERT_TRUE(out_obj.has_value());

    // Verify each field.
    {
        auto a_val = (*out_obj)[std::string_view("a")];
        ASSERT_TRUE(a_val.is_sint() || a_val.is_uint());
        int64_t a_int = a_val.is_sint() ? a_val.as_sint().value_or(0)
                                        : static_cast<int64_t>(a_val.as_uint().value_or(0));
        EXPECT_EQ(a_int, 1);
    }

    ASSERT_TRUE((*out_obj)[std::string_view("b")].is_string());
    EXPECT_EQ(
        (*out_obj)[std::string_view("b")].as_string(),
        std::string_view("x"));

    ASSERT_TRUE((*out_obj)[std::string_view("c")].is_bool());
    EXPECT_EQ((*out_obj)[std::string_view("c")].as_bool(), true);

    // "d" (null) is NOT preserved in round-trip (no SetNull).
    EXPECT_FALSE(out_obj->contains(std::string_view("d")));
}

// ===========================================================================
// MapDasResultToStatus
// ===========================================================================

TEST(LegacyJsonAdapterTest, MapDasResultToStatus)
{
    EXPECT_STREQ(MapDasResultToStatus(DAS_S_OK), "ok");
    EXPECT_STREQ(MapDasResultToStatus(DAS_E_TIMEOUT), "cancelled");
    EXPECT_STREQ(MapDasResultToStatus(DAS_E_FAIL), "error");
    EXPECT_STREQ(MapDasResultToStatus(DAS_E_NO_INTERFACE), "error");
}

// ===========================================================================
// Legacy adapter wraps old component
// ===========================================================================

TEST(LegacyJsonAdapterTest, WrapsOldComponent)
{
    // Create a mock legacy component via callback.
    // It receives the input JSON, echoes it back with an added "echo" field.
    std::string captured_input;

    auto mock_do = [&captured_input](
                       const std::string& input_json,
                       std::string&       out_result_json) -> DasResult {
        captured_input = input_json;

        // Echo: parse input, add "echo": true, return as result JSON.
        auto parsed = Das::Utils::ParseYyjsonFromString(input_json);
        if (!parsed.has_value())
        {
            return DAS_E_INVALID_JSON;
        }

        auto obj = *parsed->as_object();
        obj[std::string_view("echo")] = true;
        auto serialized = Das::Utils::SerializeYyjsonValue(*parsed);
        if (!serialized.has_value())
        {
            return DAS_E_FAIL;
        }
        out_result_json = std::move(*serialized);
        return DAS_S_OK;
    };

    LegacyJsonTaskComponentAdapter adapter(mock_do);

    // Build input PortMap.
    auto input_map = MakePortMap();
    DasReadOnlyString in_key{"value"};
    input_map->SetInt(in_key.Get(), 42);

    // Call v42 Do().
    DAS::DasPtr<IDasPortMap> output_map;
    ASSERT_EQ(
        adapter.Do(nullptr, input_map.Get(), output_map.Put()), DAS_S_OK);
    ASSERT_NE(output_map.Get(), nullptr);

    // Verify: the mock received correct JSON input.
    auto captured = ParseJson(captured_input);
    auto cap_obj  = captured.as_object();
    ASSERT_TRUE(cap_obj.has_value());
    {
        auto v_val = (*cap_obj)[std::string_view("value")];
        ASSERT_TRUE(v_val.is_sint() || v_val.is_uint());
        int64_t v_int = v_val.is_sint() ? v_val.as_sint().value_or(0)
                                        : static_cast<int64_t>(v_val.as_uint().value_or(0));
        EXPECT_EQ(v_int, 42);
    }

    // Verify: output PortMap contains the echoed data + status.
    bool              bool_val{};
    DasReadOnlyString echo_key{"echo"};
    ASSERT_EQ(output_map->GetBool(echo_key.Get(), &bool_val), DAS_S_OK);
    EXPECT_TRUE(bool_val);

    // Verify __status__ field.
    std::string status = GetStringValue(output_map.Get(), "__status__");
    EXPECT_EQ(status, "ok");
}
