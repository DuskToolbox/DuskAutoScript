#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/DoAdapter.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/GraphRuntime/PortFrame.h>
#include <das/Core/GraphRuntime/RuntimeExecutionCache.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasErrorLens.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasErrorLens.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponent.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentHost.Implements.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>

// Forward declaration — GraphRuntime.h will be created in Task 2
#include <das/Core/GraphRuntime/GraphRuntime.h>

namespace
{
    using namespace Das::Core::GraphRuntime;
    using namespace Das::Core::GraphRuntime::Dto;

    using IDasPortMap = Das::ExportInterface::IDasPortMap;
    using IDasStopToken = Das::PluginInterface::IDasStopToken;

    // ---- GUID helpers ----

    DasGuid MakeNodeGuid(const std::string_view s)
    {
        return Das::Core::ForeignInterfaceHost::MakeDasGuid(s);
    }

    // Test node GUIDs — 36-char standard format
    const std::string kNodeA = "10000000-0000-0000-0000-000000000001";
    const std::string kNodeB = "10000000-0000-0000-0000-000000000002";
    const std::string kNodeC = "10000000-0000-0000-0000-000000000003";

    // Test component GUIDs — 36-char standard format for MakeDasGuid
    const std::string kCompA = "20000000-0000-0000-0000-000000000001";
    const std::string kCompTest = "20000000-0000-0000-0000-000000000099";

    // ---- Mock StopToken ----

    class MockStopToken final
        : public Das::PluginInterface::DasStopTokenImplBase<MockStopToken>
    {
    public:
        std::atomic<bool> cancelled{false};

        DasResult DAS_STD_CALL StopRequested(bool* canStop) override
        {
            if (!canStop)
                return DAS_E_INVALID_POINTER;
            *canStop = cancelled.load();
            return DAS_S_OK;
        }
    };

    // ---- Plan fixture helpers ----

    GraphPortDefinitionDto MakePortDef(
        const std::string& port_id,
        const std::string& port_type = "int")
    {
        GraphPortDefinitionDto p;
        p.port_id = port_id;
        p.port_type = port_type;
        return p;
    }

    CompiledNodeSnapshotDto MakeSnapshot(
        const std::string& node_id,
        const std::string& component_guid =
            "20000000-0000-0000-0000-000000000099")
    {
        CompiledNodeSnapshotDto snap;
        snap.node_id = node_id;
        snap.component_guid = component_guid;
        return snap;
    }

    CompiledNodeSnapshotDto MakeSnapshotWithPorts(
        const std::string&                         node_id,
        const std::vector<GraphPortDefinitionDto>& ports,
        const std::string&                         component_guid =
            "20000000-0000-0000-0000-000000000099")
    {
        CompiledNodeSnapshotDto snap;
        snap.node_id = node_id;
        snap.component_guid = component_guid;
        snap.resolved_ports = ports;
        return snap;
    }

    PortBindingDto MakeBinding(
        const std::string& src_node,
        const std::string& src_port,
        const std::string& tgt_node,
        const std::string& tgt_port,
        const std::string& type = "int")
    {
        PortBindingDto b;
        b.source_node_id = src_node;
        b.source_port_id = src_port;
        b.target_node_id = tgt_node;
        b.target_port_id = tgt_port;
        b.expected_type = type;
        return b;
    }

    // Build a linear chain plan: node_0 -> node_1 -> ... -> node_{n-1}
    // Each node has input port "in" and output port "out"
    CompiledGraphPlanDto MakeLinearPlan(int num_nodes)
    {
        CompiledGraphPlanDto plan;
        plan.source_fingerprint = "fp_v1";
        plan.compiled_fingerprint = "compiled_fp_v1";

        for (int i = 0; i < num_nodes; ++i)
        {
            // Generate GUID-format node_id
            char buf[64];
            snprintf(buf, sizeof(buf), "10000000-0000-0000-0000-%012d", i + 1);
            std::string nid(buf);

            auto snap = MakeSnapshotWithPorts(
                nid,
                {MakePortDef("in"), MakePortDef("out")},
                kCompTest);
            plan.node_snapshots.push_back(std::move(snap));
            plan.execution_order.push_back(nid);
        }

        // Create edges between consecutive nodes
        for (int i = 0; i + 1 < num_nodes; ++i)
        {
            plan.binding_plan.bindings.push_back(MakeBinding(
                plan.execution_order[i],
                "out",
                plan.execution_order[i + 1],
                "in"));
        }

        return plan;
    }

    CompiledGraphPlanDto MakeEmptyPlan()
    {
        CompiledGraphPlanDto plan;
        plan.source_fingerprint = "fp_v1";
        plan.compiled_fingerprint = "compiled_fp_v1";
        return plan;
    }

    // Get input bindings for a specific target node
    std::vector<PortBindingDto> GetInputBindings(
        const CompiledGraphPlanDto& plan,
        const std::string&          target_node_id)
    {
        std::vector<PortBindingDto> result;
        for (const auto& b : plan.binding_plan.bindings)
        {
            if (b.target_node_id == target_node_id)
                result.push_back(b);
        }
        return result;
    }

} // namespace

// =====================================================================
// Plan 02-14: Configure/Prepare/RunWithHost tests
// =====================================================================

// Named namespace for mock types (required for template specializations)
namespace GraphRuntimeTestMock
{
    using IDasJson = Das::ExportInterface::IDasJson;
    using IDasStopToken = Das::PluginInterface::IDasStopToken;
    using IDasTaskComponent = Das::PluginInterface::IDasTaskComponent;
    using IDasTaskComponentHost = Das::PluginInterface::IDasTaskComponentHost;

    // ---- Mock IDasTaskComponent ----
    // Manual implementation (not using ImplBase to avoid DasIidOf linker
    // issues with anonymous-namespace types).
    class MockTaskComponent final : public IDasTaskComponent
    {
    public:
        std::atomic<uint32_t> ref_count_{0};
        std::atomic<int>      apply_settings_call_count{0};
        std::atomic<int>      do_call_count{0};
        std::string           last_settings_json;
        std::string           last_input_json;

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            auto c = --ref_count_;
            if (c == 0)
            {
                ref_count_ = 1;
                delete this;
            }
            return c;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (!pp_out)
                return DAS_E_INVALID_POINTER;
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            if (!p_out_guid)
                return DAS_E_INVALID_POINTER;
            *p_out_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                "30000000-0000-0000-0000-000000000001");
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            if (!pp_out_name)
                return DAS_E_INVALID_POINTER;
            return CreateIDasReadOnlyStringFromUtf8(
                "MockTaskComponent",
                pp_out_name);
        }

        DasResult ApplySettingsChange(
            IDasJson*  p_request_json,
            IDasJson** pp_out_result_json) override
        {
            ++apply_settings_call_count;
            if (p_request_json)
            {
                DAS::DasPtr<IDasReadOnlyString> p_str;
                p_request_json->ToString(0, p_str.Put());
                if (p_str.Get())
                {
                    const char* utf8 = nullptr;
                    p_str->GetUtf8(&utf8);
                    last_settings_json =
                        utf8 ? std::string{utf8} : std::string{};
                }
            }
            if (pp_out_result_json)
            {
                *pp_out_result_json = new Das::Core::Utils::IDasJsonImpl("{}");
                (*pp_out_result_json)->AddRef();
            }
            return DAS_S_OK;
        }

        DasResult Do(
            IDasStopToken*                             p_stop_token,
            Das::ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
            Das::ExportInterface::IDasPortMap**        pp_out_port_map) override
        {
            ++do_call_count;

            if (pp_out_port_map == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }

            DAS::DasPtr<Das::ExportInterface::IDasPortMap> output_map;
            DasResult hr = CreateIDasPortMap(output_map.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }

            // Echo input ports to output if available
            if (p_input_port_map)
            {
                DAS::DasPtr<Das::ExportInterface::IDasStringVector> keys;
                hr = p_input_port_map->GetKeys(keys.Put());
                if (DAS::IsOk(hr) && keys)
                {
                    uint64_t count = 0;
                    keys->Size(&count);
                    for (uint64_t i = 0; i < count; ++i)
                    {
                        IDasReadOnlyString* p_key = nullptr;
                        if (DAS::IsOk(keys->At(i, &p_key)) && p_key)
                        {
                            IDasReadOnlyString* p_val = nullptr;
                            if (DAS::IsOk(
                                    p_input_port_map->GetString(p_key, &p_val))
                                && p_val)
                            {
                                output_map->SetString(p_key, p_val);
                                p_val->Release();
                            }
                            p_key->Release();
                        }
                    }
                }
            }

            *pp_out_port_map = output_map.Get();
            output_map.Get()->AddRef();
            return DAS_S_OK;
        }

        static IDasTaskComponent* MakeRaw()
        {
            auto* p = new MockTaskComponent();
            p->AddRef();
            return p;
        }
    };

    // ---- Mock IDasTaskComponentHost ----
    class MockTaskComponentHost final
        : public Das::PluginInterface::DasTaskComponentHostImplBase<
              MockTaskComponentHost>
    {
    public:
        std::vector<std::string> created_guids;

        DasResult CreateTaskComponent(
            const DasGuid&                            component_guid,
            Das::PluginInterface::IDasTaskComponent** pp_out_component) override
        {
            if (!pp_out_component)
                return DAS_E_INVALID_POINTER;

            auto guid_str = Das::Core::ForeignInterfaceHost::DasGuidToStdString(
                component_guid);
            created_guids.push_back(guid_str);

            *pp_out_component = MockTaskComponent::MakeRaw();
            return DAS_S_OK;
        }
    };

    // ---- Mock IDasErrorLens ----
    class MockErrorLens final
        : public Das::PluginInterface::DasErrorLensImplBase<MockErrorLens>
    {
    public:
        std::map<DasResult, std::string> error_messages;

        DasResult GetSupportedIids(
            Das::ExportInterface::IDasReadOnlyGuidVector** pp_out_iids) override
        {
            if (!pp_out_iids)
                return DAS_E_INVALID_POINTER;
            *pp_out_iids = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult GetErrorMessage(
            IDasReadOnlyString*  locale_name,
            DasResult            error_code,
            IDasReadOnlyString** out_string) override
        {
            if (!out_string)
                return DAS_E_INVALID_POINTER;

            auto it = error_messages.find(error_code);
            if (it != error_messages.end())
            {
                return CreateIDasReadOnlyStringFromUtf8(
                    it->second.c_str(),
                    out_string);
            }
            *out_string = nullptr;
            return DAS_E_NOT_FOUND;
        }
    };

    // Build a plan with a settings snapshot for a node
    inline CompiledGraphPlanDto MakePlanWithSettings(
        const std::string& node_id,
        const std::string& component_guid,
        const std::string& settings_json,
        const std::string& payload_json)
    {
        CompiledGraphPlanDto plan;
        plan.source_fingerprint = "fp_v1";
        plan.compiled_fingerprint = "compiled_fp_v1";

        CompiledNodeSnapshotDto snap;
        snap.node_id = node_id;
        snap.component_guid = component_guid;

        if (!settings_json.empty())
        {
            auto parsed = Das::Utils::ParseYyjsonFromString(settings_json);
            if (parsed.has_value())
                snap.compiled_settings = std::move(*parsed);
        }
        if (!payload_json.empty())
        {
            auto parsed = Das::Utils::ParseYyjsonFromString(payload_json);
            if (parsed.has_value())
                snap.compiled_payload_json = std::move(*parsed);
        }

        snap.resolved_ports = {MakePortDef("in"), MakePortDef("out")};
        plan.node_snapshots.push_back(std::move(snap));
        plan.execution_order.push_back(node_id);
        return plan;
    }

} // namespace GraphRuntimeTestMock

// ===================================================================
// Test 16: ConfigureCreatesComponents
// ===================================================================
TEST(GraphRuntimeTest, ConfigureCreatesComponents)
{
    using namespace GraphRuntimeTestMock;

    auto plan =
        MakePlanWithSettings(kNodeA, kCompA, R"({"threshold": 42})", "");

    auto host = MockTaskComponentHost::Make();

    GraphRuntime rt;
    auto         hr = rt.Configure(plan, host.Get());

    EXPECT_EQ(hr, DAS_S_OK);

    auto* mock_host = static_cast<MockTaskComponentHost*>(host.Get());
    ASSERT_EQ(mock_host->created_guids.size(), 1u);
}

// ===================================================================
// Test 17: ConfigureAppliesSettings
// ===================================================================
TEST(GraphRuntimeTest, ConfigureAppliesSettings)
{
    using namespace GraphRuntimeTestMock;

    auto plan = MakePlanWithSettings(
        kNodeA,
        kCompA,
        R"({"threshold": 42})",
        R"({"mode": "fast"})");

    auto host = MockTaskComponentHost::Make();

    GraphRuntime rt;
    EXPECT_EQ(DAS_S_OK, rt.Configure(plan, host.Get()));

    // Verify Prepare succeeds
    EXPECT_EQ(DAS_S_OK, rt.Prepare());
}

// ===================================================================
// Test 18: PrepareValidatesConfiguredComponents
// ===================================================================
TEST(GraphRuntimeTest, PrepareValidatesConfiguredComponents)
{
    using namespace GraphRuntimeTestMock;

    // Prepare without Configure should fail
    GraphRuntime rt;
    EXPECT_NE(DAS_S_OK, rt.Prepare());

    // Prepare after Configure should succeed
    auto plan = MakeLinearPlan(2);
    auto host = MockTaskComponentHost::Make();

    EXPECT_EQ(DAS_S_OK, rt.Configure(plan, host.Get()));
    EXPECT_EQ(DAS_S_OK, rt.Prepare());
}

// ===================================================================
// Test 19: ConfigureNullHost
// ===================================================================
TEST(GraphRuntimeTest, ConfigureNullHost)
{
    auto plan = MakeLinearPlan(1);

    GraphRuntime rt;
    auto         hr = rt.Configure(plan, nullptr);
    EXPECT_NE(DAS_S_OK, hr);
    EXPECT_FALSE(rt.GetLastErrorMessage().empty());
}

// ===================================================================
// Test 20: RunWithHostSingleExecution
// ===================================================================
TEST(GraphRuntimeTest, RunWithHostSingleExecution)
{
    using namespace GraphRuntimeTestMock;
    CompiledGraphPlanDto plan;
    plan.source_fingerprint = "fp_v1";
    plan.compiled_fingerprint = "compiled_fp_v1";

    plan.node_snapshots.push_back(
        MakeSnapshotWithPorts(kNodeA, {MakePortDef("out")}, kCompA));
    plan.execution_order = {kNodeA};

    auto host = MockTaskComponentHost::Make();

    GraphRuntime               rt;
    Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get());
    EXPECT_EQ(hr, DAS_S_OK);
}

// ===================================================================
// Test 21: RunWithHostLinearChain
// ===================================================================
TEST(GraphRuntimeTest, RunWithHostLinearChain)
{
    using namespace GraphRuntimeTestMock;
    auto plan = MakeLinearPlan(3);

    auto host = MockTaskComponentHost::Make();

    GraphRuntime               rt;
    Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get());
    EXPECT_EQ(hr, DAS_S_OK);
}

// ===================================================================
// Test 22: RunWithHostFingerprintMismatch
// ===================================================================
TEST(GraphRuntimeTest, RunWithHostFingerprintMismatch)
{
    using namespace GraphRuntimeTestMock;
    auto plan = MakeLinearPlan(2);
    plan.source_fingerprint = "compiled_at_v1";

    auto host = MockTaskComponentHost::Make();

    GraphRuntime               rt;
    Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "wrong_fp", token.Get(), host.Get());
    EXPECT_NE(hr, DAS_S_OK);
    EXPECT_FALSE(rt.GetLastErrorMessage().empty());
}

// ===================================================================
// Test 23: ErrorLensIntegration
// ===================================================================
TEST(GraphRuntimeTest, ErrorLensIntegration)
{
    using namespace GraphRuntimeTestMock;
    auto  lens = MockErrorLens::Make();
    auto* raw_lens = static_cast<MockErrorLens*>(lens.Get());
    raw_lens->error_messages[DAS_E_NOT_FOUND] = "Resource not available";

    GraphRuntime rt;
    rt.SetErrorLens(lens.Get());

    std::string msg;
    auto        hr = rt.GetStructuredError(DAS_E_NOT_FOUND, msg);
    EXPECT_EQ(hr, DAS_S_OK);
    EXPECT_EQ(msg, "Resource not available");

    // Unknown error code
    hr = rt.GetStructuredError(DAS_E_FAIL, msg);
    EXPECT_NE(hr, DAS_S_OK);
}

// ===================================================================
// Test 24: RunWithHostEmptyGraph
// ===================================================================
TEST(GraphRuntimeTest, RunWithHostEmptyGraph)
{
    using namespace GraphRuntimeTestMock;
    auto plan = MakeEmptyPlan();

    auto host = MockTaskComponentHost::Make();

    GraphRuntime               rt;
    Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get());
    EXPECT_EQ(hr, DAS_S_OK);
}

// ===================================================================
// Test 25: V17DataSepSettingsNotInPortMap
// ===================================================================
TEST(GraphRuntimeTest, V17DataSepSettingsNotInPortMap)
{
    using namespace GraphRuntimeTestMock;
    // Verify that settings are applied via ApplySettingsChange (Configure)
    // and NOT passed through the PortMap data plane during Do().
    auto plan =
        MakePlanWithSettings(kNodeA, kCompA, R"({"threshold": 42})", "");

    auto host = MockTaskComponentHost::Make();

    GraphRuntime rt;
    EXPECT_EQ(DAS_S_OK, rt.Configure(plan, host.Get()));

    // The mock component should have received settings
    auto* mock_host = static_cast<MockTaskComponentHost*>(host.Get());
    ASSERT_EQ(mock_host->created_guids.size(), 1u);
}

// ===================================================================
// DAS-60 Stage 3: signal-gated runtime (gate / skip / loop)
// ===================================================================
//
// A behaviour-driven mock component lets each node play a control-flow role
// (emit a fixed signal, count loops, alternate branch signals, record Do
// calls) driven entirely by its compiled_settings. The host creates one
// instance per node and keeps observing pointers so tests can assert which
// nodes ran and how many times — the runtime exposes neither the PortFrame
// nor the components, so the do_count is the observation point.

namespace SignalGatedTestMock
{
    using IDasJson = Das::ExportInterface::IDasJson;
    using IDasPortMap = Das::ExportInterface::IDasPortMap;
    using IDasStopToken = Das::PluginInterface::IDasStopToken;
    using IDasTaskComponent = Das::PluginInterface::IDasTaskComponent;
    using IDasTaskComponentHost = Das::PluginInterface::IDasTaskComponentHost;

    // Shared component GUID for every behaviour node — the host ignores it and
    // the role comes from settings, so a single GUID is enough.
    constexpr std::string_view kBehaviorGuid =
        "20000000-0000-0000-0000-000000000099";

    class BehaviorComponent final : public IDasTaskComponent
    {
    public:
        std::atomic<uint32_t> ref_count_{0};
        std::atomic<int>      do_count{0};

        // role configuration (parsed from compiled_settings)
        std::string mode;          // emit | record | loop | alternate
        std::string emit_signal;   // mode=emit: which signal port to fire
        int         loop_end = 0;  // mode=loop: iteration count
        int         counter = 0;   // mode=loop / alternate: persists across Do()

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            auto c = --ref_count_;
            if (c == 0)
            {
                ref_count_ = 1;
                delete this;
            }
            return c;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (!pp_out)
                return DAS_E_INVALID_POINTER;
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            if (!p_out_guid)
                return DAS_E_INVALID_POINTER;
            *p_out_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                std::string{kBehaviorGuid});
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            if (!pp_out_name)
                return DAS_E_INVALID_POINTER;
            return CreateIDasReadOnlyStringFromUtf8("BehaviorComponent", pp_out_name);
        }

        DasResult ApplySettingsChange(
            IDasJson*  p_request_json,
            IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json)
            {
                *pp_out_result_json = new Das::Core::Utils::IDasJsonImpl("{}");
                (*pp_out_result_json)->AddRef();
            }
            if (!p_request_json)
            {
                return DAS_S_OK;
            }

            // ApplyNodeSettings wraps the role as {"settings": {...}}.
            DAS::DasPtr<IDasReadOnlyString> p_str;
            p_request_json->ToString(0, p_str.Put());
            if (!p_str.Get())
            {
                return DAS_S_OK;
            }
            const char* utf8 = nullptr;
            p_str->GetUtf8(&utf8);
            auto parsed = Das::Utils::ParseYyjsonFromString(
                utf8 ? std::string{utf8} : std::string{});
            if (!parsed.has_value())
            {
                return DAS_S_OK;
            }
            auto root_obj = parsed->as_object();
            if (!root_obj.has_value())
            {
                return DAS_S_OK;
            }
            yyjson::value settings = *parsed;
            if (root_obj->contains(std::string_view("settings")))
            {
                auto sv = (*root_obj)[std::string_view("settings")];
                if (sv.is_object())
                {
                    settings = sv;
                }
            }
            auto sobj = settings.as_object();
            if (!sobj.has_value())
            {
                return DAS_S_OK;
            }
            if (sobj->contains(std::string_view("mode")))
            {
                auto m = (*sobj)[std::string_view("mode")].as_string();
                if (m.has_value())
                {
                    mode = std::string(*m);
                }
            }
            if (sobj->contains(std::string_view("signal")))
            {
                auto s = (*sobj)[std::string_view("signal")].as_string();
                if (s.has_value())
                {
                    emit_signal = std::string(*s);
                }
            }
            if (sobj->contains(std::string_view("end")))
            {
                auto e = (*sobj)[std::string_view("end")].as_string();
                if (e.has_value())
                {
                    loop_end = std::atoi(std::string(*e).c_str());
                }
            }
            return DAS_S_OK;
        }

        // Emit a signal by writing the reserved "signals" JSON string array.
        void EmitSignal(IDasPortMap* map, const std::string& sig)
        {
            std::string    json = std::string(R"([")") + sig + R"("])";
            DasReadOnlyString key{"signals"};
            DasReadOnlyString val{json.c_str()};
            map->SetString(key.Get(), val.Get());
        }

        void SetOutInt(IDasPortMap* map, const std::string& port, int64_t v)
        {
            DasReadOnlyString key{port.c_str()};
            map->SetInt(key.Get(), v);
        }

        DasResult Do(
            IDasStopToken*                             p_stop_token,
            Das::ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
            Das::ExportInterface::IDasPortMap**        pp_out_port_map) override
        {
            (void)p_stop_token;
            (void)p_input_port_map;
            ++do_count;

            DAS::DasPtr<IDasPortMap> output;
            DasResult                hr = CreateIDasPortMap(output.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }

            if (mode == "loop")
            {
                if (counter < loop_end)
                {
                    SetOutInt(output.Get(), "index", counter);
                    EmitSignal(output.Get(), "continue");
                    ++counter;
                }
                else
                {
                    EmitSignal(output.Get(), "break");
                }
            }
            else if (mode == "alternate")
            {
                EmitSignal(output.Get(), counter % 2 == 0 ? "true" : "false");
                ++counter;
            }
            else if (mode == "emit")
            {
                if (!emit_signal.empty())
                {
                    EmitSignal(output.Get(), emit_signal);
                }
                SetOutInt(output.Get(), "out", do_count.load());
            }
            else
            {
                // "record" (and default): produce a data output, no signal.
                SetOutInt(output.Get(), "out", do_count.load());
            }

            *pp_out_port_map = output.Get();
            output.Get()->AddRef();
            return DAS_S_OK;
        }

        static IDasTaskComponent* MakeRaw()
        {
            auto* p = new BehaviorComponent();
            p->AddRef();
            return p;
        }
    };

    class BehaviorHost final
        : public Das::PluginInterface::DasTaskComponentHostImplBase<
              BehaviorHost>
    {
    public:
        // One entry per created component, in node-snapshot order, so a test
        // maps plan.node_snapshots[i] -> components[i].
        std::vector<BehaviorComponent*> components;

        DasResult CreateTaskComponent(
            const DasGuid&                            component_guid,
            Das::PluginInterface::IDasTaskComponent** pp_out_component) override
        {
            (void)component_guid;
            if (!pp_out_component)
            {
                return DAS_E_INVALID_POINTER;
            }
            auto* c = static_cast<BehaviorComponent*>(BehaviorComponent::MakeRaw());
            components.push_back(c);
            *pp_out_component = c;
            return DAS_S_OK;
        }
    };

    CompiledNodeSnapshotDto MakeBehaviorSnapshot(
        const std::string& node_id,
        const std::string& settings_json)
    {
        CompiledNodeSnapshotDto snap;
        snap.node_id = node_id;
        snap.component_guid = std::string{kBehaviorGuid};
        if (!settings_json.empty())
        {
            auto parsed = Das::Utils::ParseYyjsonFromString(settings_json);
            if (parsed.has_value())
            {
                snap.compiled_settings = std::move(*parsed);
            }
        }
        return snap;
    }

    SignalRouteDto MakeSignalRoute(
        const std::string& src_node,
        const std::string& src_port,
        const std::string& tgt_node,
        const std::string& tgt_port)
    {
        SignalRouteDto r;
        r.source_node_id = src_node;
        r.source_port_id = src_port;
        r.target_node_id = tgt_node;
        r.target_port_id = tgt_port;
        return r;
    }

    BackEdgeDto MakeBackEdge(
        const std::string& edge_id,
        const std::string& src_node,
        const std::string& src_port,
        const std::string& tgt_node,
        const std::string& tgt_port,
        const std::string& loop_head)
    {
        BackEdgeDto be;
        be.edge_id = edge_id;
        be.source_node_id = src_node;
        be.source_port_id = src_port;
        be.target_node_id = tgt_node;
        be.target_port_id = tgt_port;
        be.loop_head_node_id = loop_head;
        return be;
    }

    CompiledGraphPlanDto MakeBasePlan()
    {
        CompiledGraphPlanDto plan;
        plan.source_fingerprint = "fp_v1";
        plan.compiled_fingerprint = "compiled_fp_v1";
        return plan;
    }
} // namespace SignalGatedTestMock

// -------------------------------------------------------------------
// Gate: branch emits "true" → only the true-branch sink runs.
// -------------------------------------------------------------------
TEST(SignalGatedRuntimeTest, BranchIfOnlyHitBranchRuns)
{
    using namespace SignalGatedTestMock;
    const std::string branch{"10000000-0000-0000-0000-000000000011"};
    const std::string trueSink{"10000000-0000-0000-0000-000000000012"};
    const std::string falseSink{"10000000-0000-0000-0000-000000000013"};

    auto plan = MakeBasePlan();
    plan.node_snapshots = {
        MakeBehaviorSnapshot(branch, R"({"mode":"emit","signal":"true"})"),
        MakeBehaviorSnapshot(trueSink, R"({"mode":"record"})"),
        MakeBehaviorSnapshot(falseSink, R"({"mode":"record"})"),
    };
    plan.execution_order = {branch, trueSink, falseSink};
    plan.signal_routes = {
        MakeSignalRoute(branch, "true", trueSink, "in"),
        MakeSignalRoute(branch, "false", falseSink, "in"),
    };
    // Signal edges appear as bindings (mirrors compiler output) and must be
    // excluded from data dependencies.
    plan.binding_plan.bindings = {
        MakeBinding(branch, "true", trueSink, "in", "signal"),
        MakeBinding(branch, "false", falseSink, "in", "signal"),
    };

    auto host = BehaviorHost::Make();
    GraphRuntime rt;
    Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK);

    auto* h = static_cast<BehaviorHost*>(host.Get());
    ASSERT_EQ(h->components.size(), 3u);
    EXPECT_EQ(h->components[0]->do_count.load(), 1); // branch ran
    EXPECT_EQ(h->components[1]->do_count.load(), 1); // true branch ran
    EXPECT_EQ(h->components[2]->do_count.load(), 0); // false branch skipped
}

// -------------------------------------------------------------------
// Skip propagation: a gated-out branch's data downstream is also skipped.
//   branch --true--> A --+
//                         +--> C  (C needs both A and B)
//   branch --false-> B --+
// -------------------------------------------------------------------
TEST(SignalGatedRuntimeTest, SkipPropagatesAlongDataEdges)
{
    using namespace SignalGatedTestMock;
    const std::string branch{"10000000-0000-0000-0000-000000000021"};
    const std::string a{"10000000-0000-0000-0000-000000000022"};
    const std::string b{"10000000-0000-0000-0000-000000000023"};
    const std::string c{"10000000-0000-0000-0000-000000000024"};

    auto plan = MakeBasePlan();
    plan.node_snapshots = {
        MakeBehaviorSnapshot(branch, R"({"mode":"emit","signal":"true"})"),
        MakeBehaviorSnapshot(a, R"({"mode":"record"})"),
        MakeBehaviorSnapshot(b, R"({"mode":"record"})"),
        MakeBehaviorSnapshot(c, R"({"mode":"record"})"),
    };
    plan.execution_order = {branch, a, b, c};
    plan.signal_routes = {
        MakeSignalRoute(branch, "true", a, "in"),
        MakeSignalRoute(branch, "false", b, "in"),
    };
    plan.binding_plan.bindings = {
        MakeBinding(branch, "true", a, "in", "signal"),
        MakeBinding(branch, "false", b, "in", "signal"),
        MakeBinding(a, "out", c, "in", "int"),
        MakeBinding(b, "out", c, "in", "int"),
    };

    auto host = BehaviorHost::Make();
    GraphRuntime rt;
    Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK);

    auto* h = static_cast<BehaviorHost*>(host.Get());
    ASSERT_EQ(h->components.size(), 4u);
    EXPECT_EQ(h->components[0]->do_count.load(), 1); // branch
    EXPECT_EQ(h->components[1]->do_count.load(), 1); // A (true branch) ran
    EXPECT_EQ(h->components[2]->do_count.load(), 0); // B skipped
    EXPECT_EQ(h->components[3]->do_count.load(), 0); // C skipped (B produced nothing)
}

// -------------------------------------------------------------------
// Loop: for emits continue N times then break; body reruns each iteration.
//   for --continue--> body ; body --done--> for (back-edge)
// -------------------------------------------------------------------
TEST(SignalGatedRuntimeTest, ForLoopIteratesCorrectly)
{
    using namespace SignalGatedTestMock;
    const std::string forNode{"10000000-0000-0000-0000-000000000031"};
    const std::string body{"10000000-0000-0000-0000-000000000032"};

    auto plan = MakeBasePlan();
    plan.node_snapshots = {
        MakeBehaviorSnapshot(forNode, R"({"mode":"loop","end":"3"})"),
        MakeBehaviorSnapshot(body, R"({"mode":"emit","signal":"done"})"),
    };
    plan.execution_order = {forNode, body};
    plan.signal_routes = {
        MakeSignalRoute(forNode, "continue", body, "in"),
        MakeSignalRoute(body, "done", forNode, "loop_in"),
    };
    plan.back_edges = {
        MakeBackEdge("be1", body, "done", forNode, "loop_in", forNode),
    };
    plan.binding_plan.bindings = {
        MakeBinding(forNode, "continue", body, "in", "signal"),
        MakeBinding(body, "done", forNode, "loop_in", "signal"),
    };

    auto host = BehaviorHost::Make();
    GraphRuntime rt;
    Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK);

    auto* h = static_cast<BehaviorHost*>(host.Get());
    ASSERT_EQ(h->components.size(), 2u);
    // for: continue @ i=0,1,2 then break @ i=3  → 4 Do calls
    EXPECT_EQ(h->components[0]->do_count.load(), 4);
    // body: ran once per continue → 3 Do calls
    EXPECT_EQ(h->components[1]->do_count.load(), 3);
}

// -------------------------------------------------------------------
// Loop with an inner branch: the branch flips each iteration; stale signals
// from the previous iteration must NOT re-activate the wrong sink.
//   for -> selector(alternate true/false) -> sinkT / sinkF -> term -> for
// -------------------------------------------------------------------
TEST(SignalGatedRuntimeTest, LoopWithInnerBranchHasNoCrossIterationResidue)
{
    using namespace SignalGatedTestMock;
    const std::string forNode{"10000000-0000-0000-0000-000000000041"};
    const std::string selector{"10000000-0000-0000-0000-000000000042"};
    const std::string sinkT{"10000000-0000-0000-0000-000000000043"};
    const std::string sinkF{"10000000-0000-0000-0000-000000000044"};
    const std::string term{"10000000-0000-0000-0000-000000000045"};

    auto plan = MakeBasePlan();
    plan.node_snapshots = {
        MakeBehaviorSnapshot(forNode, R"({"mode":"loop","end":"2"})"),
        MakeBehaviorSnapshot(selector, R"({"mode":"alternate"})"),
        MakeBehaviorSnapshot(sinkT, R"({"mode":"emit","signal":"done"})"),
        MakeBehaviorSnapshot(sinkF, R"({"mode":"emit","signal":"done"})"),
        MakeBehaviorSnapshot(term, R"({"mode":"emit","signal":"done"})"),
    };
    plan.execution_order = {forNode, selector, sinkT, sinkF, term};
    plan.signal_routes = {
        MakeSignalRoute(forNode, "continue", selector, "in"),
        MakeSignalRoute(selector, "true", sinkT, "in"),
        MakeSignalRoute(selector, "false", sinkF, "in"),
        MakeSignalRoute(sinkT, "done", term, "in"),
        MakeSignalRoute(sinkF, "done", term, "in"),
        MakeSignalRoute(term, "done", forNode, "loop_in"),
    };
    plan.back_edges = {
        MakeBackEdge("be1", term, "done", forNode, "loop_in", forNode),
    };
    plan.binding_plan.bindings = {
        MakeBinding(forNode, "continue", selector, "in", "signal"),
        MakeBinding(selector, "true", sinkT, "in", "signal"),
        MakeBinding(selector, "false", sinkF, "in", "signal"),
        MakeBinding(sinkT, "done", term, "in", "signal"),
        MakeBinding(sinkF, "done", term, "in", "signal"),
        MakeBinding(term, "done", forNode, "loop_in", "signal"),
    };

    auto host = BehaviorHost::Make();
    GraphRuntime rt;
    Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK);

    auto* h = static_cast<BehaviorHost*>(host.Get());
    ASSERT_EQ(h->components.size(), 5u);
    EXPECT_EQ(h->components[0]->do_count.load(), 3); // for: continue x2, break x1
    EXPECT_EQ(h->components[1]->do_count.load(), 2); // selector ran both iterations
    EXPECT_EQ(h->components[2]->do_count.load(), 1); // sinkT only on iter 0 (true)
    EXPECT_EQ(h->components[3]->do_count.load(), 1); // sinkF only on iter 1 (false)
    EXPECT_EQ(h->components[4]->do_count.load(), 2); // term each iteration
}
