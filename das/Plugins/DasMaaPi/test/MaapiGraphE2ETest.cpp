#include "../src/MaapiRunTaskComponent.h"
#include "FakeMaaApiBoundary.h"

#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphRuntime.h>
#include <das/Plugins/DasMaaPi/MaaPiErrorCodes.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentHost.Implements.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace
{
    using namespace Das;
    using namespace Das::Core::GraphRuntime;
    using namespace Das::Core::GraphRuntime::Dto;
    using namespace Das::Plugins::DasMaaPi;
    using namespace Das::Plugins::DasMaaPi::Test;

    using IDasTaskComponent = PluginInterface::IDasTaskComponent;
    using IDasTaskComponentHost = PluginInterface::IDasTaskComponentHost;

    // ---- Constants ----

    const std::string MAAPI_RUN_TASK_GUID =
        "69f20008-0000-4000-8000-000000000001";
    const std::string NODE_ID = "30000000-0000-0000-0000-000000000001";

    // ---- Fixture path helper ----

    std::filesystem::path FixturePath(std::string_view name)
    {
        return std::filesystem::current_path() / "DasMaaPi" / "test"
               / "fixtures" / std::filesystem::path{name};
    }

    // ---- Mock StopToken ----

    class MockStopToken final
        : public PluginInterface::DasStopTokenImplBase<MockStopToken>
    {
    public:
        DasResult DAS_STD_CALL StopRequested(bool* canStop) override
        {
            if (!canStop)
            {
                return DAS_E_INVALID_POINTER;
            }
            *canStop = false;
            return DAS_S_OK;
        }
    };

    // ---- ScopedBoundaryHook (re-declared locally) ----

    class ScopedBoundaryHook final
    {
    public:
        explicit ScopedBoundaryHook(FakeMaaApiBoundary& boundary)
        {
            SetMaaApiBoundaryForTest(&boundary);
        }

        ~ScopedBoundaryHook() { SetMaaApiBoundaryForTest(nullptr); }
    };

    // ---- MockTaskComponentHost ----
    // Creates real MaapiRunTaskComponent instances when the GUID matches.

    class MaapiMockHost final
        : public PluginInterface::DasTaskComponentHostImplBase<MaapiMockHost>
    {
    public:
        DasResult CreateTaskComponent(
            const DasGuid&      component_guid,
            IDasTaskComponent** pp_out_component) override
        {
            if (!pp_out_component)
            {
                return DAS_E_INVALID_POINTER;
            }

            auto guid_str = DasGuidToString(component_guid);

            if (guid_str.GetUtf8() != MAAPI_RUN_TASK_GUID)
            {
                *pp_out_component = nullptr;
                return DAS_E_NOT_FOUND;
            }

            auto* component = new MaapiRunTaskComponent();
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }
    };

    // ---- Plan builders ----

    GraphPortDefinitionDto MakePortDef(
        const std::string& port_id,
        const std::string& port_type = "string")
    {
        GraphPortDefinitionDto p;
        p.port_id = port_id;
        p.port_type = port_type;
        return p;
    }

    CompiledGraphPlanDto MakeSingleMaapiNodePlan(
        const std::string& settings_json)
    {
        CompiledGraphPlanDto plan;
        plan.source_fingerprint = "fp_e2e";
        plan.compiled_fingerprint = "compiled_fp_e2e";

        CompiledNodeSnapshotDto snap;
        snap.node_id = NODE_ID;
        snap.component_guid = MAAPI_RUN_TASK_GUID;

        auto parsed = Utils::ParseYyjsonFromString(settings_json);
        if (parsed)
        {
            snap.compiled_settings = std::move(*parsed);
        }

        snap.resolved_ports = {
            MakePortDef("graph_debug", "bool"),
            MakePortDef("graph_timeout", "int"),
            MakePortDef("completedTasks", "array<string>"),
            MakePortDef("stopped", "bool"),
            MakePortDef("diagnostics", "array<object>"),
        };

        plan.node_snapshots.push_back(std::move(snap));
        plan.execution_order.push_back(NODE_ID);
        return plan;
    }

    // ---- Settings builder ----

    std::string MakeNodeSettingsJson(
        const std::string& pi_path,
        const std::string& task_name,
        const std::string& options_json,
        const std::string& port_map_json)
    {
        // Build via yyjson so special characters in pi_path (e.g. Windows
        // backslashes from FixturePath().string()) are properly escaped.
        // Raw string concatenation would emit invalid JSON escapes such as
        // "\s"/"\D", which ParseYyjsonFromString rejects, leaving
        // compiled_settings null and causing Do() to return
        // DAS_E_OBJECT_NOT_INIT.
        auto root = Utils::MakeYyjsonObject();
        auto obj = root.as_object();
        (*obj)["piPath"] = yyjson::value(pi_path);
        (*obj)["taskName"] = yyjson::value(task_name);

        if (auto parsed = Utils::ParseYyjsonFromString(options_json); parsed)
        {
            (*obj)["options"] = Das::Utils::CloneYyjsonValue(*parsed);
        }
        else
        {
            (*obj)["options"] = Utils::MakeYyjsonObject();
        }

        if (auto parsed = Utils::ParseYyjsonFromString(port_map_json); parsed)
        {
            (*obj)["portMap"] = Das::Utils::CloneYyjsonValue(*parsed);
        }
        else
        {
            (*obj)["portMap"] = Utils::MakeYyjsonObject();
        }

        auto serialized = Utils::SerializeYyjsonValue(root);
        return serialized.value_or("{}");
    }

} // namespace

// ===================================================================
// Test 1: MaaPiNodeExecutesSuccessfullyInGraphRuntime
//
// Verifies the full E2E pipeline:
//   GraphRuntime::RunWithHost → Configure → ApplySettingsChange → Do()
//   → MaaPiExecutionEngine::Execute → PostTask (via FakeMaaApiBoundary)
//   → Output PortMap with completedTasks
// ===================================================================

TEST(MaapiGraphE2ETest, MaaPiNodeExecutesSuccessfullyInGraphRuntime)
{
    FakeMaaApiBoundary fake;
    ScopedBoundaryHook hook(fake);

    auto settings_json = MakeNodeSettingsJson(
        FixturePath("pi_e2e_interface.json").string(),
        "E2ETestTask",
        R"({"screenshot_mode":"case_a","enable_debug":true,"timeout_ms":5000})",
        R"({"graph_debug":"enable_debug","graph_timeout":"timeout_ms"})");

    auto plan = MakeSingleMaapiNodePlan(settings_json);

    auto host = MaapiMockHost::Make();
    auto token = PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    GraphRuntime rt;
    auto         hr = rt.RunWithHost(plan, "fp_e2e", token.Get(), host.Get());

    EXPECT_EQ(hr, DAS_S_OK);

    // Verify that PostTask was called with the StartE2E entry
    bool found_post_task = false;
    for (const auto& call : fake.calls)
    {
        if (call.find("PostTask:StartE2E:") != std::string::npos)
        {
            found_post_task = true;

            // Extract pipeline_override JSON from the call string.
            // Format is "PostTask:StartE2E:<pipeline_override>"
            auto        colon_pos = call.find(':', call.find("PostTask:") + 9);
            auto        second_colon = call.find(':', colon_pos + 1);
            std::string pipeline_override = call.substr(second_colon + 1);

            // Verify PortMap inputs were merged via port_map mapping.
            // portMap maps graph_debug → enable_debug, graph_timeout →
            // timeout_ms. Since there are no upstream nodes, no PortMap values
            // are injected. The engine should still have called PostTask
            // successfully.
            EXPECT_FALSE(pipeline_override.empty());
            break;
        }
    }
    EXPECT_TRUE(found_post_task);
    EXPECT_TRUE(fake.Contains("CreateTasker"));
}

// ===================================================================
// Test 2: MaaPiNodePropagatesExecutionFailure
//
// Verifies that when FakeMaaApiBoundary reports task failure,
// the error propagates through GraphRuntime as
// DAS_E_MAAPI_EXECUTION_FAILED (-10005).
// ===================================================================

TEST(MaapiGraphE2ETest, MaaPiNodePropagatesExecutionFailure)
{
    FakeMaaApiBoundary fake;
    fake.wait_status_by_entry["StartE2E"] = MaaTaskStatus::Failed;
    ScopedBoundaryHook hook(fake);

    auto settings_json = MakeNodeSettingsJson(
        FixturePath("pi_e2e_interface.json").string(),
        "E2ETestTask",
        R"({"screenshot_mode":"case_a","enable_debug":false,"timeout_ms":3000})",
        R"({"graph_debug":"enable_debug","graph_timeout":"timeout_ms"})");

    auto plan = MakeSingleMaapiNodePlan(settings_json);

    auto host = MaapiMockHost::Make();
    auto token = PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    GraphRuntime rt;
    auto         hr = rt.RunWithHost(plan, "fp_e2e", token.Get(), host.Get());

    EXPECT_EQ(hr, DAS_E_MAAPI_EXECUTION_FAILED);

    // Verify PostTask was still called (engine tried to execute)
    EXPECT_TRUE(fake.ContainsSubstring("PostTask:StartE2E:"));
}
