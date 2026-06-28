#include <das/Core/GraphRuntime/DoAdapter.h>

#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace
{
    using namespace Das::Core::GraphRuntime;

    using IDasPortMap = Das::ExportInterface::IDasPortMap;

    DasGuid MakeNode(const std::string_view s)
    {
        return Das::Core::ForeignInterfaceHost::MakeDasGuid(s);
    }

    const DasGuid kSourceNode =
        MakeNode("11111111-1111-1111-1111-111111111111");
    const DasGuid kTargetNode =
        MakeNode("22222222-2222-2222-2222-222222222222");
    const DasGuid kOtherNode = MakeNode("33333333-3333-3333-3333-333333333333");

    Dto::PortBindingDto MakeBinding(
        const std::string& src_node,
        const std::string& src_port,
        const std::string& tgt_port)
    {
        Dto::PortBindingDto b;
        b.source_node_id = src_node;
        b.source_port_id = src_port;
        b.target_node_id = "22222222-2222-2222-2222-222222222222";
        b.target_port_id = tgt_port;
        b.expected_type = "";
        return b;
    }

    std::string GuidToString(DasGuid guid)
    {
        return Das::Core::ForeignInterfaceHost::DasGuidToStdString(guid);
    }

    // Broadcast (graph-input) binding: the source is the "$graph_input" sentinel
    // rather than a real node, and the value comes from default_value. (DAS-75
    // graph_inputs broadcast injection.)
    Dto::PortBindingDto MakeBroadcastBinding(
        const std::string& graph_input_port,
        const std::string& tgt_port,
        yyjson::value      default_value)
    {
        Dto::PortBindingDto b;
        b.source_node_id = "$graph_input";
        b.source_port_id = graph_input_port;
        b.target_node_id = "22222222-2222-2222-2222-222222222222";
        b.target_port_id = tgt_port;
        b.expected_type = "";
        b.default_value = std::move(default_value);
        return b;
    }

    yyjson::value ParseJson(const std::string& json)
    {
        auto v = Das::Utils::ParseYyjsonFromString(json);
        return v ? std::move(*v) : yyjson::value{};
    }

} // namespace

// ===========================================================================
// BuildInputPortMap — basic scalar types
// ===========================================================================

TEST(DoAdapterTest, BuildInputPortMap_IntValue)
{
    PortFrame frame;
    frame.Set(kSourceNode, "out", PortValue(int64_t{42}));

    auto bindings = {MakeBinding(GuidToString(kSourceNode), "out", "in")};

    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(BuildInputPortMap(frame, bindings, map.Put()), DAS_S_OK);
    ASSERT_NE(map.Get(), nullptr);

    int64_t           val{};
    DasReadOnlyString key{"in"};
    ASSERT_EQ(map->GetInt(key.Get(), &val), DAS_S_OK);
    EXPECT_EQ(val, 42);
}

TEST(DoAdapterTest, BuildInputPortMap_FloatValue)
{
    PortFrame frame;
    frame.Set(kSourceNode, "out", PortValue(3.14));

    auto bindings = {MakeBinding(GuidToString(kSourceNode), "out", "in")};

    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(BuildInputPortMap(frame, bindings, map.Put()), DAS_S_OK);

    double            val{};
    DasReadOnlyString key{"in"};
    ASSERT_EQ(map->GetFloat(key.Get(), &val), DAS_S_OK);
    EXPECT_DOUBLE_EQ(val, 3.14);
}

TEST(DoAdapterTest, BuildInputPortMap_StringValue)
{
    PortFrame frame;
    frame.Set(kSourceNode, "out", PortValue(std::string("hello")));

    auto bindings = {MakeBinding(GuidToString(kSourceNode), "out", "in")};

    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(BuildInputPortMap(frame, bindings, map.Put()), DAS_S_OK);

    IDasReadOnlyString* p_str = nullptr;
    DasReadOnlyString   key{"in"};
    ASSERT_EQ(map->GetString(key.Get(), &p_str), DAS_S_OK);
    ASSERT_NE(p_str, nullptr);

    const char* utf8 = nullptr;
    p_str->GetUtf8(&utf8);
    EXPECT_STREQ(utf8, "hello");
    p_str->Release();
}

TEST(DoAdapterTest, BuildInputPortMap_BoolValue)
{
    PortFrame frame;
    frame.Set(kSourceNode, "out", PortValue(true));

    auto bindings = {MakeBinding(GuidToString(kSourceNode), "out", "in")};

    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(BuildInputPortMap(frame, bindings, map.Put()), DAS_S_OK);

    bool              val{};
    DasReadOnlyString key{"in"};
    ASSERT_EQ(map->GetBool(key.Get(), &val), DAS_S_OK);
    EXPECT_TRUE(val);
}

TEST(DoAdapterTest, BuildInputPortMap_SignalValue)
{
    PortFrame frame;
    frame.Set(kSourceNode, "out", PortValue::Signal());

    auto bindings = {MakeBinding(GuidToString(kSourceNode), "out", "in")};

    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(BuildInputPortMap(frame, bindings, map.Put()), DAS_S_OK);

    // Signal is a control-flow marker — the port should not be present
    // in the resulting map.
    DasReadOnlyString key{"in"};
    bool              has{};
    ASSERT_EQ(map->Has(key.Get(), &has), DAS_S_OK);
    EXPECT_FALSE(has);
}

// ===========================================================================
// BuildInputPortMap — multiple bindings
// ===========================================================================

TEST(DoAdapterTest, BuildInputPortMap_MultipleBindings)
{
    PortFrame frame;
    frame.Set(kSourceNode, "out_a", PortValue(int64_t{10}));
    frame.Set(kSourceNode, "out_b", PortValue(std::string("world")));
    frame.Set(kOtherNode, "result", PortValue(2.5));

    auto bindings = {
        MakeBinding(GuidToString(kSourceNode), "out_a", "in_x"),
        MakeBinding(GuidToString(kSourceNode), "out_b", "in_y"),
        MakeBinding(GuidToString(kOtherNode), "result", "in_z"),
    };

    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(BuildInputPortMap(frame, bindings, map.Put()), DAS_S_OK);

    int64_t           int_val{};
    DasReadOnlyString key_x{"in_x"};
    ASSERT_EQ(map->GetInt(key_x.Get(), &int_val), DAS_S_OK);
    EXPECT_EQ(int_val, 10);

    IDasReadOnlyString* p_str = nullptr;
    DasReadOnlyString   key_y{"in_y"};
    ASSERT_EQ(map->GetString(key_y.Get(), &p_str), DAS_S_OK);
    ASSERT_NE(p_str, nullptr);
    const char* utf8 = nullptr;
    p_str->GetUtf8(&utf8);
    EXPECT_STREQ(utf8, "world");
    p_str->Release();

    double            float_val{};
    DasReadOnlyString key_z{"in_z"};
    ASSERT_EQ(map->GetFloat(key_z.Get(), &float_val), DAS_S_OK);
    EXPECT_DOUBLE_EQ(float_val, 2.5);
}

// ===========================================================================
// BuildInputPortMap — null output pointer
// ===========================================================================

TEST(DoAdapterTest, BuildInputPortMap_NullOutput)
{
    PortFrame frame;
    auto      bindings = {MakeBinding(GuidToString(kSourceNode), "out", "in")};

    EXPECT_EQ(
        BuildInputPortMap(frame, bindings, nullptr),
        DAS_E_INVALID_POINTER);
}

// ===========================================================================
// BuildInputPortMap — missing source port (graceful skip)
// ===========================================================================

TEST(DoAdapterTest, BuildInputPortMap_MissingSourcePort)
{
    PortFrame frame;
    // No entries in frame.

    auto bindings = {
        MakeBinding(GuidToString(kSourceNode), "nonexistent", "in")};

    DAS::DasPtr<IDasPortMap> map;
    // Should succeed — missing source ports are logged and skipped.
    ASSERT_EQ(BuildInputPortMap(frame, bindings, map.Put()), DAS_S_OK);
    ASSERT_NE(map.Get(), nullptr);

    // The map should be empty.
    bool              has{};
    DasReadOnlyString key{"in"};
    ASSERT_EQ(map->Has(key.Get(), &has), DAS_S_OK);
    EXPECT_FALSE(has);
}

// ===========================================================================
// BuildInputPortMap — null PortValue is skipped
// ===========================================================================

TEST(DoAdapterTest, BuildInputPortMap_NullValueSkipped)
{
    PortFrame frame;
    frame.Set(kSourceNode, "out", PortValue());

    auto bindings = {MakeBinding(GuidToString(kSourceNode), "out", "in")};

    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(BuildInputPortMap(frame, bindings, map.Put()), DAS_S_OK);

    DasReadOnlyString key{"in"};
    bool              has{};
    ASSERT_EQ(map->Has(key.Get(), &has), DAS_S_OK);
    EXPECT_FALSE(has);
}

// ===========================================================================
// BuildInputPortMap — broadcast (graph-input) bindings (DAS-75)
//
// A graph_inputs broadcast binding carries no upstream node; its default_value
// must be materialised directly into the input PortMap so graph-level settings
// reach the component (MAA global_option semantics).
// ===========================================================================

TEST(DoAdapterTest, BuildInputPortMap_BroadcastIntDefault)
{
    PortFrame frame; // intentionally empty — value comes from the default

    auto bindings = {MakeBroadcastBinding("threshold", "in", ParseJson("42"))};

    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(BuildInputPortMap(frame, bindings, map.Put()), DAS_S_OK);

    int64_t           val{};
    DasReadOnlyString key{"in"};
    ASSERT_EQ(map->GetInt(key.Get(), &val), DAS_S_OK);
    EXPECT_EQ(val, 42);
}

TEST(DoAdapterTest, BuildInputPortMap_BroadcastBoolDefault)
{
    PortFrame frame;

    auto bindings = {MakeBroadcastBinding("flag", "in", ParseJson("true"))};

    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(BuildInputPortMap(frame, bindings, map.Put()), DAS_S_OK);

    bool              val{};
    DasReadOnlyString key{"in"};
    ASSERT_EQ(map->GetBool(key.Get(), &val), DAS_S_OK);
    EXPECT_TRUE(val);
}

TEST(DoAdapterTest, BuildInputPortMap_BroadcastStringDefault)
{
    PortFrame frame;

    auto bindings = {
        MakeBroadcastBinding("name", "in", ParseJson(R"("greeting")"))};

    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(BuildInputPortMap(frame, bindings, map.Put()), DAS_S_OK);

    IDasReadOnlyString* p_str = nullptr;
    DasReadOnlyString   key{"in"};
    ASSERT_EQ(map->GetString(key.Get(), &p_str), DAS_S_OK);
    ASSERT_NE(p_str, nullptr);
    const char* utf8 = nullptr;
    p_str->GetUtf8(&utf8);
    EXPECT_STREQ(utf8, "greeting");
    p_str->Release();
}

TEST(DoAdapterTest, BuildInputPortMap_BroadcastFloatDefault)
{
    PortFrame frame;

    auto bindings = {MakeBroadcastBinding("ratio", "in", ParseJson("3.14"))};

    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(BuildInputPortMap(frame, bindings, map.Put()), DAS_S_OK);

    double            val{};
    DasReadOnlyString key{"in"};
    ASSERT_EQ(map->GetFloat(key.Get(), &val), DAS_S_OK);
    EXPECT_DOUBLE_EQ(val, 3.14);
}

TEST(DoAdapterTest, BuildInputPortMap_BroadcastNullDefaultSkipped)
{
    PortFrame frame;

    auto bindings = {MakeBroadcastBinding("opt", "in", ParseJson("null"))};

    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(BuildInputPortMap(frame, bindings, map.Put()), DAS_S_OK);

    // null default → port left unset; the component keeps its own default.
    DasReadOnlyString key{"in"};
    bool              has{};
    ASSERT_EQ(map->Has(key.Get(), &has), DAS_S_OK);
    EXPECT_FALSE(has);
}

TEST(DoAdapterTest, BuildInputPortMap_BroadcastNonScalarSkipped)
{
    PortFrame frame;

    // Object defaults are not scalar options — skipped with a warning.
    auto bindings = {MakeBroadcastBinding("cfg", "in", ParseJson(R"({"a":1})"))};

    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(BuildInputPortMap(frame, bindings, map.Put()), DAS_S_OK);

    DasReadOnlyString key{"in"};
    bool              has{};
    ASSERT_EQ(map->Has(key.Get(), &has), DAS_S_OK);
    EXPECT_FALSE(has);
}

TEST(DoAdapterTest, BuildInputPortMap_BroadcastAndPointToPointCoexist)
{
    // A node may receive both a normal data edge and a graph-input broadcast;
    // both must land in the input map.
    PortFrame frame;
    frame.Set(kSourceNode, "out", PortValue(int64_t{7}));

    std::vector<Dto::PortBindingDto> bindings = {
        MakeBinding(GuidToString(kSourceNode), "out", "in_data"),
        MakeBroadcastBinding("threshold", "in_opt", ParseJson("99")),
    };

    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(BuildInputPortMap(frame, bindings, map.Put()), DAS_S_OK);

    int64_t           data_val{};
    DasReadOnlyString data_key{"in_data"};
    ASSERT_EQ(map->GetInt(data_key.Get(), &data_val), DAS_S_OK);
    EXPECT_EQ(data_val, 7);

    int64_t           opt_val{};
    DasReadOnlyString opt_key{"in_opt"};
    ASSERT_EQ(map->GetInt(opt_key.Get(), &opt_val), DAS_S_OK);
    EXPECT_EQ(opt_val, 99);
}

// ===========================================================================
// ExtractOutputPortMap — basic scalar types
// ===========================================================================

TEST(DoAdapterTest, ExtractOutputPortMap_IntValue)
{
    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(CreateIDasPortMap(map.Put()), DAS_S_OK);

    DasReadOnlyString key{"out"};
    ASSERT_EQ(map->SetInt(key.Get(), 99), DAS_S_OK);

    PortFrame frame;
    ASSERT_EQ(ExtractOutputPortMap(map.Get(), kTargetNode, frame), DAS_S_OK);

    const auto* pv = frame.Find({kTargetNode, "out"});
    ASSERT_NE(pv, nullptr);
    ASSERT_TRUE(pv->IsInt());
    EXPECT_EQ(*pv->AsInt(), 99);
}

TEST(DoAdapterTest, ExtractOutputPortMap_FloatValue)
{
    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(CreateIDasPortMap(map.Put()), DAS_S_OK);

    DasReadOnlyString key{"out"};
    ASSERT_EQ(map->SetFloat(key.Get(), 1.23), DAS_S_OK);

    PortFrame frame;
    ASSERT_EQ(ExtractOutputPortMap(map.Get(), kTargetNode, frame), DAS_S_OK);

    const auto* pv = frame.Find({kTargetNode, "out"});
    ASSERT_NE(pv, nullptr);
    ASSERT_TRUE(pv->IsFloat());
    EXPECT_DOUBLE_EQ(*pv->AsFloat(), 1.23);
}

TEST(DoAdapterTest, ExtractOutputPortMap_StringValue)
{
    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(CreateIDasPortMap(map.Put()), DAS_S_OK);

    DasReadOnlyString key{"out"};
    DasReadOnlyString val{"test_str"};
    ASSERT_EQ(map->SetString(key.Get(), val.Get()), DAS_S_OK);

    PortFrame frame;
    ASSERT_EQ(ExtractOutputPortMap(map.Get(), kTargetNode, frame), DAS_S_OK);

    const auto* pv = frame.Find({kTargetNode, "out"});
    ASSERT_NE(pv, nullptr);
    ASSERT_TRUE(pv->IsString());
    EXPECT_EQ(*pv->AsString(), "test_str");
}

TEST(DoAdapterTest, ExtractOutputPortMap_BoolValue)
{
    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(CreateIDasPortMap(map.Put()), DAS_S_OK);

    DasReadOnlyString key{"out"};
    ASSERT_EQ(map->SetBool(key.Get(), true), DAS_S_OK);

    PortFrame frame;
    ASSERT_EQ(ExtractOutputPortMap(map.Get(), kTargetNode, frame), DAS_S_OK);

    const auto* pv = frame.Find({kTargetNode, "out"});
    ASSERT_NE(pv, nullptr);
    ASSERT_TRUE(pv->IsBool());
    EXPECT_TRUE(*pv->AsBool());
}

TEST(DoAdapterTest, ExtractOutputPortMap_MultipleEntries)
{
    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(CreateIDasPortMap(map.Put()), DAS_S_OK);

    DasReadOnlyString k1{"a"};
    DasReadOnlyString k2{"b"};
    map->SetInt(k1.Get(), 10);
    map->SetBool(k2.Get(), false);

    PortFrame frame;
    ASSERT_EQ(ExtractOutputPortMap(map.Get(), kTargetNode, frame), DAS_S_OK);

    const auto* pv_a = frame.Find({kTargetNode, "a"});
    ASSERT_NE(pv_a, nullptr);
    ASSERT_TRUE(pv_a->IsInt());
    EXPECT_EQ(*pv_a->AsInt(), 10);

    const auto* pv_b = frame.Find({kTargetNode, "b"});
    ASSERT_NE(pv_b, nullptr);
    ASSERT_TRUE(pv_b->IsBool());
    EXPECT_FALSE(*pv_b->AsBool());
}

// ===========================================================================
// ExtractOutputPortMap — null map
// ===========================================================================

TEST(DoAdapterTest, ExtractOutputPortMap_NullMap)
{
    PortFrame frame;
    EXPECT_EQ(
        ExtractOutputPortMap(nullptr, kTargetNode, frame),
        DAS_E_INVALID_POINTER);
}

// ===========================================================================
// Round-trip: BuildInput → ExtractOutput
// ===========================================================================

TEST(DoAdapterTest, RoundTrip_IntAndString)
{
    // Setup: source node has int and string outputs in PortFrame.
    PortFrame frame;
    frame.Set(kSourceNode, "count", PortValue(int64_t{7}));
    frame.Set(kSourceNode, "name", PortValue(std::string("alice")));

    // Build input map for the target node.
    auto bindings = {
        MakeBinding(GuidToString(kSourceNode), "count", "count"),
        MakeBinding(GuidToString(kSourceNode), "name", "name"),
    };

    DAS::DasPtr<IDasPortMap> input_map;
    ASSERT_EQ(BuildInputPortMap(frame, bindings, input_map.Put()), DAS_S_OK);

    // Verify input map contents.
    int64_t           count_val{};
    DasReadOnlyString count_key{"count"};
    ASSERT_EQ(input_map->GetInt(count_key.Get(), &count_val), DAS_S_OK);
    EXPECT_EQ(count_val, 7);

    // Now simulate Do() producing an output map with the same data.
    DAS::DasPtr<IDasPortMap> output_map;
    ASSERT_EQ(CreateIDasPortMap(output_map.Put()), DAS_S_OK);
    DasReadOnlyString out_key{"result"};
    output_map->SetInt(out_key.Get(), 42);

    // Extract output back into the frame under a different node.
    PortFrame out_frame;
    ASSERT_EQ(
        ExtractOutputPortMap(output_map.Get(), kTargetNode, out_frame),
        DAS_S_OK);

    const auto* pv = out_frame.Find({kTargetNode, "result"});
    ASSERT_NE(pv, nullptr);
    ASSERT_TRUE(pv->IsInt());
    EXPECT_EQ(*pv->AsInt(), 42);
}

// ===========================================================================
// Empty bindings → empty map
// ===========================================================================

TEST(DoAdapterTest, BuildInputPortMap_EmptyBindings)
{
    PortFrame frame;
    frame.Set(kSourceNode, "out", PortValue(int64_t{1}));

    std::vector<Dto::PortBindingDto> empty_bindings;

    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(BuildInputPortMap(frame, empty_bindings, map.Put()), DAS_S_OK);
    ASSERT_NE(map.Get(), nullptr);
}

// ===========================================================================
// Empty output map → no entries in frame
// ===========================================================================

TEST(DoAdapterTest, ExtractOutputPortMap_EmptyMap)
{
    DAS::DasPtr<IDasPortMap> map;
    ASSERT_EQ(CreateIDasPortMap(map.Put()), DAS_S_OK);

    PortFrame frame;
    ASSERT_EQ(ExtractOutputPortMap(map.Get(), kTargetNode, frame), DAS_S_OK);
    EXPECT_TRUE(frame.Empty());
}
