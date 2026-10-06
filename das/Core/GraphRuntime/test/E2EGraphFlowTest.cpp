#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>
#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/CompiledPlanFingerprint.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/DoAdapter.h>
#include <das/Core/GraphRuntime/GraphCompiler.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/GraphRuntime/GraphRuntime.h>
#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/Core/GraphRuntime/PortFrame.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/TaskComponentPorts.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasStringVector.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/header/IDasStringVector.generated.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasImage.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentCreator.Implements.hpp>
#include <gtest/gtest.h>

#include "FlowControlErrorCodes.h"
#include "FlowControlTaskComponents.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace
{
    using namespace Das::Core::GraphRuntime;
    using namespace Das::Core::GraphRuntime::Dto;
    using namespace Das::Core::ForeignInterfaceHost;

    using IDasPortMap = Das::ExportInterface::IDasPortMap;
    using IDasReadOnlyPortMap = Das::ExportInterface::IDasReadOnlyPortMap;
    using IDasStopToken = Das::PluginInterface::IDasStopToken;

    // ---- GUID helpers ----

    const std::string COMP_PASS_THROUGH =
        "A0000000-0000-0000-0000-000000000001";
    const std::string COMP_DOUBLE = "A0000000-0000-0000-0000-000000000002";
    const std::string COMP_SUM = "A0000000-0000-0000-0000-000000000003";
    const std::string COMP_FAIL = "A0000000-0000-0000-0000-000000000004";

    const std::string NODE_A = "10000000-0000-0000-0000-000000000001";
    const std::string NODE_B = "10000000-0000-0000-0000-000000000002";
    const std::string NODE_C = "10000000-0000-0000-0000-000000000003";
    const std::string NODE_D = "10000000-0000-0000-0000-000000000004";

    // ---- Mock StopToken ----

    class MockStopToken final
        : public Das::PluginInterface::DasStopTokenImplBase<MockStopToken>
    {
    public:
        std::atomic<bool> cancelled{false};

        DasResult DAS_STD_CALL StopRequested(bool* canStop) override
        {
            if (!canStop)
            {
                return DAS_E_INVALID_POINTER;
            }
            *canStop = cancelled.load();
            return DAS_S_OK;
        }
    };

    // ---- Stub Factory Manager for GraphCompiler ----

    class StubFactoryManager : public TaskComponentFactoryManager
    {
    public:
        void AddDefinition(
            const std::string&   component_guid_str,
            const yyjson::value& definition)
        {
            const DasGuid guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                component_guid_str);
            definitions_.push_back(
                {guid, guid, guid, Das::Utils::CloneYyjsonValue(definition)});
        }

        std::vector<TaskComponentDefinitionInfo> EnumerateDefinitions()
            const override
        {
            return definitions_;
        }

    private:
        std::vector<TaskComponentDefinitionInfo> definitions_;
    };

    // ---- Document helpers ----

    ComponentRefDto MakeComponentRef(const std::string& component_guid)
    {
        ComponentRefDto ref;
        ref.kind = "componentRef";
        ref.component_guid = component_guid;
        return ref;
    }

    GraphNodeDto MakeComponentNode(
        const std::string& node_id,
        const std::string& component_guid)
    {
        GraphNodeDto node;
        node.node_id = node_id;
        node.target.target_kind = "componentRef";
        node.target.component_ref = MakeComponentRef(component_guid);
        return node;
    }

    GraphEdgeDto MakeEdge(
        const std::string& edge_id,
        const std::string& src_node,
        const std::string& src_port,
        const std::string& tgt_node,
        const std::string& tgt_port)
    {
        GraphEdgeDto edge;
        edge.edge_id = edge_id;
        edge.source_node_id = src_node;
        edge.source_port_id = src_port;
        edge.target_node_id = tgt_node;
        edge.target_port_id = tgt_port;
        return edge;
    }

    GraphPortDefinitionDto MakePortDef(
        const std::string& port_id,
        const std::string& port_type = "int")
    {
        GraphPortDefinitionDto p;
        p.port_id = port_id;
        p.port_type = port_type;
        return p;
    }

    yyjson::value MakeDefinition(
        const std::vector<std::pair<std::string, std::string>>& inputs,
        const std::vector<std::pair<std::string, std::string>>& outputs)
    {
        std::string json = R"({"inputs":[)";
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            if (i > 0)
            {
                json += ",";
            }
            json += R"({"id":")" + inputs[i].first + R"(","type":")"
                    + inputs[i].second + R"("})";
        }
        json += R"(],"outputs":[)";
        for (size_t i = 0; i < outputs.size(); ++i)
        {
            if (i > 0)
            {
                json += ",";
            }
            json += R"({"id":")" + outputs[i].first + R"(","type":")"
                    + outputs[i].second + R"("})";
        }
        json += R"(]})";
        auto result = Das::Utils::ParseYyjsonFromString(json);
        return result ? std::move(*result) : yyjson::value{};
    }

    // ---- Compiled plan helpers ----

    CompiledNodeSnapshotDto MakeSnapshot(
        const std::string&                         node_id,
        const std::string&                         component_guid,
        const std::vector<GraphPortDefinitionDto>& ports = {})
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
    CompiledGraphPlanDto MakeLinearPlan(
        int                num_nodes,
        const std::string& component_guid = COMP_PASS_THROUGH)
    {
        CompiledGraphPlanDto plan;
        plan.source_fingerprint = "fp_v1";

        for (int i = 0; i < num_nodes; ++i)
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "10000000-0000-0000-0000-%012d", i + 1);
            const std::string nid(buf);

            auto snap = MakeSnapshot(
                nid,
                component_guid,
                {MakePortDef("value_in"), MakePortDef("value_out")});
            plan.node_snapshots.push_back(std::move(snap));
            plan.execution_order.push_back(nid);
        }

        for (int i = 0; i + 1 < num_nodes; ++i)
        {
            plan.binding_plan.bindings.push_back(MakeBinding(
                plan.execution_order[i],
                "value_out",
                plan.execution_order[i + 1],
                "value_in"));
        }

        return plan;
    }

    // ---- PortMap value helpers ----

    void PortMapSetInt(IDasPortMap* map, const std::string& key, int64_t value)
    {
        auto port_id = DasReadOnlyString::FromUtf8(key.c_str(), nullptr);
        map->SetInt(port_id.Get(), value);
    }

    bool PortMapHas(IDasReadOnlyPortMap* map, const std::string& key)
    {
        auto port_id = DasReadOnlyString::FromUtf8(key.c_str(), nullptr);
        bool has = false;
        map->Has(port_id.Get(), &has);
        return has;
    }

    int64_t PortMapGetInt(IDasReadOnlyPortMap* map, const std::string& key)
    {
        auto    port_id = DasReadOnlyString::FromUtf8(key.c_str(), nullptr);
        int64_t value = 0;
        map->GetInt(port_id.Get(), &value);
        return value;
    }

    Das::ExportInterface::DasVariantType PortMapGetType(
        IDasReadOnlyPortMap* map,
        const std::string&   key)
    {
        auto port_id = DasReadOnlyString::FromUtf8(key.c_str(), nullptr);
        Das::ExportInterface::DasVariantType type =
            Das::ExportInterface::DAS_VARIANT_TYPE_NULL;
        map->GetType(port_id.Get(), &type);
        return type;
    }

    bool PortMapGetDouble(
        IDasReadOnlyPortMap* map,
        const std::string&   key,
        double&              value)
    {
        auto port_id = DasReadOnlyString::FromUtf8(key.c_str(), nullptr);
        return DAS::IsOk(map->GetFloat(port_id.Get(), &value));
    }

    bool PortMapGetBool(
        IDasReadOnlyPortMap* map,
        const std::string&   key,
        bool&                value)
    {
        auto port_id = DasReadOnlyString::FromUtf8(key.c_str(), nullptr);
        return DAS::IsOk(map->GetBool(port_id.Get(), &value));
    }

    // 读字符串口（缺港/类型不符返回空串——常量/拼接链的输出观测通道）。
    std::string PortMapGetString(
        IDasReadOnlyPortMap* map,
        const std::string&   key)
    {
        auto port_id = DasReadOnlyString::FromUtf8(key.c_str(), nullptr);
        IDasReadOnlyString* p_str = nullptr;
        if (DAS::IsFailed(map->GetString(port_id.Get(), &p_str))
            || p_str == nullptr)
        {
            return {};
        }
        const char* utf8 = nullptr;
        p_str->GetUtf8(&utf8);
        std::string value(utf8 != nullptr ? utf8 : "");
        p_str->Release();
        return value;
    }

    // 诊断 kind 在 plan.diagnostics 里序列化为枚举整数的字符串形式。
    size_t CountDiagnosticsOfKind(
        const CompiledGraphPlanDto& plan,
        CompileDiagnosticKind       kind)
    {
        const std::string kind_text = std::to_string(static_cast<int>(kind));
        size_t            count = 0;
        for (const auto& diag : plan.diagnostics)
        {
            auto obj = diag.as_object();
            if (!obj.has_value())
            {
                continue;
            }
            auto kind_value = (*obj)[std::string_view("kind")].as_string();
            if (kind_value.has_value() && *kind_value == kind_text)
            {
                ++count;
            }
        }
        return count;
    }

} // namespace

// =====================================================================
// Test fixture
// =====================================================================
class E2EGraphFlowTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Set up factory manager with standard test component definitions
        stub_mgr_ = std::make_unique<StubFactoryManager>();

        // Pass-through: reads "value_in", writes "value_out" (= value_in)
        stub_mgr_->AddDefinition(
            COMP_PASS_THROUGH,
            MakeDefinition({{"value_in", "int"}}, {{"value_out", "int"}}));

        // Double: reads "value_in", writes "value_out" (= value_in * 2)
        stub_mgr_->AddDefinition(
            COMP_DOUBLE,
            MakeDefinition({{"value_in", "int"}}, {{"value_out", "int"}}));

        // Sum: reads "value_in", writes "value_out" (= sum of value_in)
        stub_mgr_->AddDefinition(
            COMP_SUM,
            MakeDefinition({{"value_in", "int"}}, {{"value_out", "int"}}));

        // Fail: reads "value_in", always returns DAS_E_FAIL from Do()
        stub_mgr_->AddDefinition(
            COMP_FAIL,
            MakeDefinition({{"value_in", "int"}}, {{"value_out", "int"}}));

        token_ =
            Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();
    }

    void TearDown() override {}

    /// Compile a GraphDocumentDto through GraphCompiler with the stub manager
    CompiledGraphPlanDto CompileDocument(const GraphDocumentDto& doc)
    {
        GraphCompiler compiler;
        compiler.SetFactoryManager(stub_mgr_.get());
        auto plan = compiler.Compile(doc);
        return plan;
    }

    std::unique_ptr<StubFactoryManager> stub_mgr_;
    DAS::DasPtr<IDasStopToken>          token_;
};

// ---- 真实 launch 组件直驱（FlowControl 源码静态编入本测试目标） ----

constexpr std::string_view LAUNCH_COMPONENT_GUID_TEXT =
    "95FD2FFA-88F2-4F85-A536-C708D25B641F";
constexpr std::string_view LAUNCH_KIND_TEXT = "das.flow.launch";

class LaunchComponentHost final
    : public Das::PluginInterface::DasTaskComponentCreatorImplBase<
          LaunchComponentHost>
{
public:
    // 可选进程启动缝：创建组件后立即注入（返回组件前）——驱动 spawn 失败
    // 形态而无需真实进程。空 std::function = 不注入（生产路径）。
    void SetProcessSpawnerForTest(Das::FlowControlProcessSpawner spawner)
    {
        spawner_to_inject_ = std::move(spawner);
    }

    DasResult CreateTaskComponent(
        const DasGuid&                            component_guid,
        Das::PluginInterface::IDasTaskComponent** pp_out_component) override
    {
        if (pp_out_component == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_component = nullptr;
        if (component_guid
            != Das::Core::ForeignInterfaceHost::MakeDasGuid(
                std::string{LAUNCH_COMPONENT_GUID_TEXT}))
        {
            return DAS_E_NOT_FOUND;
        }

        auto* component =
            new Das::DasFlowControlTaskComponent(LAUNCH_KIND_TEXT);
        if (spawner_to_inject_)
        {
            component->SetProcessSpawnerForTest(spawner_to_inject_);
        }
        component->AddRef();
        *pp_out_component = component;
        return DAS_S_OK;
    }

    DasResult CreateAuthoringSession(
        const DasGuid&                                   task_guid,
        Das::ExportInterface::IDasJson*                  p_context_json,
        Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session)
        override
    {
        if (pp_out_session == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_session = nullptr;
        return DAS_E_NO_IMPLEMENTATION;
    }

private:
    Das::FlowControlProcessSpawner spawner_to_inject_;
};

// launch 执行失败（空配置）→ 图以专用错误码终止，错误文本携带节点 id
// 冒泡（GraphRuntime 现有失败接管路径，无 status=failed 继续跑）。测试进程
// 无 i18n 目录，诊断消息走组件硬编码英文 fallback（确定性）。
TEST_F(E2EGraphFlowTest, LaunchFailureTerminatesGraphWithErrorCode)
{
    CompiledGraphPlanDto plan;
    plan.source_fingerprint = "fp_launch_failure";

    CompiledNodeSnapshotDto launch_node;
    launch_node.node_id = "n_1";
    launch_node.component_guid = std::string{LAUNCH_COMPONENT_GUID_TEXT};
    plan.node_snapshots.push_back(std::move(launch_node));
    plan.execution_order.push_back("n_1");

    auto         host = LaunchComponentHost::Make();
    GraphRuntime runtime;
    const auto   hr = runtime.RunWithHost(
        plan,
        "fp_launch_failure",
        token_.Get(),
        host.Get());

    EXPECT_EQ(
        hr,
        Das::Plugins::DasFlowControl::DAS_E_FLOW_LAUNCH_EXE_PATH_MISSING);
    EXPECT_NE(runtime.GetLastErrorMessage().find("n_1"), std::string::npos)
        << runtime.GetLastErrorMessage();
    EXPECT_NE(
        runtime.GetLastErrorMessage().find(
            "No executable path is configured for the launch component."),
        std::string::npos)
        << runtime.GetLastErrorMessage();
}

// spawn 失败形态：注入返回 false 的启动缝 + 非空 exePath → 图以
// launch-spawn-failed 专用错误码终止，GetLastErrorMessage 同时携带节点 id
// 与组件 diagnostic 端口的 i18n fallback 文本（运行时诊断透出，非裸 hr）。
TEST_F(E2EGraphFlowTest, LaunchSpawnFailureSurfacesDiagnosticDetail)
{
    CompiledGraphPlanDto plan;
    plan.source_fingerprint = "fp_launch_spawn_failure";

    CompiledNodeSnapshotDto launch_node;
    launch_node.node_id = "n_1";
    launch_node.component_guid = std::string{LAUNCH_COMPONENT_GUID_TEXT};
    auto settings = Das::Utils::MakeYyjsonObject();
    (*settings.as_object())[std::string_view("exePath")] =
        std::string("C:/nonexistent/app.exe");
    launch_node.compiled_settings = std::move(settings);
    plan.node_snapshots.push_back(std::move(launch_node));
    plan.execution_order.push_back("n_1");

    auto host = LaunchComponentHost::Make();
    static_cast<LaunchComponentHost*>(host.Get())
        ->SetProcessSpawnerForTest(
            [](const std::string&,
               const std::vector<std::string>&,
               const std::string&,
               bool) -> bool { return false; });

    GraphRuntime runtime;
    const auto   hr = runtime.RunWithHost(
        plan,
        "fp_launch_spawn_failure",
        token_.Get(),
        host.Get());

    EXPECT_EQ(hr, Das::Plugins::DasFlowControl::DAS_E_FLOW_LAUNCH_SPAWN_FAILED);
    EXPECT_NE(runtime.GetLastErrorMessage().find("n_1"), std::string::npos)
        << runtime.GetLastErrorMessage();
    EXPECT_NE(
        runtime.GetLastErrorMessage().find(
            "Failed to start the configured executable."),
        std::string::npos)
        << runtime.GetLastErrorMessage();
}

// ---- 真实 while + 数据回边经典形状（FlowControl 源码静态编入本测试目标） ----

constexpr std::string_view WHILE_COMPONENT_GUID_TEXT =
    "68F10005-0000-4000-8000-000000000005";
constexpr std::string_view WHILE_KIND_TEXT = "das.flow.while";
constexpr std::string_view FOR_COMPONENT_GUID_TEXT =
    "68F10004-0000-4000-8000-000000000004";
constexpr std::string_view FOR_KIND_TEXT = "das.flow.for";
constexpr std::string_view COMPARE_DOUBLE_COMPONENT_GUID_TEXT =
    "68F1000E-0000-4000-8000-00000000000E";
constexpr std::string_view COMPARE_DOUBLE_KIND_TEXT = "das.flow.compare_double";
constexpr std::string_view ARITHMETIC_NUMBER_COMPONENT_GUID_TEXT =
    "68F10010-0000-4000-8000-000000000010";
constexpr std::string_view ARITHMETIC_NUMBER_KIND_TEXT =
    "das.flow.arithmetic_number";

// ---- 常量/拼接组件族（guid ↔ kind 与 manifest 登记一一对应） ----

constexpr std::string_view CONSTANT_BOOL_COMPONENT_GUID_TEXT =
    "68F10009-0000-4000-8000-000000000009";
constexpr std::string_view CONSTANT_BOOL_KIND_TEXT = "das.flow.constant_bool";
constexpr std::string_view CONSTANT_INT64_COMPONENT_GUID_TEXT =
    "68F1000A-0000-4000-8000-00000000000A";
constexpr std::string_view CONSTANT_INT64_KIND_TEXT = "das.flow.constant_int64";
constexpr std::string_view CONSTANT_DOUBLE_COMPONENT_GUID_TEXT =
    "68F1000B-0000-4000-8000-00000000000B";
constexpr std::string_view CONSTANT_DOUBLE_KIND_TEXT =
    "das.flow.constant_double";
constexpr std::string_view CONSTANT_STRING_COMPONENT_GUID_TEXT =
    "68F1000C-0000-4000-8000-00000000000C";
constexpr std::string_view CONSTANT_STRING_KIND_TEXT =
    "das.flow.constant_string";
constexpr std::string_view CONCAT_STRING_COMPONENT_GUID_TEXT =
    "68F10011-0000-4000-8000-000000000011";
constexpr std::string_view CONCAT_STRING_KIND_TEXT = "das.flow.concat_string";

// ---- 整数/字符串比较组件（guid ↔ kind 与 manifest 登记一一对应） ----

constexpr std::string_view COMPARE_INT64_COMPONENT_GUID_TEXT =
    "68F1000D-0000-4000-8000-00000000000D";
constexpr std::string_view COMPARE_INT64_KIND_TEXT = "das.flow.compare_int64";
constexpr std::string_view COMPARE_STRING_COMPONENT_GUID_TEXT =
    "68F1000F-0000-4000-8000-00000000000F";
constexpr std::string_view COMPARE_STRING_KIND_TEXT = "das.flow.compare_string";

// 执行侧观测装饰器：转发真实组件的 Do 并计数、捕获末次 result 浮点值
// ——while+数据回边收敛断言（执行次数/终值）的唯一测量点。
class CountingTaskComponent final
    : public Das::PluginInterface::IDasTaskComponent
{
public:
    std::atomic<uint32_t> ref_count_{0};
    std::atomic<int>      do_count{0};
    double                last_result = 0.0;
    std::string           last_string_result;
    std::string           last_string_output;
    bool                  last_bool_result = false;

    explicit CountingTaskComponent(
        Das::DasPtr<Das::PluginInterface::IDasTaskComponent> inner)
        : inner_(std::move(inner))
    {
    }

    uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

    uint32_t DAS_STD_CALL Release() override
    {
        auto remaining = --ref_count_;
        if (remaining == 0)
        {
            ref_count_ = 1;
            delete this;
        }
        return remaining;
    }

    DasResult DAS_STD_CALL
    QueryInterface(const DasGuid& iid, void** pp_out) override
    {
        if (pp_out == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out = nullptr;
        return DAS_E_NO_INTERFACE;
    }

    DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
    {
        return inner_->GetGuid(p_out_guid);
    }

    DasResult DAS_STD_CALL
    GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
    {
        return inner_->GetRuntimeClassName(pp_out_name);
    }

    DasResult ApplySettingsChange(
        Das::ExportInterface::IDasJson*  p_request_json,
        Das::ExportInterface::IDasJson** pp_out_result_json) override
    {
        return inner_->ApplySettingsChange(p_request_json, pp_out_result_json);
    }

    DasResult Do(
        IDasStopToken*                             p_stop_token,
        Das::ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
        Das::ExportInterface::IDasPortMap**        pp_out_port_map) override
    {
        const DasResult hr =
            inner_->Do(p_stop_token, p_input_port_map, pp_out_port_map);
        if (DAS::IsOk(hr) && pp_out_port_map != nullptr
            && *pp_out_port_map != nullptr)
        {
            do_count.fetch_add(1);
            double value = 0.0;
            if (PortMapGetDouble(*pp_out_port_map, "result", value))
            {
                last_result = value;
            }
            last_string_result = PortMapGetString(*pp_out_port_map, "result");
            last_string_output = PortMapGetString(*pp_out_port_map, "value");
            (void)PortMapGetBool(*pp_out_port_map, "result", last_bool_result);
        }
        return hr;
    }

private:
    Das::DasPtr<Das::PluginInterface::IDasTaskComponent> inner_;
};

// 按 component_guid 创建真实 FlowControl 组件并套计数装饰器（guid ↔ kind
// 与 manifest 登记一一对应）。
class FlowControlCountingHost final
    : public Das::PluginInterface::DasTaskComponentCreatorImplBase<
          FlowControlCountingHost>
{
public:
    DasResult CreateTaskComponent(
        const DasGuid&                            component_guid,
        Das::PluginInterface::IDasTaskComponent** pp_out_component) override
    {
        if (pp_out_component == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_component = nullptr;

        std::string_view kind;
        if (component_guid
            == Das::Core::ForeignInterfaceHost::MakeDasGuid(
                std::string{WHILE_COMPONENT_GUID_TEXT}))
        {
            kind = WHILE_KIND_TEXT;
        }
        else if (
            component_guid
            == Das::Core::ForeignInterfaceHost::MakeDasGuid(
                std::string{FOR_COMPONENT_GUID_TEXT}))
        {
            kind = FOR_KIND_TEXT;
        }
        else if (
            component_guid
            == Das::Core::ForeignInterfaceHost::MakeDasGuid(
                std::string{COMPARE_DOUBLE_COMPONENT_GUID_TEXT}))
        {
            kind = COMPARE_DOUBLE_KIND_TEXT;
        }
        else if (
            component_guid
            == Das::Core::ForeignInterfaceHost::MakeDasGuid(
                std::string{ARITHMETIC_NUMBER_COMPONENT_GUID_TEXT}))
        {
            kind = ARITHMETIC_NUMBER_KIND_TEXT;
        }
        else if (
            component_guid
            == Das::Core::ForeignInterfaceHost::MakeDasGuid(
                std::string{CONSTANT_STRING_COMPONENT_GUID_TEXT}))
        {
            kind = CONSTANT_STRING_KIND_TEXT;
        }
        else if (
            component_guid
            == Das::Core::ForeignInterfaceHost::MakeDasGuid(
                std::string{CONSTANT_INT64_COMPONENT_GUID_TEXT}))
        {
            kind = CONSTANT_INT64_KIND_TEXT;
        }
        else if (
            component_guid
            == Das::Core::ForeignInterfaceHost::MakeDasGuid(
                std::string{CONCAT_STRING_COMPONENT_GUID_TEXT}))
        {
            kind = CONCAT_STRING_KIND_TEXT;
        }
        else if (
            component_guid
            == Das::Core::ForeignInterfaceHost::MakeDasGuid(
                std::string{COMPARE_INT64_COMPONENT_GUID_TEXT}))
        {
            kind = COMPARE_INT64_KIND_TEXT;
        }
        else if (
            component_guid
            == Das::Core::ForeignInterfaceHost::MakeDasGuid(
                std::string{COMPARE_STRING_COMPONENT_GUID_TEXT}))
        {
            kind = COMPARE_STRING_KIND_TEXT;
        }
        else
        {
            return DAS_E_NOT_FOUND;
        }

        auto inner = Das::DasPtr<Das::PluginInterface::IDasTaskComponent>(
            new Das::DasFlowControlTaskComponent(kind));
        // 裸构造即持有一份引用（new 出的对象计数为 0 → 挪入 DasPtr 后归
        // 1），装饰器与宿主各自持链即可。
        auto* wrapper = new CountingTaskComponent(std::move(inner));
        wrapper->AddRef();
        *pp_out_component = wrapper;
        counters.push_back(wrapper);
        return DAS_S_OK;
    }

    DasResult CreateAuthoringSession(
        const DasGuid&                                   task_guid,
        Das::ExportInterface::IDasJson*                  p_context_json,
        Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session)
        override
    {
        if (pp_out_session == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_session = nullptr;
        return DAS_E_NO_IMPLEMENTATION;
    }

    std::vector<CountingTaskComponent*> counters;
};

// while(cond){ x = step(x) } 经典形状端到端：真实 FlowControl 组件
// （while 首条件口 + arithmetic_number + compare_double）+ 数据回边
// （A.result→A.left 自环喂值、C.result→W.condition 回边入头）。
TEST_F(E2EGraphFlowTest, WhileLoopWithDataBackEdgeConverges)
{
    const std::string node_while{"n_while"};
    const std::string node_arith{"n_arith"};
    const std::string node_compare{"n_compare"};

    // 编译侧口面（镜像 manifest 声明）。
    stub_mgr_->AddDefinition(
        std::string{WHILE_COMPONENT_GUID_TEXT},
        MakeDefinition(
            {{"condition", "bool"}, {"first_condition", "bool"}},
            {{"continue", "signal"}, {"break", "signal"}}));
    stub_mgr_->AddDefinition(
        std::string{ARITHMETIC_NUMBER_COMPONENT_GUID_TEXT},
        MakeDefinition(
            {{"left", "double"}, {"right", "double"}},
            {{"result", "double"}}));
    stub_mgr_->AddDefinition(
        std::string{COMPARE_DOUBLE_COMPONENT_GUID_TEXT},
        MakeDefinition(
            {{"left", "double"}, {"right", "double"}},
            {{"result", "bool"}}));

    GraphDocumentDto doc;
    doc.document_id = "doc_while_data";
    doc.nodes.push_back(
        MakeComponentNode(node_while, std::string{WHILE_COMPONENT_GUID_TEXT}));
    doc.nodes.push_back(MakeComponentNode(
        node_arith,
        std::string{ARITHMETIC_NUMBER_COMPONENT_GUID_TEXT}));
    doc.nodes.push_back(MakeComponentNode(
        node_compare,
        std::string{COMPARE_DOUBLE_COMPONENT_GUID_TEXT}));

    // 顺序线：W.continue → A.signal_in（门控）；C.signal_out → W.signal_in
    // （顺序回边承载迭代）。回边必须从环尾（执行序最后一节点）发出：多趟
    // 调度器在回边发火处立即重启 pass，环尾之后的节点在本圈永远轮不到
    // ——比较节点若落在发火点之后就永远不执行，循环条件永不更新。
    auto e_seq_body =
        MakeEdge("e_seq_body", node_while, "continue", node_arith, "signal_in");
    e_seq_body.edge_type = "signal";
    auto e_seq_back = MakeEdge(
        "e_seq_back",
        node_compare,
        "signal_out",
        node_while,
        "signal_in");
    e_seq_back.edge_type = "signal";
    // 数据回边：A.result → A.left（跨圈自喂）；前向：A.result → C.left；
    // 数据回边：C.result → W.condition（比较真值回传循环头）。
    doc.edges.push_back(std::move(e_seq_body));
    doc.edges.push_back(std::move(e_seq_back));
    doc.edges.push_back(
        MakeEdge("e_data_self", node_arith, "result", node_arith, "left"));
    doc.edges.push_back(
        MakeEdge("e_data_cond", node_arith, "result", node_compare, "left"));
    doc.edges.push_back(MakeEdge(
        "e_data_back",
        node_compare,
        "result",
        node_while,
        "condition"));

    auto plan = CompileDocument(doc);

    // 规范口名 signal 边（signal_in/signal_out）落在未声明这些口的组件上
    // 不构成 PortNotFound（隐式顺序口豁免）。
    EXPECT_EQ(
        CountDiagnosticsOfKind(plan, CompileDiagnosticKind::PortNotFound),
        0);

    // 数据回边与顺序回边同档记档（3 条），Kahn 剔除全部回边后保序。
    ASSERT_EQ(plan.back_edges.size(), 3u);
    size_t signal_kind_count = 0;
    size_t data_kind_count = 0;
    for (const auto& back : plan.back_edges)
    {
        if (back.edge_kind == "signal")
        {
            ++signal_kind_count;
            EXPECT_EQ(back.edge_id, "e_seq_back");
            EXPECT_EQ(back.loop_head_node_id, node_while);
        }
        else
        {
            ++data_kind_count;
        }
    }
    EXPECT_EQ(signal_kind_count, 1u);
    EXPECT_EQ(data_kind_count, 2u);

    // 环层记档：恰一层（tier 0），成员含 W/A/C，数据回边源口两组坐标。
    ASSERT_EQ(plan.loop_layers.size(), 1u);
    EXPECT_EQ(plan.loop_layers[0].head_node_id, node_while);
    EXPECT_EQ(plan.loop_layers[0].tier, 0);
    ASSERT_EQ(plan.loop_layers[0].members.size(), 3u);
    EXPECT_EQ(plan.loop_layers[0].members[0], node_while);
    EXPECT_EQ(plan.loop_layers[0].members[1], node_arith);
    EXPECT_EQ(plan.loop_layers[0].members[2], node_compare);
    ASSERT_EQ(plan.loop_layers[0].data_back_edge_sources.size(), 2u);

    ASSERT_EQ(plan.execution_order.size(), 3u);
    EXPECT_EQ(plan.execution_order[0], node_while);
    EXPECT_EQ(plan.execution_order[1], node_arith);
    EXPECT_EQ(plan.execution_order[2], node_compare);

    auto  host = FlowControlCountingHost::Make();
    auto* counting_host = static_cast<FlowControlCountingHost*>(host.Get());
    GraphRuntime runtime;
    const auto   hr =
        runtime.RunWithHost(plan, "fp_v1", token_.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK) << runtime.GetLastErrorMessage();

    // 首条件口缺省 true 启动第一圈；A 每圈 result 递增 1（left 默认 0、
    // right 默认 1），C 在 3 < 3 为假时让 while 读 false 走 break。
    ASSERT_EQ(counting_host->counters.size(), 3u);
    EXPECT_EQ(counting_host->counters[1]->do_count.load(), 3);
    EXPECT_EQ(counting_host->counters[1]->last_result, 3.0);
    EXPECT_EQ(counting_host->counters[0]->do_count.load(), 4);
    EXPECT_EQ(counting_host->counters[2]->do_count.load(), 3);
}

// ---- for 计数入帧（loopCarried 自环通道） ----

// for{body} 单层：计数经编译器合成的 index 自环通道入帧（组件零跨 Do 状
// 态），自环进 back_edges 后不构成数据依赖——for 不会等自己死锁。
TEST_F(E2EGraphFlowTest, ForLoopCountsViaLoopCarriedSelfChannel)
{
    const std::string node_for{"n_for"};
    const std::string node_body{"n_body"};
    const std::string node_end{"n_end"};

    stub_mgr_->AddDefinition(
        std::string{FOR_COMPONENT_GUID_TEXT},
        *Das::Utils::ParseYyjsonFromString(R"({
            "inputs": [
                {"id": "start", "type": "int"},
                {"id": "end", "type": "int"},
                {"id": "step", "type": "int"},
                {"id": "index", "type": "int"}
            ],
            "outputs": [
                {"id": "continue", "type": "signal"},
                {"id": "break", "type": "signal"},
                {"id": "index", "type": "int", "loopCarried": true}
            ]
        })"));
    stub_mgr_->AddDefinition(
        std::string{COMPARE_INT64_COMPONENT_GUID_TEXT},
        MakeDefinition(
            {{"left", "int64"}, {"right", "int64"}},
            {{"result", "bool"}, {"signal_out", "signal"}}));
    stub_mgr_->AddDefinition(
        std::string{CONSTANT_INT64_COMPONENT_GUID_TEXT},
        MakeDefinition({}, {{"value", "int64"}}));

    GraphDocumentDto doc;
    doc.document_id = "doc_for_self_channel";
    doc.nodes.push_back(
        MakeComponentNode(node_for, std::string{FOR_COMPONENT_GUID_TEXT}));
    doc.nodes.push_back(MakeComponentNode(
        node_body,
        std::string{COMPARE_INT64_COMPONENT_GUID_TEXT}));
    auto end_node = MakeComponentNode(
        node_end,
        std::string{CONSTANT_INT64_COMPONENT_GUID_TEXT});
    auto end_settings = Das::Utils::MakeYyjsonObject();
    (*end_settings.as_object())[std::string_view("value")] = 3;
    end_node.settings = end_settings;
    doc.nodes.push_back(std::move(end_node));

    auto e_body_gate =
        MakeEdge("e_body_gate", node_for, "continue", node_body, "signal_in");
    e_body_gate.edge_type = "signal";
    auto e_body_back =
        MakeEdge("e_body_back", node_body, "signal_out", node_for, "signal_in");
    e_body_back.edge_type = "signal";
    doc.edges.push_back(std::move(e_body_gate));
    doc.edges.push_back(std::move(e_body_back));
    doc.edges.push_back(
        MakeEdge("e_index", node_for, "index", node_body, "left"));
    doc.edges.push_back(MakeEdge("e_end", node_end, "value", node_for, "end"));

    auto plan = CompileDocument(doc);

    // 合成回边记档：自环 data 回边归属 for 自身层，源口入层清单。
    const Dto::BackEdgeDto* carried_back = nullptr;
    for (const auto& back_edge : plan.back_edges)
    {
        if (back_edge.edge_kind == "data"
            && back_edge.source_node_id == node_for
            && back_edge.target_node_id == node_for)
        {
            carried_back = &back_edge;
        }
    }
    ASSERT_NE(carried_back, nullptr);
    EXPECT_EQ(carried_back->loop_head_node_id, node_for);
    ASSERT_EQ(plan.loop_layers.size(), 1u);
    ASSERT_EQ(plan.loop_layers[0].data_back_edge_sources.size(), 1u);
    EXPECT_EQ(plan.loop_layers[0].data_back_edge_sources[0].node_id, node_for);
    EXPECT_EQ(plan.loop_layers[0].data_back_edge_sources[0].port_id, "index");

    auto  host = FlowControlCountingHost::Make();
    auto* counting_host = static_cast<FlowControlCountingHost*>(host.Get());
    GraphRuntime runtime;
    const auto   hr =
        runtime.RunWithHost(plan, "fp_for", token_.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK) << runtime.GetLastErrorMessage();

    // end=3：for 3 次 continue（index 0/1/2）+ 1 次 break，body 每圈执行。
    ASSERT_EQ(counting_host->counters.size(), 3u);
    EXPECT_EQ(counting_host->counters[0]->do_count.load(), 4);
    EXPECT_EQ(counting_host->counters[1]->do_count.load(), 3);
}

// 嵌套 while{for{body}}：外层 2 圈 × 内层从头计数——for 的 index 状态入
// 帧且归属内层清单，外层发火清严格内层源口时计数复位（外圈 2 起 body 每
// 圈照常执行 N 次）。
TEST_F(E2EGraphFlowTest, NestedWhileForRestartsInnerCountPerOuterRound)
{
    const std::string node_while{"n_while"};
    const std::string node_counter{"n_counter"};
    const std::string node_for{"n_for"};
    const std::string node_body{"n_body"};
    const std::string node_tail{"n_tail"};
    const std::string node_end{"n_end"};

    stub_mgr_->AddDefinition(
        std::string{WHILE_COMPONENT_GUID_TEXT},
        MakeDefinition(
            {{"condition", "bool"}, {"first_condition", "bool"}},
            {{"continue", "signal"}, {"break", "signal"}}));
    stub_mgr_->AddDefinition(
        std::string{FOR_COMPONENT_GUID_TEXT},
        *Das::Utils::ParseYyjsonFromString(R"({
            "inputs": [
                {"id": "start", "type": "int"},
                {"id": "end", "type": "int"},
                {"id": "step", "type": "int"},
                {"id": "index", "type": "int"}
            ],
            "outputs": [
                {"id": "continue", "type": "signal"},
                {"id": "break", "type": "signal"},
                {"id": "index", "type": "int", "loopCarried": true}
            ]
        })"));
    stub_mgr_->AddDefinition(
        std::string{COMPARE_INT64_COMPONENT_GUID_TEXT},
        MakeDefinition(
            {{"left", "int64"}, {"right", "int64"}},
            {{"result", "bool"}, {"signal_out", "signal"}}));
    stub_mgr_->AddDefinition(
        std::string{COMPARE_DOUBLE_COMPONENT_GUID_TEXT},
        MakeDefinition(
            {{"left", "double"}, {"right", "double"}},
            {{"result", "bool"}, {"signal_out", "signal"}}));
    stub_mgr_->AddDefinition(
        std::string{ARITHMETIC_NUMBER_COMPONENT_GUID_TEXT},
        MakeDefinition(
            {{"left", "double"}, {"right", "double"}},
            {{"result", "double"}}));
    stub_mgr_->AddDefinition(
        std::string{CONSTANT_INT64_COMPONENT_GUID_TEXT},
        MakeDefinition({}, {{"value", "int64"}}));

    GraphDocumentDto doc;
    doc.document_id = "doc_nested_while_for";
    doc.nodes.push_back(
        MakeComponentNode(node_while, std::string{WHILE_COMPONENT_GUID_TEXT}));
    doc.nodes.push_back(MakeComponentNode(
        node_counter,
        std::string{ARITHMETIC_NUMBER_COMPONENT_GUID_TEXT}));
    doc.nodes.push_back(
        MakeComponentNode(node_for, std::string{FOR_COMPONENT_GUID_TEXT}));
    doc.nodes.push_back(MakeComponentNode(
        node_body,
        std::string{COMPARE_INT64_COMPONENT_GUID_TEXT}));
    auto tail_node = MakeComponentNode(
        node_tail,
        std::string{COMPARE_DOUBLE_COMPONENT_GUID_TEXT});
    // 外层条件：counter < 2（counter 每外圈 +1，第 2 圈收尾时翻假）。计数
    // 器输出 double，条件比较必须用 double 侧——浮点→整数物化按设计拒
    // 绝，错配会让左值恒缺回落 0 永真。
    auto tail_settings = Das::Utils::MakeYyjsonObject();
    auto tail_settings_object = *tail_settings.as_object();
    tail_settings_object[std::string_view("operator")] =
        std::make_pair(std::string_view("<"), yyjson::copy_string);
    tail_settings_object[std::string_view("right")] = 2;
    tail_node.settings = tail_settings;
    doc.nodes.push_back(std::move(tail_node));
    auto end_node = MakeComponentNode(
        node_end,
        std::string{CONSTANT_INT64_COMPONENT_GUID_TEXT});
    auto end_settings = Das::Utils::MakeYyjsonObject();
    (*end_settings.as_object())[std::string_view("value")] = 3;
    end_node.settings = end_settings;
    doc.nodes.push_back(std::move(end_node));

    // 顺序线：W.continue 门控外圈计数器与 for；for.continue 门控 body；
    // body 完成信号回 for（内层回边）；for 耗尽发 break 门控外层尾；外层
    // 尾完成信号回 W（外层回边）。计数器与 body 都必须受门控：无门的
    // 环内节点每趟无条件执行，会把回边发成死循环。
    auto e_gate_counter = MakeEdge(
        "e_gate_counter",
        node_while,
        "continue",
        node_counter,
        "signal_in");
    e_gate_counter.edge_type = "signal";
    auto e_gate_for =
        MakeEdge("e_gate_for", node_while, "continue", node_for, "signal_in");
    e_gate_for.edge_type = "signal";
    auto e_gate_body =
        MakeEdge("e_gate_body", node_for, "continue", node_body, "signal_in");
    e_gate_body.edge_type = "signal";
    auto e_inner_back = MakeEdge(
        "e_inner_back",
        node_body,
        "signal_out",
        node_for,
        "signal_in");
    e_inner_back.edge_type = "signal";
    auto e_gate_tail =
        MakeEdge("e_gate_tail", node_for, "break", node_tail, "signal_in");
    e_gate_tail.edge_type = "signal";
    auto e_outer_back = MakeEdge(
        "e_outer_back",
        node_tail,
        "signal_out",
        node_while,
        "signal_in");
    e_outer_back.edge_type = "signal";
    doc.edges.push_back(std::move(e_gate_counter));
    doc.edges.push_back(std::move(e_gate_for));
    doc.edges.push_back(std::move(e_gate_body));
    doc.edges.push_back(std::move(e_inner_back));
    doc.edges.push_back(std::move(e_gate_tail));
    doc.edges.push_back(std::move(e_outer_back));

    // 数据面：for.index 门控 body 比较左值；counter 自环累计并喂外层条件
    // 比较；外层条件真值回 W.condition（外层数据回边）；常数 3 喂 for.end。
    doc.edges.push_back(
        MakeEdge("e_body_left", node_for, "index", node_body, "left"));
    doc.edges.push_back(MakeEdge(
        "e_counter_self",
        node_counter,
        "result",
        node_counter,
        "left"));
    doc.edges.push_back(
        MakeEdge("e_counter_tail", node_counter, "result", node_tail, "left"));
    doc.edges.push_back(MakeEdge(
        "e_condition_back",
        node_tail,
        "result",
        node_while,
        "condition"));
    doc.edges.push_back(
        MakeEdge("e_for_end", node_end, "value", node_for, "end"));

    auto plan = CompileDocument(doc);

    // 合成回边归属内层（for 自身层），源口在内层清单——外层发火清严格内
    // 层源口时复位计数。
    const Dto::BackEdgeDto* carried_back = nullptr;
    for (const auto& back_edge : plan.back_edges)
    {
        if (back_edge.edge_kind == "data"
            && back_edge.source_node_id == node_for
            && back_edge.target_node_id == node_for)
        {
            carried_back = &back_edge;
        }
    }
    ASSERT_NE(carried_back, nullptr);
    EXPECT_EQ(carried_back->loop_head_node_id, node_for);
    bool has_self_binding = false;
    for (const auto& binding : plan.binding_plan.bindings)
    {
        if (binding.source_node_id == node_for
            && binding.source_port_id == "index"
            && binding.target_node_id == node_for
            && binding.target_port_id == "index")
        {
            has_self_binding = true;
        }
    }
    EXPECT_TRUE(has_self_binding);
    const Dto::LoopLayerDto* inner_layer = nullptr;
    for (const auto& layer : plan.loop_layers)
    {
        if (layer.head_node_id == node_for)
        {
            inner_layer = &layer;
        }
    }
    ASSERT_NE(inner_layer, nullptr);
    ASSERT_EQ(inner_layer->data_back_edge_sources.size(), 1u);
    EXPECT_EQ(inner_layer->data_back_edge_sources[0].node_id, node_for);
    EXPECT_EQ(inner_layer->data_back_edge_sources[0].port_id, "index");

    auto  host = FlowControlCountingHost::Make();
    auto* counting_host = static_cast<FlowControlCountingHost*>(host.Get());
    GraphRuntime runtime;
    const auto   hr =
        runtime.RunWithHost(plan, "fp_nested", token_.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK) << runtime.GetLastErrorMessage();

    // body 每外圈执行 3 次（end=3，外圈 2 起从头计数）共 6；外圈计数器
    // 每外圈 1 次共 2（值 1、2）；while 2 次 continue + 1 次 break。
    ASSERT_EQ(counting_host->counters.size(), 6u);
    EXPECT_EQ(counting_host->counters[3]->do_count.load(), 6);
    EXPECT_EQ(counting_host->counters[1]->do_count.load(), 2);
    EXPECT_EQ(counting_host->counters[1]->last_result, 2.0);
    EXPECT_EQ(counting_host->counters[0]->do_count.load(), 3);
    EXPECT_EQ(counting_host->counters[4]->do_count.load(), 2);
}

// ---- 常量组件族 + 字符串拼接 ----

// 经 GraphRuntime 的 {"settings": S} 包装形直投组件配置（与生产
// ApplyNodeSettings 投递形状一致）。
void ApplyComponentSettings(
    Das::DasFlowControlTaskComponent* component,
    const yyjson::value&              settings)
{
    auto change = Das::Utils::MakeYyjsonObject();
    (*change.as_object())[std::string_view("settings")] =
        Das::Utils::CloneYyjsonValue(settings);
    auto json = Das::Utils::WrapJson(std::move(change));
    ASSERT_NE(json.Get(), nullptr);
    Das::DasPtr<Das::ExportInterface::IDasJson> result;
    ASSERT_EQ(
        component->ApplySettingsChange(json.Get(), result.Put()),
        DAS_S_OK);
}

// 文本形态入口：解析后走共享投递路径（ASSERT 需 void 函数体，lambda
// 返回非 void 不能内联断言）。
void ApplyComponentSettingsText(
    Das::DasFlowControlTaskComponent* component,
    std::string_view                  settings_text)
{
    auto settings = Das::Utils::ParseYyjsonFromString(settings_text);
    ASSERT_TRUE(settings.has_value());
    ApplyComponentSettings(component, *settings);
}

// 常量→拼接真组件链：constant_string 产 "Hello"、concat 连线吃左值 +
// 面板默认右值 → result == "Hello World"（值路由经帧、右值走 config
// 缺省即语义）。
TEST_F(E2EGraphFlowTest, ConstantsAndConcatChainProducesJoinedString)
{
    const std::string node_constant{"n_constant"};
    const std::string node_concat{"n_concat"};

    stub_mgr_->AddDefinition(
        std::string{CONSTANT_STRING_COMPONENT_GUID_TEXT},
        MakeDefinition({}, {{"value", "string"}}));
    stub_mgr_->AddDefinition(
        std::string{CONCAT_STRING_COMPONENT_GUID_TEXT},
        MakeDefinition(
            {{"left", "string"}, {"right", "string"}},
            {{"result", "string"}}));

    GraphDocumentDto doc;
    doc.document_id = "doc_constants_concat";
    auto constant_node = MakeComponentNode(
        node_constant,
        std::string{CONSTANT_STRING_COMPONENT_GUID_TEXT});
    auto constant_settings = Das::Utils::MakeYyjsonObject();
    (*constant_settings.as_object())[std::string_view("value")] =
        std::string("Hello");
    constant_node.settings = constant_settings;
    doc.nodes.push_back(std::move(constant_node));
    auto concat_node = MakeComponentNode(
        node_concat,
        std::string{CONCAT_STRING_COMPONENT_GUID_TEXT});
    auto concat_settings = Das::Utils::MakeYyjsonObject();
    (*concat_settings.as_object())[std::string_view("right")] =
        std::string(" World");
    concat_node.settings = concat_settings;
    doc.nodes.push_back(std::move(concat_node));

    doc.edges.push_back(
        MakeEdge("e_left", node_constant, "value", node_concat, "left"));

    auto plan = CompileDocument(doc);
    EXPECT_EQ(
        CountDiagnosticsOfKind(plan, CompileDiagnosticKind::PortNotFound),
        0);
    ASSERT_EQ(plan.execution_order.size(), 2u);
    EXPECT_EQ(plan.execution_order[0], node_constant);
    EXPECT_EQ(plan.execution_order[1], node_concat);

    auto  host = FlowControlCountingHost::Make();
    auto* counting_host = static_cast<FlowControlCountingHost*>(host.Get());
    GraphRuntime runtime;
    const auto   hr =
        runtime.RunWithHost(plan, "fp_constants", token_.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK) << runtime.GetLastErrorMessage();

    ASSERT_EQ(counting_host->counters.size(), 2u);
    EXPECT_EQ(counting_host->counters[0]->do_count.load(), 1);
    EXPECT_EQ(counting_host->counters[0]->last_string_output, "Hello");
    EXPECT_EQ(counting_host->counters[1]->do_count.load(), 1);
    EXPECT_EQ(counting_host->counters[1]->last_string_result, "Hello World");
}

// 标量常量组件直驱：typed 输出口 + status 两件（bool/int64/double 三型；
// config 缺 value 键回落各类型零值=缺省即语义）。规范口 signal_out 不再
// 自发（发射归运行时兜底），signals 键缺省。
TEST_F(E2EGraphFlowTest, ScalarConstantComponentsEmitTypedValue)
{
    const auto run_constant = [this](
                                  std::string_view                kind,
                                  std::optional<std::string_view> settings_text)
        -> Das::DasPtr<Das::ExportInterface::IDasPortMap>
    {
        auto component = Das::DasPtr<Das::DasFlowControlTaskComponent>{
            new Das::DasFlowControlTaskComponent(kind)};
        if (settings_text.has_value())
        {
            ApplyComponentSettingsText(component.Get(), *settings_text);
        }
        Das::DasPtr<Das::ExportInterface::IDasPortMap> out;
        const auto do_hr = component->Do(token_.Get(), nullptr, out.Put());
        EXPECT_EQ(do_hr, DAS_S_OK);
        return out;
    };

    {
        auto out = run_constant(CONSTANT_BOOL_KIND_TEXT, R"({"value": true})");
        ASSERT_NE(out.Get(), nullptr);
        bool value = false;
        ASSERT_TRUE(PortMapGetBool(out.Get(), "value", value));
        EXPECT_TRUE(value);
        EXPECT_EQ(PortMapGetString(out.Get(), "status"), "completed");
        EXPECT_EQ(PortMapGetString(out.Get(), "signals"), "");
    }
    {
        auto out = run_constant(CONSTANT_INT64_KIND_TEXT, R"({"value": 42})");
        ASSERT_NE(out.Get(), nullptr);
        EXPECT_EQ(PortMapGetInt(out.Get(), "value"), 42);
        EXPECT_EQ(PortMapGetString(out.Get(), "status"), "completed");
    }
    {
        auto out = run_constant(CONSTANT_DOUBLE_KIND_TEXT, R"({"value": 2.5})");
        ASSERT_NE(out.Get(), nullptr);
        double value = 0.0;
        ASSERT_TRUE(PortMapGetDouble(out.Get(), "value", value));
        EXPECT_DOUBLE_EQ(value, 2.5);
        EXPECT_EQ(PortMapGetString(out.Get(), "status"), "completed");
    }
    {
        // config 缺 value 键：各类型零值回落（缺省即语义，不报错）。
        auto out = run_constant(CONSTANT_INT64_KIND_TEXT, std::nullopt);
        ASSERT_NE(out.Get(), nullptr);
        EXPECT_EQ(PortMapGetInt(out.Get(), "value"), 0);
        EXPECT_EQ(PortMapGetString(out.Get(), "status"), "completed");
    }
}

// ---- 整数/字符串比较组件 ----

// int 比较链：constant_int64 产 7、compare_int64 连线吃左值 + 面板默认
// 右值（10）按 "<" 产出 true。
TEST_F(E2EGraphFlowTest, CompareInt64ChainProducesBool)
{
    const std::string node_constant{"n_constant"};
    const std::string node_compare{"n_compare"};

    stub_mgr_->AddDefinition(
        std::string{CONSTANT_INT64_COMPONENT_GUID_TEXT},
        MakeDefinition({}, {{"value", "int64"}}));
    stub_mgr_->AddDefinition(
        std::string{COMPARE_INT64_COMPONENT_GUID_TEXT},
        MakeDefinition(
            {{"left", "int64"}, {"right", "int64"}},
            {{"result", "bool"}}));

    GraphDocumentDto doc;
    doc.document_id = "doc_compare_int64";
    auto constant_node = MakeComponentNode(
        node_constant,
        std::string{CONSTANT_INT64_COMPONENT_GUID_TEXT});
    auto constant_settings = Das::Utils::MakeYyjsonObject();
    (*constant_settings.as_object())[std::string_view("value")] = 7;
    constant_node.settings = constant_settings;
    doc.nodes.push_back(std::move(constant_node));
    auto compare_node = MakeComponentNode(
        node_compare,
        std::string{COMPARE_INT64_COMPONENT_GUID_TEXT});
    auto compare_settings = Das::Utils::MakeYyjsonObject();
    (*compare_settings.as_object())[std::string_view("operator")] =
        std::string("<");
    (*compare_settings.as_object())[std::string_view("right")] = 10;
    compare_node.settings = compare_settings;
    doc.nodes.push_back(std::move(compare_node));

    doc.edges.push_back(
        MakeEdge("e_left", node_constant, "value", node_compare, "left"));

    auto plan = CompileDocument(doc);
    ASSERT_EQ(plan.execution_order.size(), 2u);

    auto  host = FlowControlCountingHost::Make();
    auto* counting_host = static_cast<FlowControlCountingHost*>(host.Get());
    GraphRuntime runtime;
    const auto   hr =
        runtime.RunWithHost(plan, "fp_compare_int64", token_.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK) << runtime.GetLastErrorMessage();

    ASSERT_EQ(counting_host->counters.size(), 2u);
    EXPECT_EQ(counting_host->counters[1]->do_count.load(), 1);
    EXPECT_TRUE(counting_host->counters[1]->last_bool_result);
}

// string 比较链 + 运算符值域 fail-closed：等值比较产出 true；值域外
// 运算符（"<" 不在 string 的 {==,!=} 域）经图执行链以专用错误码显性
// 终止（不静默走默认分支），错误文本携带节点 id。
TEST_F(E2EGraphFlowTest, CompareStringChainAndOperatorDomainFailClosed)
{
    const std::string node_constant{"n_constant"};
    const std::string node_compare{"n_compare"};

    stub_mgr_->AddDefinition(
        std::string{CONSTANT_STRING_COMPONENT_GUID_TEXT},
        MakeDefinition({}, {{"value", "string"}}));
    stub_mgr_->AddDefinition(
        std::string{COMPARE_STRING_COMPONENT_GUID_TEXT},
        MakeDefinition(
            {{"left", "string"}, {"right", "string"}},
            {{"result", "bool"}}));

    {
        GraphDocumentDto doc;
        doc.document_id = "doc_compare_string";
        auto constant_node = MakeComponentNode(
            node_constant,
            std::string{CONSTANT_STRING_COMPONENT_GUID_TEXT});
        auto constant_settings = Das::Utils::MakeYyjsonObject();
        (*constant_settings.as_object())[std::string_view("value")] =
            std::string("abc");
        constant_node.settings = constant_settings;
        doc.nodes.push_back(std::move(constant_node));
        auto compare_node = MakeComponentNode(
            node_compare,
            std::string{COMPARE_STRING_COMPONENT_GUID_TEXT});
        auto compare_settings = Das::Utils::MakeYyjsonObject();
        (*compare_settings.as_object())[std::string_view("operator")] =
            std::string("==");
        (*compare_settings.as_object())[std::string_view("right")] =
            std::string("abc");
        compare_node.settings = compare_settings;
        doc.nodes.push_back(std::move(compare_node));

        doc.edges.push_back(
            MakeEdge("e_left", node_constant, "value", node_compare, "left"));

        auto plan = CompileDocument(doc);
        ASSERT_EQ(plan.execution_order.size(), 2u);

        auto  host = FlowControlCountingHost::Make();
        auto* counting_host = static_cast<FlowControlCountingHost*>(host.Get());
        GraphRuntime runtime;
        const auto   hr = runtime.RunWithHost(
            plan,
            "fp_compare_string",
            token_.Get(),
            host.Get());
        ASSERT_EQ(hr, DAS_S_OK) << runtime.GetLastErrorMessage();

        ASSERT_EQ(counting_host->counters.size(), 2u);
        EXPECT_EQ(counting_host->counters[1]->do_count.load(), 1);
        EXPECT_TRUE(counting_host->counters[1]->last_bool_result);
    }

    {
        // 值域外运算符：图执行以专用错误码终止，消息携带节点 id。
        GraphDocumentDto doc;
        doc.document_id = "doc_compare_string_invalid";
        auto compare_node = MakeComponentNode(
            node_compare,
            std::string{COMPARE_STRING_COMPONENT_GUID_TEXT});
        auto compare_settings = Das::Utils::MakeYyjsonObject();
        (*compare_settings.as_object())[std::string_view("operator")] =
            std::string("<");
        (*compare_settings.as_object())[std::string_view("left")] =
            std::string("abc");
        (*compare_settings.as_object())[std::string_view("right")] =
            std::string("abd");
        compare_node.settings = compare_settings;
        doc.nodes.push_back(std::move(compare_node));

        auto plan = CompileDocument(doc);
        ASSERT_EQ(plan.execution_order.size(), 1u);

        auto         host = FlowControlCountingHost::Make();
        GraphRuntime runtime;
        const auto   hr = runtime.RunWithHost(
            plan,
            "fp_invalid_op",
            token_.Get(),
            host.Get());
        EXPECT_EQ(
            hr,
            Das::Plugins::DasFlowControl::DAS_E_FLOW_OPERATOR_INVALID);
        EXPECT_NE(
            runtime.GetLastErrorMessage().find(node_compare),
            std::string::npos)
            << runtime.GetLastErrorMessage();
    }
}

// ---- 规范口 signal 边兜底（成功发射是运行时保证） ----

// 不自发射 signal_out 的源节点经规范 signal 边连通的后继真实执行：两种
// 形态——源 manifest 未声明 signal_out（隐式豁免形态，镜像常量组件）与
// 声明为 signal 类型（launch 形态）。标记经 plan JSON 序列化/反序列化往
// 返存活（钉住 DasGraphTask 的 plan JSON 通路），用往返后的 plan 跑图。
TEST_F(E2EGraphFlowTest, CanonicalSignalEdgeFromNonEmittingSourceRunsSuccessor)
{
    const std::string node_source_implicit{"n_source_implicit"};
    const std::string node_source_declared{"n_source_declared"};
    const std::string node_probe_implicit{"n_probe_implicit"};
    const std::string node_probe_declared{"n_probe_declared"};

    stub_mgr_->AddDefinition(
        std::string{CONSTANT_INT64_COMPONENT_GUID_TEXT},
        MakeDefinition({}, {{"value", "int64"}}));
    stub_mgr_->AddDefinition(
        std::string{COMPARE_INT64_COMPONENT_GUID_TEXT},
        MakeDefinition(
            {{"left", "int64"}, {"right", "int64"}},
            {{"result", "bool"}, {"signal_out", "signal"}}));

    GraphDocumentDto doc;
    doc.document_id = "doc_canonical_signal";
    doc.nodes.push_back(MakeComponentNode(
        node_source_implicit,
        std::string{CONSTANT_INT64_COMPONENT_GUID_TEXT}));
    doc.nodes.push_back(MakeComponentNode(
        node_source_declared,
        std::string{COMPARE_INT64_COMPONENT_GUID_TEXT}));
    doc.nodes.push_back(MakeComponentNode(
        node_probe_implicit,
        std::string{CONSTANT_INT64_COMPONENT_GUID_TEXT}));
    doc.nodes.push_back(MakeComponentNode(
        node_probe_declared,
        std::string{CONSTANT_INT64_COMPONENT_GUID_TEXT}));

    auto edge_implicit = MakeEdge(
        "e_implicit",
        node_source_implicit,
        "signal_out",
        node_probe_implicit,
        "signal_in");
    edge_implicit.edge_type = "signal";
    auto edge_declared = MakeEdge(
        "e_declared",
        node_source_declared,
        "signal_out",
        node_probe_declared,
        "signal_in");
    edge_declared.edge_type = "signal";
    doc.edges.push_back(std::move(edge_implicit));
    doc.edges.push_back(std::move(edge_declared));

    auto plan = CompileDocument(doc);
    EXPECT_EQ(
        CountDiagnosticsOfKind(plan, CompileDiagnosticKind::PortNotFound),
        0);
    ASSERT_EQ(plan.signal_routes.size(), 2u);
    for (const auto& route : plan.signal_routes)
    {
        EXPECT_TRUE(route.source_port_canonical);
    }

    const auto serialized =
        Das::Utils::SerializeYyjsonValue(yyjson::object(plan));
    ASSERT_TRUE(serialized.has_value());
    const auto parsed = Das::Utils::ParseYyjsonFromString(*serialized);
    ASSERT_TRUE(parsed.has_value());
    const auto round_tripped = yyjson::cast<CompiledGraphPlanDto>(*parsed);
    ASSERT_EQ(round_tripped.signal_routes.size(), 2u);
    for (const auto& route : round_tripped.signal_routes)
    {
        EXPECT_TRUE(route.source_port_canonical);
    }

    auto  host = FlowControlCountingHost::Make();
    auto* counting_host = static_cast<FlowControlCountingHost*>(host.Get());
    GraphRuntime runtime;
    const auto   hr = runtime.RunWithHost(
        round_tripped,
        "fp_canonical",
        token_.Get(),
        host.Get());
    ASSERT_EQ(hr, DAS_S_OK) << runtime.GetLastErrorMessage();

    // 四节点全部执行恰一次：修复前探针=0（门没开被静默跳过），兜底后
    // 两形态探针都真实执行。
    ASSERT_EQ(counting_host->counters.size(), 4u);
    for (const auto* counter : counting_host->counters)
    {
        EXPECT_EQ(counter->do_count.load(), 1);
    }
}

namespace
{
    // 合作取消形态的最小源组件：Do 返回 S_OK，仅写 status="cancelled"、
    // 不发射任何信号（BuildResultPortMap 组件协议取消形态的镜像）。
    const std::string CANCELLED_SOURCE_GUID =
        "C0000000-0000-0000-0000-000000000001";
    const std::string COUNTING_PROBE_GUID =
        "C0000000-0000-0000-0000-000000000002";

    class CancelledSourceComponent final
        : public Das::PluginInterface::IDasTaskComponent
    {
    public:
        std::atomic<uint32_t> ref_count_{0};

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                ref_count_ = 1;
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            *p_out_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                CANCELLED_SOURCE_GUID);
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            return CreateIDasReadOnlyStringFromUtf8(
                "CancelledSourceComponent",
                pp_out_name);
        }

        DasResult ApplySettingsChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json != nullptr)
            {
                *pp_out_result_json = nullptr;
            }
            return DAS_S_OK;
        }

        DasResult Do(
            IDasStopToken*,
            IDasReadOnlyPortMap*,
            IDasPortMap** pp_out_port_map) override
        {
            if (pp_out_port_map == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_port_map = nullptr;
            DAS::DasPtr<IDasPortMap> output_map;
            DasResult                hr = CreateIDasPortMap(output_map.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            Das::Utils::SetPortString(output_map.Get(), "status", "cancelled");
            *pp_out_port_map = output_map.Get();
            output_map.Get()->AddRef();
            return DAS_S_OK;
        }
    };

    // 计数探针：Do 计数即「下游是否执行」的唯一观测点。
    class CountingProbeComponent final
        : public Das::PluginInterface::IDasTaskComponent
    {
    public:
        std::atomic<uint32_t> ref_count_{0};
        std::atomic<int>      do_count{0};

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                ref_count_ = 1;
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            *p_out_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                COUNTING_PROBE_GUID);
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            return CreateIDasReadOnlyStringFromUtf8(
                "CountingProbeComponent",
                pp_out_name);
        }

        DasResult ApplySettingsChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json != nullptr)
            {
                *pp_out_result_json = nullptr;
            }
            return DAS_S_OK;
        }

        DasResult Do(
            IDasStopToken*,
            IDasReadOnlyPortMap*,
            IDasPortMap** pp_out_port_map) override
        {
            if (pp_out_port_map == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_port_map = nullptr;
            do_count.fetch_add(1);
            return DAS_S_OK;
        }
    };

    class CancelledProbeHost final
        : public Das::PluginInterface::DasTaskComponentCreatorImplBase<
              CancelledProbeHost>
    {
    public:
        DasResult CreateTaskComponent(
            const DasGuid&                            component_guid,
            Das::PluginInterface::IDasTaskComponent** pp_out_component) override
        {
            if (pp_out_component == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_component = nullptr;
            const std::string guid_text =
                Das::Core::ForeignInterfaceHost::DasGuidToStdString(
                    component_guid);

            if (guid_text == CANCELLED_SOURCE_GUID)
            {
                auto* component = new CancelledSourceComponent();
                component->AddRef();
                *pp_out_component = component;
                return DAS_S_OK;
            }
            if (guid_text == COUNTING_PROBE_GUID)
            {
                auto* component = new CountingProbeComponent();
                component->AddRef();
                *pp_out_component = component;
                probe = component;
                return DAS_S_OK;
            }
            return DAS_E_NOT_FOUND;
        }

        DasResult CreateAuthoringSession(
            const DasGuid&                                   task_guid,
            Das::ExportInterface::IDasJson*                  p_context_json,
            Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session)
            override
        {
            if (pp_out_session == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_session = nullptr;
            return DAS_E_NO_IMPLEMENTATION;
        }

        CountingProbeComponent* probe = nullptr;
    };
} // namespace

// 合作取消（S_OK + status="cancelled"）节点的规范口不代发：兜底的
// cancelled 守卫保持下游门关闭，探针不执行（取消语义不回归——代发会让
// 下游尝试执行并在 stop 检查处打断整图）。
TEST_F(E2EGraphFlowTest, CanonicalBackfillSkippedWhenNodeCancelled)
{
    const std::string node_cancel{"n_cancel"};
    const std::string node_probe{"n_probe"};

    stub_mgr_->AddDefinition(CANCELLED_SOURCE_GUID, MakeDefinition({}, {}));
    stub_mgr_->AddDefinition(COUNTING_PROBE_GUID, MakeDefinition({}, {}));

    GraphDocumentDto doc;
    doc.document_id = "doc_canonical_cancelled";
    doc.nodes.push_back(MakeComponentNode(node_cancel, CANCELLED_SOURCE_GUID));
    doc.nodes.push_back(MakeComponentNode(node_probe, COUNTING_PROBE_GUID));

    auto edge_gate =
        MakeEdge("e_gate", node_cancel, "signal_out", node_probe, "signal_in");
    edge_gate.edge_type = "signal";
    doc.edges.push_back(std::move(edge_gate));

    auto plan = CompileDocument(doc);
    EXPECT_EQ(
        CountDiagnosticsOfKind(plan, CompileDiagnosticKind::PortNotFound),
        0);
    ASSERT_EQ(plan.signal_routes.size(), 1u);
    EXPECT_TRUE(plan.signal_routes[0].source_port_canonical);

    auto         host = CancelledProbeHost::Make();
    auto*        probe_host = static_cast<CancelledProbeHost*>(host.Get());
    GraphRuntime runtime;
    const auto   hr =
        runtime.RunWithHost(plan, "fp_cancelled", token_.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK) << runtime.GetLastErrorMessage();

    ASSERT_NE(probe_host->probe, nullptr);
    EXPECT_EQ(probe_host->probe->do_count.load(), 0);
}

// ===========================================================================
// 引用闭包注入真实链 E2E（编译期现算+creator 携带通道）：fake creator
// 携带进图 authoring 会话 → 会话编译含绑条目节点的图 → binding_plan 出
// 现产物广播绑定（$graph_input→executionInput，default_value=compile 段
// 原文）→ RunWithHost 执行 → 记录组件经 executionInput 收到产物（物化
// 路径零新机制——DoAdapter 现成 $graph_input 广播物化）。
// ===========================================================================
namespace
{
    using IDasTaskComponent = Das::PluginInterface::IDasTaskComponent;

    constexpr std::string_view REF_CHAIN_GRAPH_TASK_GUID =
        "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3";

    struct ReferenceChainState
    {
        bool        received = false;
        std::string execution_input_text;
    };

    // GraphTask 执行组件替身：Do 记录 executionInput 端口收到的产物串。
    class ExecutionInputRecorderComponent final : public IDasTaskComponent
    {
    public:
        ExecutionInputRecorderComponent(
            std::shared_ptr<ReferenceChainState> state,
            DasGuid                              guid)
            : state_(std::move(state)), guid_(guid)
        {
        }

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult GetGuid(DasGuid* p_out_guid) override
        {
            *p_out_guid = guid_;
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            return CreateIDasReadOnlyStringFromUtf8(
                "ExecutionInputRecorderComponent",
                pp_out_name);
        }

        DasResult ApplySettingsChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json != nullptr)
            {
                *pp_out_result_json = nullptr;
            }
            return DAS_S_OK;
        }

        DasResult Do(
            IDasStopToken*,
            IDasReadOnlyPortMap* p_input_port_map,
            IDasPortMap**        pp_out_port_map) override
        {
            if (pp_out_port_map == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_port_map = nullptr;
            if (p_input_port_map != nullptr)
            {
                const DasReadOnlyString         key{"executionInput"};
                Das::DasPtr<IDasReadOnlyString> artifact;
                if (DAS::IsOk(
                        p_input_port_map->GetString(key.Get(), artifact.Put()))
                    && artifact.Get() != nullptr)
                {
                    const char* utf8 = nullptr;
                    if (DAS::IsOk(artifact->GetUtf8(&utf8)) && utf8 != nullptr)
                    {
                        state_->received = true;
                        state_->execution_input_text = utf8;
                    }
                }
            }
            return DAS_S_OK;
        }

    private:
        std::shared_ptr<ReferenceChainState> state_;
        DasGuid                              guid_;
        std::atomic<uint32_t>                ref_count_{0};
    };

    // 固定响应的 authoring 会话替身（fake creator 的后端产物）。
    class ReferenceChainFakeSession final
        : public Das::PluginInterface::IDasTaskAuthoringSession
    {
    public:
        ReferenceChainFakeSession(
            std::string document_json,
            std::string compile_json)
            : document_json_(std::move(document_json)),
              compile_json_(std::move(compile_json))
        {
        }

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DasIidOf<IDasBase>())
            {
                *pp_out = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid
                == DasIidOf<Das::PluginInterface::IDasTaskAuthoringSession>())
            {
                *pp_out = static_cast<
                    Das::PluginInterface::IDasTaskAuthoringSession*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult GetGuid(DasGuid* p_out_guid) override
        {
            *p_out_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                std::string{REF_CHAIN_GRAPH_TASK_GUID});
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            return CreateIDasReadOnlyStringFromUtf8(
                "ReferenceChainFakeSession",
                pp_out_name);
        }

        DasResult GetDocument(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_document_json) override
        {
            if (pp_out_document_json == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_document_json =
                new Das::Core::Utils::IDasJsonImpl(document_json_.c_str());
            (*pp_out_document_json)->AddRef();
            return DAS_S_OK;
        }

        DasResult ApplyChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_result_json = nullptr;
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult Compile(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_result_json =
                new Das::Core::Utils::IDasJsonImpl(compile_json_.c_str());
            (*pp_out_result_json)->AddRef();
            return DAS_S_OK;
        }

    private:
        std::string           document_json_;
        std::string           compile_json_;
        std::atomic<uint32_t> ref_count_{0};
    };

    // fake creator：CreateTaskComponent 造记录组件（GraphTask/begin/end
    // 三 guid 通吃），CreateAuthoringSession 造固定响应会话。
    class ReferenceChainCreatorHost final
        : public Das::PluginInterface::DasTaskComponentCreatorImplBase<
              ReferenceChainCreatorHost>
    {
    public:
        ReferenceChainCreatorHost(
            std::shared_ptr<ReferenceChainState> state,
            std::string                          compile_json)
            : state_(std::move(state)), compile_json_(std::move(compile_json))
        {
        }

        DasResult CreateTaskComponent(
            const DasGuid&                            component_guid,
            Das::PluginInterface::IDasTaskComponent** pp_out_component) override
        {
            if (pp_out_component == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_component = nullptr;
            auto* component =
                new ExecutionInputRecorderComponent(state_, component_guid);
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }

        DasResult CreateAuthoringSession(
            const DasGuid&                                   task_guid,
            Das::ExportInterface::IDasJson*                  p_context_json,
            Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session)
            override
        {
            std::ignore = task_guid;
            std::ignore = p_context_json;
            if (pp_out_session == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            // 根 ports 自述空口面（被引条目文档根 ports 自述——消费文档
            // 自述的口面通道）。
            auto* session = new ReferenceChainFakeSession(
                R"({"document":{"viewKind":"graph","ports":[]},"diagnostics":[]})",
                compile_json_);
            session->AddRef();
            *pp_out_session = session;
            return DAS_S_OK;
        }

    private:
        std::shared_ptr<ReferenceChainState> state_;
        std::string                          compile_json_;
    };

    Das::DasPtr<IDasReadOnlyString> RefChainMakeText(const std::string& text)
    {
        Das::DasPtr<IDasReadOnlyString> ro;
        CreateIDasReadOnlyStringFromUtf8(text.c_str(), ro.Put());
        return ro;
    }

    std::string RefChainJsonText(Das::ExportInterface::IDasJson* json);

    // 三层嵌套真实链的中间层：GraphAuthoringSessionState 的薄包装（生产
    // DasGraphTask 会话同款管道——GetDocument/Compile 直转 Core C ABI、
    // state RAII 销毁）。其编译内部的注入 pass 经同一 creator 递归处理
    // 孙引用——creator 本尊单一（与执行侧对称，零新工厂机制）。
    class NestedChainRealSession final
        : public Das::PluginInterface::IDasTaskAuthoringSession
    {
    public:
        explicit NestedChainRealSession(GraphAuthoringSessionState* state)
            : state_(state)
        {
        }

        ~NestedChainRealSession()
        {
            DestroyGraphAuthoringSession(state_);
            state_ = nullptr;
        }

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DasIidOf<IDasBase>())
            {
                *pp_out = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid
                == DasIidOf<Das::PluginInterface::IDasTaskAuthoringSession>())
            {
                *pp_out = static_cast<
                    Das::PluginInterface::IDasTaskAuthoringSession*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult GetGuid(DasGuid* p_out_guid) override
        {
            *p_out_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                std::string{REF_CHAIN_GRAPH_TASK_GUID});
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            return CreateIDasReadOnlyStringFromUtf8(
                "NestedChainRealSession",
                pp_out_name);
        }

        DasResult GetDocument(
            Das::ExportInterface::IDasJson*  p_request_json,
            Das::ExportInterface::IDasJson** pp_out_document_json) override
        {
            if (pp_out_document_json == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            auto req = RefChainMakeText(RefChainJsonText(p_request_json));
            return GraphAuthoringSessionGetDocument(
                state_,
                req.Get(),
                pp_out_document_json);
        }

        DasResult ApplyChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json != nullptr)
            {
                *pp_out_result_json = nullptr;
            }
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult Compile(
            Das::ExportInterface::IDasJson*  p_request_json,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            auto req = RefChainMakeText(RefChainJsonText(p_request_json));
            return GraphAuthoringSessionCompile(
                state_,
                req.Get(),
                pp_out_result_json);
        }

    private:
        GraphAuthoringSessionState* state_ = nullptr;
        std::atomic<uint32_t>       ref_count_{0};
    };

    // 三层链 fake creator：按 context 的 repositoryEntryId 路由——子条目
    // （42）开真实 Core 会话（properties/actionCatalog/nodeFaces/继承闭包
    // 栈组 context，creator 传本尊=递归），孙条目（7）回固定 fake 会话。
    // 收到的 context 原文记录进 seen_contexts（闭包栈跨层传递的取证）。
    class NestedChainCreatorHost final
        : public Das::PluginInterface::DasTaskComponentCreatorImplBase<
              NestedChainCreatorHost>
    {
    public:
        NestedChainCreatorHost(
            std::shared_ptr<ReferenceChainState> state,
            std::string                          grandchild_compile_json,
            std::string                          child_properties_json,
            std::string                          catalog_json,
            std::string                          child_node_faces_json)
            : state_(std::move(state)),
              grandchild_compile_json_(std::move(grandchild_compile_json)),
              child_properties_json_(std::move(child_properties_json)),
              catalog_json_(std::move(catalog_json)),
              child_node_faces_json_(std::move(child_node_faces_json))
        {
        }

        DasResult CreateTaskComponent(
            const DasGuid&                            component_guid,
            Das::PluginInterface::IDasTaskComponent** pp_out_component) override
        {
            if (pp_out_component == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_component = nullptr;
            auto* component =
                new ExecutionInputRecorderComponent(state_, component_guid);
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }

        DasResult CreateAuthoringSession(
            const DasGuid&                                   task_guid,
            Das::ExportInterface::IDasJson*                  p_context_json,
            Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session)
            override
        {
            std::ignore = task_guid;
            if (pp_out_session == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_session = nullptr;

            const std::string context_text = RefChainJsonText(p_context_json);
            seen_contexts.push_back(context_text);

            // context 解析：repositoryEntryId 路由 + closureEntryIds 透传。
            std::optional<int64_t> entry_id;
            std::string            closure_ids_text;
            try
            {
                auto context = yyjson::read(context_text);
                auto obj = context.as_object();
                if (obj.has_value())
                {
                    if (obj->contains(std::string_view("repositoryEntryId")))
                    {
                        entry_id = (*obj)[std::string_view("repositoryEntryId")]
                                       .as_int();
                    }
                    if (obj->contains(std::string_view("closureEntryIds")))
                    {
                        closure_ids_text =
                            Das::Utils::SerializeYyjsonValue(
                                Das::Utils::CloneYyjsonValue((
                                    *obj)[std::string_view("closureEntryIds")]))
                                .value_or("[]");
                    }
                }
            }
            catch (...)
            {
                return DAS_E_INVALID_ARGUMENT;
            }
            if (!entry_id.has_value() || *entry_id <= 0)
            {
                return DAS_E_INVALID_ARGUMENT;
            }

            if (*entry_id == 42)
            {
                // 子条目：真实 Core 会话（context=被引条目文档 + catalog +
                // 边界口 faces + 继承闭包栈——生产后端 resolver 的
                // properties 预载同款合并语义）。creator 传本尊：子层编译
                // 的注入 pass 经同一 creator 递归处理孙引用。
                const std::string child_context =
                    std::string(R"({"properties":)") + child_properties_json_
                    + R"(,"actionCatalog":)" + catalog_json_
                    + R"(,"nodeFaces":)" + child_node_faces_json_
                    + R"(,"closureEntryIds":)" + closure_ids_text + "}";
                GraphAuthoringSessionState* child_state = nullptr;
                auto       ctx = RefChainMakeText(child_context);
                const auto hr =
                    CreateGraphAuthoringSession(this, ctx.Get(), &child_state);
                if (DAS::IsFailed(hr) || child_state == nullptr)
                {
                    return DAS::IsFailed(hr) ? hr : DAS_E_FAIL;
                }
                auto* session = new NestedChainRealSession(child_state);
                session->AddRef();
                *pp_out_session = session;
                return DAS_S_OK;
            }

            // 孙条目（7）：固定 fake 会话（根 ports 自述 + 固定产物）。
            auto* session = new ReferenceChainFakeSession(
                R"({"document":{"viewKind":"graph","ports":[{"id":"grand_out","type":"string","direction":"output"}]},"diagnostics":[]})",
                grandchild_compile_json_);
            session->AddRef();
            *pp_out_session = session;
            return DAS_S_OK;
        }

        std::vector<std::string> seen_contexts;

    private:
        std::shared_ptr<ReferenceChainState> state_;
        std::string                          grandchild_compile_json_;
        std::string                          child_properties_json_;
        std::string                          catalog_json_;
        std::string                          child_node_faces_json_;
    };

    std::string RefChainJsonText(Das::ExportInterface::IDasJson* json)
    {
        if (json == nullptr)
        {
            return {};
        }
        Das::DasPtr<IDasReadOnlyString> text;
        if (DAS::IsFailed(json->ToString(0, text.Put())) || !text)
        {
            return {};
        }
        const char* utf8 = nullptr;
        if (DAS::IsFailed(text->GetUtf8(&utf8)) || utf8 == nullptr)
        {
            return {};
        }
        return utf8;
    }
} // namespace

TEST_F(E2EGraphFlowTest, ReferenceClosureInjectionReachesExecutionInputPort)
{
    const auto state = std::make_shared<ReferenceChainState>();

    // 被引条目 Compile 产物：真实 {"fingerprint","plan"} shell（单节点子
    // 计划）——与生产 compile 段 executionInput 形态逐字同构。
    std::string child_shell_text;
    {
        CompiledGraphPlanDto child_plan;
        child_plan.source_fingerprint = "fp_ref_chain_child";
        CompiledNodeSnapshotDto child_node;
        child_node.node_id = "child_node";
        child_node.component_guid = std::string{REF_CHAIN_GRAPH_TASK_GUID};
        child_plan.node_snapshots.push_back(std::move(child_node));
        child_plan.execution_order = {"child_node"};
        child_shell_text =
            Das::Utils::SerializeYyjsonValue(
                BuildCompiledPlanShell(yyjson::object(child_plan)))
                .value_or("");
        ASSERT_FALSE(child_shell_text.empty());
    }
    // 被引条目 Compile 响应：{compile:{executionInput:<shell 串>},
    // diagnostics:[]}——经 yyjson 构造再序列化（shell 内引号自动转义）。
    std::string referenced_compile_json;
    {
        yyjson::value compile_response(Das::Utils::MakeYyjsonObject());
        auto          response_obj = compile_response.as_object();
        yyjson::value compile_section(Das::Utils::MakeYyjsonObject());
        (*compile_section.as_object())[std::string_view("executionInput")] =
            yyjson::value(child_shell_text);
        (*response_obj)[std::string_view("compile")] =
            std::move(compile_section);
        (*response_obj)[std::string_view("diagnostics")] =
            Das::Utils::MakeYyjsonArray();
        referenced_compile_json =
            Das::Utils::SerializeYyjsonValue(
                Das::Utils::CloneYyjsonValue(compile_response))
                .value_or("");
        ASSERT_FALSE(referenced_compile_json.empty());
    }

    auto creator =
        ReferenceChainCreatorHost::Make(state, referenced_compile_json);

    // 会话 context：actionCatalog 声明 GraphTask 组件的 taskRepositoryRef
    // 配置字段（repository_fields_ 的数据源——绑定判定的硬前置）。
    const std::string catalog =
        std::string(R"([{"actionId":")")
        + std::string{REF_CHAIN_GRAPH_TASK_GUID}
        + R"(","ports":[{"portId":"executionInput","portType":"object","direction":"input"}],)"
          R"("config":[{"name":"repositoryRef","type":"taskRepositoryRef","taskGuid":")"
        + std::string{REF_CHAIN_GRAPH_TASK_GUID} + R"("}]}])";
    const std::string context =
        std::string(R"({"actionCatalog":)") + catalog + "}";

    GraphAuthoringSessionState* session_state = nullptr;
    {
        auto ctx = RefChainMakeText(context);
        ASSERT_EQ(
            CreateGraphAuthoringSession(
                creator.Get(),
                ctx.Get(),
                &session_state),
            DAS_S_OK);
    }
    ASSERT_NE(session_state, nullptr);

    // 建绑条目节点 + 绑定 entryId 42（真实 authoring 变更流）。
    std::string bound_node_id;
    {
        Das::DasPtr<Das::ExportInterface::IDasJson> add_result;
        const auto                                  add_change =
            std::string(
                R"({"baseRevision":0,"kind":"addGraphNode","payload":{"actionId":")")
            + std::string{REF_CHAIN_GRAPH_TASK_GUID} + R"("}})";
        ASSERT_EQ(
            GraphAuthoringSessionApplyChange(
                session_state,
                RefChainMakeText(add_change).Get(),
                add_result.Put()),
            DAS_S_OK);
        const auto add_text = RefChainJsonText(add_result.Get());
        ASSERT_EQ(add_text.find("\"errorKind\""), std::string::npos)
            << add_text;

        // 定位新节点：acceptedProperties（DTO 形状，nodes 顶层）里
        // componentGuid 命中 GraphTask guid 的即目标（预置 begin/end
        // 之外的新增节点）。
        auto add_root = yyjson::read(add_text);
        auto add_obj = add_root.as_object();
        ASSERT_TRUE(add_obj.has_value());
        ASSERT_TRUE(add_obj->contains(std::string_view("acceptedProperties")));
        auto accepted = (*add_obj)[std::string_view("acceptedProperties")];
        auto accepted_obj = accepted.as_object();
        ASSERT_TRUE(accepted_obj.has_value());
        auto nodes = (*accepted_obj)[std::string_view("nodes")].as_array();
        ASSERT_TRUE(nodes.has_value());
        for (const auto& node : *nodes)
        {
            auto serialized = Das::Utils::SerializeYyjsonValue(
                                  Das::Utils::CloneYyjsonValue(node))
                                  .value_or("");
            if (serialized.find(std::string{REF_CHAIN_GRAPH_TASK_GUID})
                != std::string::npos)
            {
                auto node_obj = node.as_object();
                auto node_id =
                    (*node_obj)[std::string_view("nodeId")].as_string();
                ASSERT_TRUE(node_id.has_value());
                bound_node_id = *node_id;
                break;
            }
        }
        ASSERT_FALSE(bound_node_id.empty()) << add_text;
    }
    {
        Das::DasPtr<Das::ExportInterface::IDasJson> set_result;
        const auto                                  set_change =
            std::string(
                R"({"baseRevision":1,"kind":"setValue","payload":{"target":{"kind":"graphNode","id":")")
            + bound_node_id + R"("},"fieldName":"repositoryRef","value":42}})";
        ASSERT_EQ(
            GraphAuthoringSessionApplyChange(
                session_state,
                RefChainMakeText(set_change).Get(),
                set_result.Put()),
            DAS_S_OK);
        const auto set_text = RefChainJsonText(set_result.Get());
        ASSERT_EQ(set_text.find("\"errorKind\""), std::string::npos)
            << set_text;
        // 绑定落盘：节点 settings 携带 repositoryRef 字段（正整数值形态
        // 由注入 pass 的触发与否在编译断言端验证——pass 判据即正值）。
        ASSERT_NE(set_text.find("\"repositoryRef\""), std::string::npos)
            << set_text;
    }

    // 编译：注入 pass 产物广播绑定进 plan.binding_plan。
    std::string parent_shell_text;
    {
        Das::DasPtr<Das::ExportInterface::IDasJson> compiled;
        ASSERT_EQ(
            GraphAuthoringSessionCompile(
                session_state,
                nullptr,
                compiled.Put()),
            DAS_S_OK);
        const auto envelope_text = RefChainJsonText(compiled.Get());
        auto       envelope = yyjson::read(envelope_text);
        auto       envelope_obj = envelope.as_object();
        ASSERT_TRUE(envelope_obj.has_value());
        auto compile_obj =
            (*envelope_obj)[std::string_view("compile")].as_object();
        ASSERT_TRUE(compile_obj.has_value());
        const auto& execution_input =
            (*compile_obj)[std::string_view("executionInput")];
        // 会话编译出口的 executionInput 为 shell 对象形态——序列化为
        // 文本（与注入 pass 的 default_value 消费形态一致）。
        if (const auto text = execution_input.as_string())
        {
            parent_shell_text = std::string(*text);
        }
        else
        {
            parent_shell_text =
                Das::Utils::SerializeYyjsonValue(
                    Das::Utils::CloneYyjsonValue(execution_input))
                    .value_or("");
        }
        ASSERT_FALSE(parent_shell_text.empty());
    }
    DestroyGraphAuthoringSession(session_state);

    // 解析父 shell：断言 binding_plan 含产物广播绑定（$graph_input →
    // bound 节点 executionInput，default_value=compile 段原文）。
    std::string parent_fingerprint;
    {
        auto shell = yyjson::read(parent_shell_text);
        auto shell_obj = shell.as_object();
        ASSERT_TRUE(shell_obj.has_value());
        auto fingerprint =
            (*shell_obj)[std::string_view("fingerprint")].as_string();
        ASSERT_TRUE(fingerprint.has_value());
        parent_fingerprint = *fingerprint;

        auto plan_value = (*shell_obj)[std::string_view("plan")];
        auto plan_obj = plan_value.as_object();
        ASSERT_TRUE(plan_obj.has_value());
        // DTO 序列化为 camelCase 键（PFR 字段名驼峰化——CompiledArtifactTest
        // 契约）。
        auto binding_plan =
            (*plan_obj)[std::string_view("bindingPlan")].as_object();
        ASSERT_TRUE(binding_plan.has_value());
        auto bindings =
            (*binding_plan)[std::string_view("bindings")].as_array();
        ASSERT_TRUE(bindings.has_value());
        bool found_broadcast = false;
        for (const auto& binding : *bindings)
        {
            auto binding_obj = binding.as_object();
            if (!binding_obj.has_value())
            {
                continue;
            }
            auto source =
                (*binding_obj)[std::string_view("sourceNodeId")].as_string();
            auto target_node =
                (*binding_obj)[std::string_view("targetNodeId")].as_string();
            auto target_port =
                (*binding_obj)[std::string_view("targetPortId")].as_string();
            if (!source.has_value() || *source != "$graph_input"
                || !target_node.has_value() || *target_node != bound_node_id
                || !target_port.has_value() || *target_port != "executionInput")
            {
                continue;
            }
            auto default_value =
                (*binding_obj)[std::string_view("defaultValue")].as_string();
            ASSERT_TRUE(default_value.has_value());
            EXPECT_EQ(*default_value, child_shell_text);
            found_broadcast = true;
        }
        EXPECT_TRUE(found_broadcast)
            << "compiled plan must carry the reference artifact broadcast "
               "binding";
    }

    // 执行：物化路径（DoAdapter $graph_input 广播）把产物送到绑条目节点
    // 的 executionInput 口——记录组件收到 compile 段原文。
    yyjson::value parent_plan_value;
    {
        auto shell_root = yyjson::read(parent_shell_text);
        auto shell_obj = shell_root.as_object();
        ASSERT_TRUE(shell_obj.has_value());
        parent_plan_value = Das::Utils::CloneYyjsonValue(
            (*shell_obj)[std::string_view("plan")]);
    }
    const auto parent_plan =
        yyjson::cast<CompiledGraphPlanDto>(parent_plan_value);
    GraphRuntime runtime;
    const auto   hr = runtime.RunWithHost(
        parent_plan,
        parent_fingerprint,
        token_.Get(),
        creator.Get());
    ASSERT_EQ(hr, DAS_S_OK) << runtime.GetLastErrorMessage();

    ASSERT_TRUE(state->received)
        << "the bound GraphTask node must receive executionInput";
    EXPECT_EQ(state->execution_input_text, child_shell_text);
}

// 三层嵌套真实链：父图 GraphTask 节点绑子条目 42（真实 Core 会话），子图
// 又含 GraphTask 节点绑孙条目 7（固定 fake）→ 编译期逐层递归（闭包栈经
// context 传递：父 [42] → 孙 [42,7]），孙引用产物逐层上浮进父 plan 的广
// 播绑定链，执行绿灯（记录组件收到子层产物）。
TEST_F(E2EGraphFlowTest, NestedReferenceClosureCompilesRecursively)
{
    const auto state = std::make_shared<ReferenceChainState>();

    // 孙条目 Compile 产物：固定 shell 串（真实 {"fingerprint","plan"} 形
    // 态，单节点计划）。
    std::string grandchild_shell_text;
    {
        CompiledGraphPlanDto grandchild_plan;
        grandchild_plan.source_fingerprint = "fp_nested_grandchild";
        CompiledNodeSnapshotDto grandchild_node;
        grandchild_node.node_id = "grandchild_node";
        grandchild_node.component_guid = std::string{REF_CHAIN_GRAPH_TASK_GUID};
        grandchild_plan.node_snapshots.push_back(std::move(grandchild_node));
        grandchild_plan.execution_order = {"grandchild_node"};
        grandchild_shell_text =
            Das::Utils::SerializeYyjsonValue(
                BuildCompiledPlanShell(yyjson::object(grandchild_plan)))
                .value_or("");
        ASSERT_FALSE(grandchild_shell_text.empty());
    }
    std::string grandchild_compile_json;
    {
        yyjson::value compile_response(Das::Utils::MakeYyjsonObject());
        auto          response_obj = compile_response.as_object();
        yyjson::value compile_section(Das::Utils::MakeYyjsonObject());
        (*compile_section.as_object())[std::string_view("executionInput")] =
            yyjson::value(grandchild_shell_text);
        (*response_obj)[std::string_view("compile")] =
            std::move(compile_section);
        (*response_obj)[std::string_view("diagnostics")] =
            Das::Utils::MakeYyjsonArray();
        grandchild_compile_json =
            Das::Utils::SerializeYyjsonValue(
                Das::Utils::CloneYyjsonValue(compile_response))
                .value_or("");
        ASSERT_FALSE(grandchild_compile_json.empty());
    }

    // 子条目文档（raw graph doc）：begin + GraphTask 节点（绑孙条目 7）+
    // end。end 节点携带 nodeFaces（child_out 输出口）——图视图投影根
    // ports section 的发射条件（faces 缺席时整 section 缺省，注入 pass 会
    // 把子文档判为缺 root ports）。
    constexpr std::string_view child_properties = R"json({
        "documentId": "nested_child",
        "nodes": [
            {"nodeId": "cb", "target": {"targetKind": "componentRef", "componentRef": {"kind": "componentRef", "componentGuid": "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E", "pluginGuid": "{p}"}}},
            {"nodeId": "c_task", "target": {"targetKind": "componentRef", "componentRef": {"kind": "componentRef", "componentGuid": ")json";
    const std::string          child_properties_json =
        std::string{child_properties} + std::string{REF_CHAIN_GRAPH_TASK_GUID}
        + R"json(", "pluginGuid": "{p}"}}, "settings": {"repositoryRef": 7}},
            {"nodeId": "ce", "target": {"targetKind": "componentRef", "componentRef": {"kind": "componentRef", "componentGuid": "E1B7C9D3-5F4A-4E2B-8D6C-7A9E1F3B5C2D", "pluginGuid": "{p}"}}}
        ],
        "edges": []
    })json";
    constexpr std::string_view child_node_faces =
        R"json([{"nodeId": "ce", "ports": [{"id": "child_out", "type": "string", "direction": "output"}]}])json";

    // catalog：GraphTask 组件声明 taskRepositoryRef 配置字段 +
    // executionInput 端口（子层绑定判定的数据源）。
    const std::string catalog =
        std::string(R"([{"actionId":")")
        + std::string{REF_CHAIN_GRAPH_TASK_GUID}
        + R"(","ports":[{"portId":"executionInput","portType":"object","direction":"input"}],)"
          R"("config":[{"name":"repositoryRef","type":"taskRepositoryRef","taskGuid":")"
        + std::string{REF_CHAIN_GRAPH_TASK_GUID} + R"("}]}])";

    auto creator = NestedChainCreatorHost::MakeImpl(
        state,
        grandchild_compile_json,
        child_properties_json,
        catalog,
        std::string{child_node_faces});

    // 父会话：真实 authoring 变更流建绑条目节点（entryId 42）。
    const std::string parent_context =
        std::string(R"({"actionCatalog":)") + catalog + "}";
    GraphAuthoringSessionState* session_state = nullptr;
    {
        auto ctx = RefChainMakeText(parent_context);
        ASSERT_EQ(
            CreateGraphAuthoringSession(
                creator.Get(),
                ctx.Get(),
                &session_state),
            DAS_S_OK);
    }
    ASSERT_NE(session_state, nullptr);

    std::string bound_node_id;
    {
        Das::DasPtr<Das::ExportInterface::IDasJson> add_result;
        const auto                                  add_change =
            std::string(
                R"({"baseRevision":0,"kind":"addGraphNode","payload":{"actionId":")")
            + std::string{REF_CHAIN_GRAPH_TASK_GUID} + R"("}})";
        ASSERT_EQ(
            GraphAuthoringSessionApplyChange(
                session_state,
                RefChainMakeText(add_change).Get(),
                add_result.Put()),
            DAS_S_OK);
        const auto add_text = RefChainJsonText(add_result.Get());
        ASSERT_EQ(add_text.find("\"errorKind\""), std::string::npos)
            << add_text;
        auto add_root = yyjson::read(add_text);
        auto add_obj = add_root.as_object();
        ASSERT_TRUE(add_obj.has_value());
        ASSERT_TRUE(add_obj->contains(std::string_view("acceptedProperties")));
        auto accepted = (*add_obj)[std::string_view("acceptedProperties")];
        auto accepted_obj = accepted.as_object();
        ASSERT_TRUE(accepted_obj.has_value());
        auto nodes = (*accepted_obj)[std::string_view("nodes")].as_array();
        ASSERT_TRUE(nodes.has_value());
        for (const auto& node : *nodes)
        {
            auto serialized = Das::Utils::SerializeYyjsonValue(
                                  Das::Utils::CloneYyjsonValue(node))
                                  .value_or("");
            if (serialized.find(std::string{REF_CHAIN_GRAPH_TASK_GUID})
                != std::string::npos)
            {
                auto node_obj = node.as_object();
                auto node_id =
                    (*node_obj)[std::string_view("nodeId")].as_string();
                ASSERT_TRUE(node_id.has_value());
                bound_node_id = *node_id;
                break;
            }
        }
        ASSERT_FALSE(bound_node_id.empty()) << add_text;
    }
    {
        Das::DasPtr<Das::ExportInterface::IDasJson> set_result;
        const auto                                  set_change =
            std::string(
                R"({"baseRevision":1,"kind":"setValue","payload":{"target":{"kind":"graphNode","id":")")
            + bound_node_id + R"("},"fieldName":"repositoryRef","value":42}})";
        ASSERT_EQ(
            GraphAuthoringSessionApplyChange(
                session_state,
                RefChainMakeText(set_change).Get(),
                set_result.Put()),
            DAS_S_OK);
        const auto set_text = RefChainJsonText(set_result.Get());
        ASSERT_EQ(set_text.find("\"errorKind\""), std::string::npos)
            << set_text;
    }

    // 父编译：注入 pass 逐层递归（子会话编译内部再走注入 pass 处理孙引
    // 用），产物逐层上浮。
    std::string parent_shell_text;
    {
        Das::DasPtr<Das::ExportInterface::IDasJson> compiled;
        ASSERT_EQ(
            GraphAuthoringSessionCompile(
                session_state,
                nullptr,
                compiled.Put()),
            DAS_S_OK);
        const auto envelope_text = RefChainJsonText(compiled.Get());
        auto       envelope = yyjson::read(envelope_text);
        auto       envelope_obj = envelope.as_object();
        ASSERT_TRUE(envelope_obj.has_value());
        auto compile_obj =
            (*envelope_obj)[std::string_view("compile")].as_object();
        ASSERT_TRUE(compile_obj.has_value());
        const auto& execution_input =
            (*compile_obj)[std::string_view("executionInput")];
        if (const auto text = execution_input.as_string())
        {
            parent_shell_text = std::string(*text);
        }
        else
        {
            parent_shell_text =
                Das::Utils::SerializeYyjsonValue(
                    Das::Utils::CloneYyjsonValue(execution_input))
                    .value_or("");
        }
        ASSERT_FALSE(parent_shell_text.empty()) << envelope_text;
    }
    DestroyGraphAuthoringSession(session_state);

    // 闭包栈跨层传递取证：子条目（42）context 携带 [42]，孙条目（7）
    // context 携带 [42,7]（结构化断言，不依赖序列化空白形态）。
    ASSERT_EQ(creator->seen_contexts.size(), 2u);
    const auto closure_ids_of =
        [](const std::string& context_text) -> std::vector<int64_t>
    {
        auto parsed = yyjson::read(context_text);
        auto obj = parsed.as_object();
        if (!obj.has_value()
            || !obj->contains(std::string_view("closureEntryIds")))
        {
            return {};
        }
        auto arr = (*obj)[std::string_view("closureEntryIds")].as_array();
        if (!arr.has_value())
        {
            return {};
        }
        std::vector<int64_t> ids;
        for (const auto& id_value : *arr)
        {
            ids.push_back(id_value.as_int().value_or(0));
        }
        return ids;
    };
    {
        const auto child_ids = closure_ids_of(creator->seen_contexts[0]);
        ASSERT_EQ(child_ids.size(), 1u) << creator->seen_contexts[0];
        EXPECT_EQ(child_ids[0], 42) << creator->seen_contexts[0];
    }
    {
        const auto grandchild_ids = closure_ids_of(creator->seen_contexts[1]);
        ASSERT_EQ(grandchild_ids.size(), 2u) << creator->seen_contexts[1];
        EXPECT_EQ(grandchild_ids[0], 42) << creator->seen_contexts[1];
        EXPECT_EQ(grandchild_ids[1], 7) << creator->seen_contexts[1];
    }

    // 父 shell：绑条目节点的产物广播绑定 default_value=子层编译产物。
    std::string child_shell_text;
    std::string parent_fingerprint;
    {
        auto shell = yyjson::read(parent_shell_text);
        auto shell_obj = shell.as_object();
        ASSERT_TRUE(shell_obj.has_value());
        auto fingerprint =
            (*shell_obj)[std::string_view("fingerprint")].as_string();
        ASSERT_TRUE(fingerprint.has_value());
        parent_fingerprint = *fingerprint;
        auto plan_value = (*shell_obj)[std::string_view("plan")];
        auto plan_obj = plan_value.as_object();
        ASSERT_TRUE(plan_obj.has_value());
        auto binding_plan =
            (*plan_obj)[std::string_view("bindingPlan")].as_object();
        ASSERT_TRUE(binding_plan.has_value());
        auto bindings =
            (*binding_plan)[std::string_view("bindings")].as_array();
        ASSERT_TRUE(bindings.has_value());
        bool found_broadcast = false;
        for (const auto& binding : *bindings)
        {
            auto binding_obj = binding.as_object();
            if (!binding_obj.has_value())
            {
                continue;
            }
            auto source =
                (*binding_obj)[std::string_view("sourceNodeId")].as_string();
            auto target_node =
                (*binding_obj)[std::string_view("targetNodeId")].as_string();
            auto target_port =
                (*binding_obj)[std::string_view("targetPortId")].as_string();
            if (!source.has_value() || *source != "$graph_input"
                || !target_node.has_value() || *target_node != bound_node_id
                || !target_port.has_value() || *target_port != "executionInput")
            {
                continue;
            }
            auto default_value =
                (*binding_obj)[std::string_view("defaultValue")].as_string();
            ASSERT_TRUE(default_value.has_value());
            child_shell_text = *default_value;
            found_broadcast = true;
        }
        ASSERT_TRUE(found_broadcast)
            << "parent plan must carry the child artifact broadcast binding";
    }
    ASSERT_FALSE(child_shell_text.empty());

    // 子 shell 内层：孙引用产物广播绑定（c_task.executionInput 的
    // default_value=孙层产物）——产物逐层上浮的物证。
    {
        auto shell = yyjson::read(child_shell_text);
        auto shell_obj = shell.as_object();
        ASSERT_TRUE(shell_obj.has_value());
        auto plan_value = (*shell_obj)[std::string_view("plan")];
        auto plan_obj = plan_value.as_object();
        ASSERT_TRUE(plan_obj.has_value());
        auto binding_plan =
            (*plan_obj)[std::string_view("bindingPlan")].as_object();
        ASSERT_TRUE(binding_plan.has_value());
        auto bindings =
            (*binding_plan)[std::string_view("bindings")].as_array();
        ASSERT_TRUE(bindings.has_value());
        bool found_grandchild_broadcast = false;
        for (const auto& binding : *bindings)
        {
            auto binding_obj = binding.as_object();
            if (!binding_obj.has_value())
            {
                continue;
            }
            auto target_node =
                (*binding_obj)[std::string_view("targetNodeId")].as_string();
            auto target_port =
                (*binding_obj)[std::string_view("targetPortId")].as_string();
            if (!target_node.has_value() || *target_node != "c_task"
                || !target_port.has_value() || *target_port != "executionInput")
            {
                continue;
            }
            auto default_value =
                (*binding_obj)[std::string_view("defaultValue")].as_string();
            ASSERT_TRUE(default_value.has_value());
            EXPECT_EQ(*default_value, grandchild_shell_text);
            found_grandchild_broadcast = true;
        }
        EXPECT_TRUE(found_grandchild_broadcast)
            << "child plan must carry the grandchild artifact broadcast "
               "binding";
    }

    // 执行绿灯：父 plan 物化路径把子层产物送到绑条目节点的
    // executionInput 口。
    yyjson::value parent_plan_value;
    {
        auto shell_root = yyjson::read(parent_shell_text);
        auto shell_obj = shell_root.as_object();
        ASSERT_TRUE(shell_obj.has_value());
        parent_plan_value = Das::Utils::CloneYyjsonValue(
            (*shell_obj)[std::string_view("plan")]);
    }
    const auto parent_plan =
        yyjson::cast<CompiledGraphPlanDto>(parent_plan_value);
    GraphRuntime runtime;
    const auto   hr = runtime.RunWithHost(
        parent_plan,
        parent_fingerprint,
        token_.Get(),
        creator.Get());
    ASSERT_EQ(hr, DAS_S_OK) << runtime.GetLastErrorMessage();
    ASSERT_TRUE(state->received)
        << "the bound GraphTask node must receive the child artifact";
    EXPECT_EQ(state->execution_input_text, child_shell_text);
}

// ===========================================================================
// 执行时编译 E2E：绑条目节点的产物投递通道=GraphRuntime 在节点执行期间经
// creator 现编——计数器在 Run 开始前快照、Run 结束后增量 > 0（时点取证，
// 与编译期注入 pass 的调用区分）；执行期 context 携 repositoryEntryId 与
// 闭包栈（顶层图空栈+本条目）；组件 Do 照旧从 executionInput 键读产物。
// ===========================================================================
namespace
{
    // 执行时编译探针：creator 收到的调用计数与 context 原文（按调用次序）。
    struct ExecutionTimeProbeCounters
    {
        std::atomic<int>         session_calls{0};
        std::atomic<int>         compile_calls{0};
        std::vector<std::string> contexts;
    };

    // 计数会话替身：Compile 递增计数后回固定产物。
    class ExecutionTimeProbeSession final
        : public Das::PluginInterface::IDasTaskAuthoringSession
    {
    public:
        ExecutionTimeProbeSession(
            std::shared_ptr<ExecutionTimeProbeCounters> counters,
            std::string                                 compile_json)
            : counters_(std::move(counters)),
              compile_json_(std::move(compile_json))
        {
        }

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (iid == DasIidOf<IDasBase>())
            {
                *pp_out = static_cast<IDasBase*>(this);
                AddRef();
                return DAS_S_OK;
            }
            if (iid
                == DasIidOf<Das::PluginInterface::IDasTaskAuthoringSession>())
            {
                *pp_out = static_cast<
                    Das::PluginInterface::IDasTaskAuthoringSession*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult GetGuid(DasGuid* p_out_guid) override
        {
            *p_out_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                std::string{REF_CHAIN_GRAPH_TASK_GUID});
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            return CreateIDasReadOnlyStringFromUtf8(
                "ExecutionTimeProbeSession",
                pp_out_name);
        }

        DasResult GetDocument(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_document_json) override
        {
            if (pp_out_document_json == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_document_json = new Das::Core::Utils::IDasJsonImpl(
                R"({"document":{"viewKind":"graph","ports":[]},"diagnostics":[]})");
            (*pp_out_document_json)->AddRef();
            return DAS_S_OK;
        }

        DasResult ApplyChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json != nullptr)
            {
                *pp_out_result_json = nullptr;
            }
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult Compile(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            ++counters_->compile_calls;
            *pp_out_result_json =
                new Das::Core::Utils::IDasJsonImpl(compile_json_.c_str());
            (*pp_out_result_json)->AddRef();
            return DAS_S_OK;
        }

    private:
        std::shared_ptr<ExecutionTimeProbeCounters> counters_;
        std::string                                 compile_json_;
        std::atomic<uint32_t>                       ref_count_{0};
    };

    // 探针 creator：CreateTaskComponent 造记录组件，
    // CreateAuthoringSession 递增计数并记录 context 原文。
    class ExecutionTimeProbeHost final
        : public Das::PluginInterface::DasTaskComponentCreatorImplBase<
              ExecutionTimeProbeHost>
    {
    public:
        ExecutionTimeProbeHost(
            std::shared_ptr<ReferenceChainState>        state,
            std::shared_ptr<ExecutionTimeProbeCounters> counters,
            std::string                                 compile_json)
            : state_(std::move(state)), counters_(std::move(counters)),
              compile_json_(std::move(compile_json))
        {
        }

        DasResult CreateTaskComponent(
            const DasGuid&                            component_guid,
            Das::PluginInterface::IDasTaskComponent** pp_out_component) override
        {
            if (pp_out_component == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_component = nullptr;
            auto* component =
                new ExecutionInputRecorderComponent(state_, component_guid);
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }

        DasResult CreateAuthoringSession(
            const DasGuid&                                   task_guid,
            Das::ExportInterface::IDasJson*                  p_context_json,
            Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session)
            override
        {
            std::ignore = task_guid;
            if (pp_out_session == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_session = nullptr;
            ++counters_->session_calls;
            counters_->contexts.push_back(RefChainJsonText(p_context_json));
            auto* session =
                new ExecutionTimeProbeSession(counters_, compile_json_);
            session->AddRef();
            *pp_out_session = session;
            return DAS_S_OK;
        }

    private:
        std::shared_ptr<ReferenceChainState>        state_;
        std::shared_ptr<ExecutionTimeProbeCounters> counters_;
        std::string                                 compile_json_;
    };
} // namespace

TEST_F(E2EGraphFlowTest, ExecutionTimeReferenceCompileReachesComponentPort)
{
    const auto state = std::make_shared<ReferenceChainState>();
    const auto counters = std::make_shared<ExecutionTimeProbeCounters>();

    // 被引条目 Compile 产物：真实 {"fingerprint","plan"} shell（单节点子计
    // 划）——编译期注入与执行期现编共用同一固定产物（内容确定性一致）。
    std::string child_shell_text;
    {
        CompiledGraphPlanDto child_plan;
        child_plan.source_fingerprint = "fp_execution_time_child";
        CompiledNodeSnapshotDto child_node;
        child_node.node_id = "child_node";
        child_node.component_guid = std::string{REF_CHAIN_GRAPH_TASK_GUID};
        child_plan.node_snapshots.push_back(std::move(child_node));
        child_plan.execution_order = {"child_node"};
        child_shell_text =
            Das::Utils::SerializeYyjsonValue(
                BuildCompiledPlanShell(yyjson::object(child_plan)))
                .value_or("");
        ASSERT_FALSE(child_shell_text.empty());
    }
    std::string referenced_compile_json;
    {
        yyjson::value compile_response(Das::Utils::MakeYyjsonObject());
        auto          response_obj = compile_response.as_object();
        yyjson::value compile_section(Das::Utils::MakeYyjsonObject());
        (*compile_section.as_object())[std::string_view("executionInput")] =
            yyjson::value(child_shell_text);
        (*response_obj)[std::string_view("compile")] =
            std::move(compile_section);
        (*response_obj)[std::string_view("diagnostics")] =
            Das::Utils::MakeYyjsonArray();
        referenced_compile_json =
            Das::Utils::SerializeYyjsonValue(
                Das::Utils::CloneYyjsonValue(compile_response))
                .value_or("");
        ASSERT_FALSE(referenced_compile_json.empty());
    }

    auto creator =
        ExecutionTimeProbeHost::Make(state, counters, referenced_compile_json);

    // 会话 context：actionCatalog 声明 GraphTask 组件的 taskRepositoryRef
    // 配置字段（绑条目判定的数据源）。
    const std::string catalog =
        std::string(R"([{"actionId":")")
        + std::string{REF_CHAIN_GRAPH_TASK_GUID}
        + R"(","ports":[{"portId":"executionInput","portType":"object","direction":"input"}],)"
          R"("config":[{"name":"repositoryRef","type":"taskRepositoryRef","taskGuid":")"
        + std::string{REF_CHAIN_GRAPH_TASK_GUID} + R"("}]}])";
    const std::string context =
        std::string(R"({"actionCatalog":)") + catalog + "}";

    GraphAuthoringSessionState* session_state = nullptr;
    {
        auto ctx = RefChainMakeText(context);
        ASSERT_EQ(
            CreateGraphAuthoringSession(
                creator.Get(),
                ctx.Get(),
                &session_state),
            DAS_S_OK);
    }
    ASSERT_NE(session_state, nullptr);

    // 建绑条目节点 + 绑定 entryId 42（真实 authoring 变更流）。
    std::string bound_node_id;
    {
        Das::DasPtr<Das::ExportInterface::IDasJson> add_result;
        const auto                                  add_change =
            std::string(
                R"({"baseRevision":0,"kind":"addGraphNode","payload":{"actionId":")")
            + std::string{REF_CHAIN_GRAPH_TASK_GUID} + R"("}})";
        ASSERT_EQ(
            GraphAuthoringSessionApplyChange(
                session_state,
                RefChainMakeText(add_change).Get(),
                add_result.Put()),
            DAS_S_OK);
        const auto add_text = RefChainJsonText(add_result.Get());
        ASSERT_EQ(add_text.find("\"errorKind\""), std::string::npos)
            << add_text;
        auto add_root = yyjson::read(add_text);
        auto add_obj = add_root.as_object();
        ASSERT_TRUE(add_obj.has_value());
        ASSERT_TRUE(add_obj->contains(std::string_view("acceptedProperties")));
        auto accepted = (*add_obj)[std::string_view("acceptedProperties")];
        auto accepted_obj = accepted.as_object();
        ASSERT_TRUE(accepted_obj.has_value());
        auto nodes = (*accepted_obj)[std::string_view("nodes")].as_array();
        ASSERT_TRUE(nodes.has_value());
        for (const auto& node : *nodes)
        {
            auto serialized = Das::Utils::SerializeYyjsonValue(
                                  Das::Utils::CloneYyjsonValue(node))
                                  .value_or("");
            if (serialized.find(std::string{REF_CHAIN_GRAPH_TASK_GUID})
                != std::string::npos)
            {
                auto node_obj = node.as_object();
                auto node_id =
                    (*node_obj)[std::string_view("nodeId")].as_string();
                ASSERT_TRUE(node_id.has_value());
                bound_node_id = *node_id;
                break;
            }
        }
        ASSERT_FALSE(bound_node_id.empty()) << add_text;
    }
    {
        Das::DasPtr<Das::ExportInterface::IDasJson> set_result;
        const auto                                  set_change =
            std::string(
                R"({"baseRevision":1,"kind":"setValue","payload":{"target":{"kind":"graphNode","id":")")
            + bound_node_id + R"("},"fieldName":"repositoryRef","value":42}})";
        ASSERT_EQ(
            GraphAuthoringSessionApplyChange(
                session_state,
                RefChainMakeText(set_change).Get(),
                set_result.Put()),
            DAS_S_OK);
        const auto set_text = RefChainJsonText(set_result.Get());
        ASSERT_EQ(set_text.find("\"errorKind\""), std::string::npos)
            << set_text;
    }

    // 编译父图（编译期注入链照旧产出注入绑定），取 shell 供执行。
    std::string parent_shell_text;
    std::string parent_fingerprint;
    {
        Das::DasPtr<Das::ExportInterface::IDasJson> compiled;
        ASSERT_EQ(
            GraphAuthoringSessionCompile(
                session_state,
                nullptr,
                compiled.Put()),
            DAS_S_OK);
        const auto envelope_text = RefChainJsonText(compiled.Get());
        auto       envelope = yyjson::read(envelope_text);
        auto       envelope_obj = envelope.as_object();
        ASSERT_TRUE(envelope_obj.has_value());
        auto compile_obj =
            (*envelope_obj)[std::string_view("compile")].as_object();
        ASSERT_TRUE(compile_obj.has_value());
        const auto& execution_input =
            (*compile_obj)[std::string_view("executionInput")];
        if (const auto text = execution_input.as_string())
        {
            parent_shell_text = std::string(*text);
        }
        else
        {
            parent_shell_text =
                Das::Utils::SerializeYyjsonValue(
                    Das::Utils::CloneYyjsonValue(execution_input))
                    .value_or("");
        }
        ASSERT_FALSE(parent_shell_text.empty()) << envelope_text;
        auto shell = yyjson::read(parent_shell_text);
        auto shell_obj = shell.as_object();
        ASSERT_TRUE(shell_obj.has_value());
        auto fingerprint =
            (*shell_obj)[std::string_view("fingerprint")].as_string();
        ASSERT_TRUE(fingerprint.has_value());
        parent_fingerprint = *fingerprint;
    }
    DestroyGraphAuthoringSession(session_state);

    yyjson::value parent_plan_value;
    {
        auto shell_root = yyjson::read(parent_shell_text);
        auto shell_obj = shell_root.as_object();
        ASSERT_TRUE(shell_obj.has_value());
        parent_plan_value = Das::Utils::CloneYyjsonValue(
            (*shell_obj)[std::string_view("plan")]);
    }
    const auto parent_plan =
        yyjson::cast<CompiledGraphPlanDto>(parent_plan_value);

    // Run 前快照：编译期的 creator 调用不计入执行期取证。
    const auto session_calls_before = counters->session_calls.load();
    const auto compile_calls_before = counters->compile_calls.load();
    const auto contexts_before = counters->contexts.size();

    GraphRuntime runtime;
    const auto   hr = runtime.RunWithHost(
        parent_plan,
        parent_fingerprint,
        token_.Get(),
        creator.Get());
    ASSERT_EQ(hr, DAS_S_OK) << runtime.GetLastErrorMessage();

    // 执行期现编取证：Run 期间 creator 收到 CreateAuthoringSession 与
    // Compile（产物经协议保留键达组件 Do）。
    const auto session_calls_delta =
        counters->session_calls.load() - session_calls_before;
    const auto compile_calls_delta =
        counters->compile_calls.load() - compile_calls_before;
    EXPECT_GT(session_calls_delta, 0)
        << "creator must receive CreateAuthoringSession during Run";
    EXPECT_GT(compile_calls_delta, 0)
        << "entry session must receive Compile during Run";
    ASSERT_GT(counters->contexts.size(), contexts_before);

    // 执行期 context：repositoryEntryId=42 + 闭包栈=[42]（顶层图空栈+本
    // 条目——子层环检测经它恢复祖先链）。
    {
        const auto& context_text = counters->contexts.back();
        auto        parsed = yyjson::read(context_text);
        auto        obj = parsed.as_object();
        ASSERT_TRUE(obj.has_value()) << context_text;
        ASSERT_TRUE(obj->contains(std::string_view("repositoryEntryId")))
            << context_text;
        EXPECT_EQ(
            (*obj)[std::string_view("repositoryEntryId")].as_int().value_or(0),
            42)
            << context_text;
        ASSERT_TRUE(obj->contains(std::string_view("closureEntryIds")))
            << context_text;
        auto ids = (*obj)[std::string_view("closureEntryIds")].as_array();
        ASSERT_TRUE(ids.has_value()) << context_text;
        ASSERT_EQ(ids->size(), 1u) << context_text;
        EXPECT_EQ(ids->begin()->as_int().value_or(0), 42) << context_text;
    }

    ASSERT_TRUE(state->received)
        << "the bound GraphTask node must receive executionInput";
    EXPECT_EQ(state->execution_input_text, child_shell_text);
}

// 记档字段序列化往返：referenceEntries 与 closureEntryIds 经
// DAS_GRAPHRUNTIME_DTO_CASTER 序列化/反序列化存活（DasGraphTask 的 plan
// JSON 通路钉住——全字段键由序列化侧产出保证齐备）。
TEST_F(E2EGraphFlowTest, ReferenceRecordFieldsRoundTripThroughPlanJson)
{
    CompiledGraphPlanDto plan;
    plan.source_fingerprint = "fp_reference_record_roundtrip";
    plan.reference_entries.push_back({"n_bound", 42});
    plan.reference_entries.push_back({"n_bound_two", 7});
    plan.closure_entry_ids = {41, 42};

    const auto serialized =
        Das::Utils::SerializeYyjsonValue(yyjson::object(plan));
    ASSERT_TRUE(serialized.has_value());
    const auto parsed = Das::Utils::ParseYyjsonFromString(*serialized);
    ASSERT_TRUE(parsed.has_value());
    const auto round_tripped = yyjson::cast<CompiledGraphPlanDto>(*parsed);

    ASSERT_EQ(round_tripped.reference_entries.size(), 2u);
    EXPECT_EQ(round_tripped.reference_entries[0].node_id, "n_bound");
    EXPECT_EQ(round_tripped.reference_entries[0].entry_id, 42);
    EXPECT_EQ(round_tripped.reference_entries[1].node_id, "n_bound_two");
    EXPECT_EQ(round_tripped.reference_entries[1].entry_id, 7);
    ASSERT_EQ(round_tripped.closure_entry_ids.size(), 2u);
    EXPECT_EQ(round_tripped.closure_entry_ids[0], 41);
    EXPECT_EQ(round_tripped.closure_entry_ids[1], 42);
}

// ===========================================================================
// 引用闭包真实链的类型化值流 E2E：绑条目 GraphTask 节点的口面=被引条目文
// 档根 ports 自述（编译期注入 pass 现算），编译产物经 $graph_input 广播绑
// 定物化到节点 executionInput 口；执行期 GraphTask 形替身组件消费产物、
// 经 GraphRuntime 类型化 IO 跑子图（graph_inputs=节点输入 map 的类型化直
// 传覆盖层）、逐口回写类型化边界输出——image 引用全程同一实例 + 多值分
// 型输出口各达各点（跨子图值流语义在注入链上的重建：调用中介组件退役
// 后，调用节点=类型自身节点）。
// ===========================================================================
namespace
{
    using IDasImage = Das::ExportInterface::IDasImage;

    const std::string TYPED_IMAGE_SOURCE_GUID =
        "B0000000-0000-0000-0000-000000000001";
    const std::string TYPED_RELAY_CHILD_GUID =
        "B0000000-0000-0000-0000-000000000002";
    const std::string TYPED_IMAGE_SINK_GUID =
        "B0000000-0000-0000-0000-000000000003";
    const std::string MULTIVALUE_CHILD_GUID =
        "B0000000-0000-0000-0000-000000000004";
    const std::string INT_STRING_SINK_GUID =
        "B0000000-0000-0000-0000-000000000005";
    const std::string GRAPH_BEGIN_NODE_GUID =
        "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E";
    const std::string GRAPH_END_NODE_GUID =
        "E1B7C9D3-5F4A-4E2B-8D6C-7A9E1F3B5C2D";

    class E2ECountingFakeImage final
        : public Das::ExportInterface::DasImageImplBase<E2ECountingFakeImage>
    {
    public:
        DAS_IMPL GetSize(Das::ExportInterface::DasSize* p_out_size) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DAS_IMPL GetChannelCount(int32_t* p_out_channel_count) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DAS_IMPL Clip(
            const Das::ExportInterface::DasRect* p_rect,
            Das::ExportInterface::IDasImage**    p_out_image) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DAS_IMPL GetDataSize(uint64_t* p_out_size) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DAS_IMPL GetBinaryBuffer(
            Das::ExportInterface::IDasBinaryBuffer** pp_out_buffer) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DAS_IMPL GetPixelFormat(
            Das::ExportInterface::DasImagePixelFormat* p_out_format) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }
    };

    struct TypedChainState
    {
        DAS::DasPtr<IDasImage> source_image;
        // sink 收到的引用（非拥有记录——身份断言面）。
        IDasImage* received_image = nullptr;
        // 替身组件收到的编译产物串（注入物化断言面）。
        std::string received_execution_input;
    };

    struct InvokeMultiValueE2EState
    {
        bool        received_int_present = false;
        int64_t     received_int_value = 0;
        bool        received_string_present = false;
        std::string received_string_value;
    };

    // 父图 image 源替身：Do 输出 state 内持有的同一实例到 "img" 口。
    class E2EImageSourceComponent final : public IDasTaskComponent
    {
    public:
        std::atomic<uint32_t> ref_count_{0};

        explicit E2EImageSourceComponent(std::shared_ptr<TypedChainState> state)
            : state_(std::move(state))
        {
        }

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                ref_count_ = 1;
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            *p_out_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                TYPED_IMAGE_SOURCE_GUID);
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            return CreateIDasReadOnlyStringFromUtf8(
                "E2EImageSourceComponent",
                pp_out_name);
        }

        DasResult ApplySettingsChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json != nullptr)
            {
                *pp_out_result_json = nullptr;
            }
            return DAS_S_OK;
        }

        DasResult Do(
            IDasStopToken*,
            IDasReadOnlyPortMap*,
            IDasPortMap** pp_out_port_map) override
        {
            if (pp_out_port_map == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_port_map = nullptr;
            DAS::DasPtr<IDasPortMap> output_map;
            DasResult                hr = CreateIDasPortMap(output_map.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            const DasReadOnlyString image_key{"img"};
            hr = output_map->SetImage(
                image_key.Get(),
                state_->source_image.Get());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            *pp_out_port_map = output_map.Get();
            output_map.Get()->AddRef();
            return DAS_S_OK;
        }

    private:
        std::shared_ptr<TypedChainState> state_;
    };

    // 子图替身（模拟被引子图的类型化值流）：把 "p1" 口收到的 image 原引用
    // 写到 "p2" 口——类型化中继的最小同构。
    class E2ETypedRelayChildComponent final : public IDasTaskComponent
    {
    public:
        std::atomic<uint32_t> ref_count_{0};

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                ref_count_ = 1;
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            *p_out_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                TYPED_RELAY_CHILD_GUID);
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            return CreateIDasReadOnlyStringFromUtf8(
                "E2ETypedRelayChildComponent",
                pp_out_name);
        }

        DasResult ApplySettingsChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json != nullptr)
            {
                *pp_out_result_json = nullptr;
            }
            return DAS_S_OK;
        }

        DasResult Do(
            IDasStopToken*,
            IDasReadOnlyPortMap* p_input_port_map,
            IDasPortMap**        pp_out_port_map) override
        {
            if (pp_out_port_map == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_port_map = nullptr;
            DAS::DasPtr<IDasPortMap> output_map;
            DasResult                hr = CreateIDasPortMap(output_map.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            DAS::DasPtr<IDasImage>  image;
            const DasReadOnlyString in_key{"p1"};
            if (p_input_port_map != nullptr
                && DAS::IsOk(
                    p_input_port_map->GetImage(in_key.Get(), image.Put()))
                && image)
            {
                const DasReadOnlyString out_key{"p2"};
                hr = output_map->SetImage(out_key.Get(), image.Get());
                if (DAS::IsFailed(hr))
                {
                    return hr;
                }
            }
            *pp_out_port_map = output_map.Get();
            output_map.Get()->AddRef();
            return DAS_S_OK;
        }
    };

    // 父图 image 消费替身：Do 记录 "in" 口的引用身份到共享 state。
    class E2EImageSinkComponent final : public IDasTaskComponent
    {
    public:
        std::atomic<uint32_t> ref_count_{0};

        explicit E2EImageSinkComponent(std::shared_ptr<TypedChainState> state)
            : state_(std::move(state))
        {
        }

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                ref_count_ = 1;
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            *p_out_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                TYPED_IMAGE_SINK_GUID);
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            return CreateIDasReadOnlyStringFromUtf8(
                "E2EImageSinkComponent",
                pp_out_name);
        }

        DasResult ApplySettingsChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json != nullptr)
            {
                *pp_out_result_json = nullptr;
            }
            return DAS_S_OK;
        }

        DasResult Do(
            IDasStopToken*,
            IDasReadOnlyPortMap* p_input_port_map,
            IDasPortMap**        pp_out_port_map) override
        {
            if (pp_out_port_map == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (p_input_port_map != nullptr)
            {
                DAS::DasPtr<IDasImage>  image;
                const DasReadOnlyString in_key{"in"};
                if (DAS::IsOk(
                        p_input_port_map->GetImage(in_key.Get(), image.Put())))
                {
                    state_->received_image = image.Get();
                }
            }
            *pp_out_port_map = nullptr;
            return DAS_S_OK;
        }

    private:
        std::shared_ptr<TypedChainState> state_;
    };

    // 子图替身（多值分型出口）：Do 直接产出两个分型边界口条目（e_1 int /
    // e_2 string）——值出口集合=编译清单。
    class E2EMultiValueChildComponent final : public IDasTaskComponent
    {
    public:
        std::atomic<uint32_t> ref_count_{0};

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                ref_count_ = 1;
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            *p_out_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                MULTIVALUE_CHILD_GUID);
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            return CreateIDasReadOnlyStringFromUtf8(
                "E2EMultiValueChildComponent",
                pp_out_name);
        }

        DasResult ApplySettingsChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json != nullptr)
            {
                *pp_out_result_json = nullptr;
            }
            return DAS_S_OK;
        }

        DasResult Do(
            IDasStopToken*,
            IDasReadOnlyPortMap*,
            IDasPortMap** pp_out_port_map) override
        {
            if (pp_out_port_map == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_port_map = nullptr;
            DAS::DasPtr<IDasPortMap> output_map;
            DasResult                hr = CreateIDasPortMap(output_map.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            const DasReadOnlyString int_key{"e_1"};
            hr = output_map->SetInt(int_key.Get(), int64_t{4242});
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            const DasReadOnlyString string_key{"e_2"};
            const DasReadOnlyString string_value{"multi-out"};
            hr = output_map->SetString(string_key.Get(), string_value.Get());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            *pp_out_port_map = output_map.Get();
            output_map.Get()->AddRef();
            return DAS_S_OK;
        }
    };

    // 父图消费替身：Do 记录 in_int / in_string 两口收值。
    class E2EIntStringSinkComponent final : public IDasTaskComponent
    {
    public:
        std::atomic<uint32_t> ref_count_{0};

        explicit E2EIntStringSinkComponent(
            std::shared_ptr<InvokeMultiValueE2EState> state)
            : state_(std::move(state))
        {
        }

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                ref_count_ = 1;
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            *p_out_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                INT_STRING_SINK_GUID);
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            return CreateIDasReadOnlyStringFromUtf8(
                "E2EIntStringSinkComponent",
                pp_out_name);
        }

        DasResult ApplySettingsChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json != nullptr)
            {
                *pp_out_result_json = nullptr;
            }
            return DAS_S_OK;
        }

        DasResult Do(
            IDasStopToken*,
            IDasReadOnlyPortMap* p_input_port_map,
            IDasPortMap**        pp_out_port_map) override
        {
            if (pp_out_port_map == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (p_input_port_map != nullptr)
            {
                const DasReadOnlyString int_key{"in_int"};
                int64_t                 int_value = 0;
                if (DAS::IsOk(
                        p_input_port_map->GetInt(int_key.Get(), &int_value)))
                {
                    state_->received_int_present = true;
                    state_->received_int_value = int_value;
                }
                const DasReadOnlyString         string_key{"in_string"};
                Das::DasPtr<IDasReadOnlyString> string_value;
                if (DAS::IsOk(p_input_port_map->GetString(
                        string_key.Get(),
                        string_value.Put()))
                    && string_value)
                {
                    const char* raw = nullptr;
                    if (DAS::IsOk(string_value->GetUtf8(&raw)) && raw)
                    {
                        state_->received_string_present = true;
                        state_->received_string_value = raw;
                    }
                }
            }
            *pp_out_port_map = nullptr;
            return DAS_S_OK;
        }

    private:
        std::shared_ptr<InvokeMultiValueE2EState> state_;
    };

    // GraphTask 形替身（生产 DasGraphTaskImpl::Do 的最小同构）：读
    // executionInput 口的编译产物串（{"fingerprint","plan"} shell），经
    // GraphRuntime 类型化通道执行子图——graph_inputs=自身输入 map（类型化
    // 直传覆盖层，键=边界口 id），产物=类型化边界输出逐口直写自身输出。
    class E2EGraphTaskStandInComponent final : public IDasTaskComponent
    {
    public:
        std::atomic<uint32_t> ref_count_{0};

        E2EGraphTaskStandInComponent(
            Das::PluginInterface::IDasTaskComponentCreator* creator,
            std::shared_ptr<TypedChainState>                state)
            : creator_(creator), state_(std::move(state))
        {
        }

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            const auto count = --ref_count_;
            if (count == 0)
            {
                ref_count_ = 1;
                delete this;
            }
            return count;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (pp_out == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            *p_out_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                std::string{REF_CHAIN_GRAPH_TASK_GUID});
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            return CreateIDasReadOnlyStringFromUtf8(
                "E2EGraphTaskStandInComponent",
                pp_out_name);
        }

        DasResult ApplySettingsChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json != nullptr)
            {
                *pp_out_result_json = nullptr;
            }
            return DAS_S_OK;
        }

        DasResult Do(
            IDasStopToken*       stop_token,
            IDasReadOnlyPortMap* p_input_port_map,
            IDasPortMap**        pp_out_port_map) override
        {
            if (pp_out_port_map == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_port_map = nullptr;

            std::string artifact_text;
            if (p_input_port_map != nullptr)
            {
                const DasReadOnlyString         key{"executionInput"};
                Das::DasPtr<IDasReadOnlyString> artifact;
                if (DAS::IsOk(
                        p_input_port_map->GetString(key.Get(), artifact.Put()))
                    && artifact.Get() != nullptr)
                {
                    const char* utf8 = nullptr;
                    if (DAS::IsOk(artifact->GetUtf8(&utf8)) && utf8 != nullptr)
                    {
                        artifact_text = utf8;
                    }
                }
            }
            if (artifact_text.empty())
            {
                return DAS_E_FAIL;
            }
            if (state_)
            {
                state_->received_execution_input = artifact_text;
            }

            CompiledGraphPlanDto child_plan;
            {
                auto shell = yyjson::read(artifact_text);
                auto shell_obj = shell.as_object();
                if (!shell_obj.has_value())
                {
                    return DAS_E_INVALID_JSON;
                }
                child_plan = yyjson::cast<CompiledGraphPlanDto>(
                    Das::Utils::CloneYyjsonValue(
                        (*shell_obj)[std::string_view("plan")]));
            }

            // 与生产 ExecuteWithTypedIO 同款：current=plan.source_fingerprint
            // （shell 指纹是产物完整性签，不是运行期比对值）。
            GraphRuntime child_runtime;
            child_runtime.SetGraphInputs(p_input_port_map);
            return child_runtime.RunWithHost(
                child_plan,
                child_plan.source_fingerprint,
                stop_token,
                creator_,
                pp_out_port_map);
        }

    private:
        Das::PluginInterface::IDasTaskComponentCreator* creator_;
        std::shared_ptr<TypedChainState>                state_;
    };

    // 链路 fake creator：绑条目节点的会话=固定响应 fake 会话（文档根
    // ports 自述 + 固定编译产物）；图内其余组件按 guid 分发替身，begin/
    // end 等未特判 guid 落到记录组件（零输出不阻断执行序）。
    class TypedChainCreatorHost final
        : public Das::PluginInterface::DasTaskComponentCreatorImplBase<
              TypedChainCreatorHost>
    {
    public:
        TypedChainCreatorHost(
            std::shared_ptr<ReferenceChainState> recorder_state,
            std::shared_ptr<TypedChainState>     typed_state,
            std::string                          entry_document_json,
            std::string                          entry_compile_json)
            : recorder_state_(std::move(recorder_state)),
              typed_state_(std::move(typed_state)),
              entry_document_json_(std::move(entry_document_json)),
              entry_compile_json_(std::move(entry_compile_json))
        {
        }

        DasResult CreateTaskComponent(
            const DasGuid&                            component_guid,
            Das::PluginInterface::IDasTaskComponent** pp_out_component) override
        {
            if (pp_out_component == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_component = nullptr;
            const std::string guid_text =
                Das::Core::ForeignInterfaceHost::DasGuidToStdString(
                    component_guid);

            IDasTaskComponent* component = nullptr;
            if (guid_text == REF_CHAIN_GRAPH_TASK_GUID)
            {
                component =
                    new E2EGraphTaskStandInComponent(this, typed_state_);
            }
            else if (guid_text == TYPED_IMAGE_SOURCE_GUID)
            {
                component = new E2EImageSourceComponent(typed_state_);
            }
            else if (guid_text == TYPED_IMAGE_SINK_GUID)
            {
                component = new E2EImageSinkComponent(typed_state_);
            }
            else if (guid_text == TYPED_RELAY_CHILD_GUID)
            {
                component = new E2ETypedRelayChildComponent();
            }
            else
            {
                component = new ExecutionInputRecorderComponent(
                    recorder_state_,
                    component_guid);
            }
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }

        DasResult CreateAuthoringSession(
            const DasGuid&                                   task_guid,
            Das::ExportInterface::IDasJson*                  p_context_json,
            Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session)
            override
        {
            std::ignore = task_guid;
            std::ignore = p_context_json;
            if (pp_out_session == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            auto* session = new ReferenceChainFakeSession(
                entry_document_json_,
                entry_compile_json_);
            session->AddRef();
            *pp_out_session = session;
            return DAS_S_OK;
        }

    private:
        std::shared_ptr<ReferenceChainState> recorder_state_;
        std::shared_ptr<TypedChainState>     typed_state_;
        std::string                          entry_document_json_;
        std::string                          entry_compile_json_;
    };

    struct ChainPlanHandle
    {
        std::string          shell_text;
        std::string          fingerprint;
        CompiledGraphPlanDto plan;
    };

    // 会话编译 → shell 解析（fingerprint + plan DTO）。
    void CompileChainPlan(
        Das::PluginInterface::IDasTaskComponentCreator* creator,
        const std::string&                              context_text,
        ChainPlanHandle&                                out)
    {
        // 前置校验：context 必须是合法 JSON——SeedFromContext 对非法输入
        // 静默降级（空会话 + auto-seed begin/end），夹具拼接错误在此显性
        // 化而非表现为「广播绑定缺失」。
        ASSERT_NO_THROW(yyjson::read(context_text)) << context_text;

        GraphAuthoringSessionState* session_state = nullptr;
        {
            auto ctx = RefChainMakeText(context_text);
            ASSERT_EQ(
                CreateGraphAuthoringSession(creator, ctx.Get(), &session_state),
                DAS_S_OK);
        }
        ASSERT_NE(session_state, nullptr);

        Das::DasPtr<Das::ExportInterface::IDasJson> compiled;
        ASSERT_EQ(
            GraphAuthoringSessionCompile(
                session_state,
                nullptr,
                compiled.Put()),
            DAS_S_OK);
        DestroyGraphAuthoringSession(session_state);

        const auto envelope_text = RefChainJsonText(compiled.Get());
        ASSERT_FALSE(envelope_text.empty()) << envelope_text;

        auto envelope = yyjson::read(envelope_text);
        auto envelope_obj = envelope.as_object();
        ASSERT_TRUE(envelope_obj.has_value());
        auto compile_obj =
            (*envelope_obj)[std::string_view("compile")].as_object();
        ASSERT_TRUE(compile_obj.has_value());
        const auto& execution_input =
            (*compile_obj)[std::string_view("executionInput")];
        if (const auto text = execution_input.as_string())
        {
            out.shell_text = std::string(*text);
        }
        else
        {
            out.shell_text = Das::Utils::SerializeYyjsonValue(
                                 Das::Utils::CloneYyjsonValue(execution_input))
                                 .value_or("");
        }
        ASSERT_FALSE(out.shell_text.empty());

        auto shell = yyjson::read(out.shell_text);
        auto shell_obj = shell.as_object();
        ASSERT_TRUE(shell_obj.has_value());
        auto fingerprint =
            (*shell_obj)[std::string_view("fingerprint")].as_string();
        ASSERT_TRUE(fingerprint.has_value());
        out.fingerprint = std::string(*fingerprint);
        out.plan =
            yyjson::cast<CompiledGraphPlanDto>(Das::Utils::CloneYyjsonValue(
                (*shell_obj)[std::string_view("plan")]));
    }

    // 断言 plan 携带产物广播绑定（$graph_input → node.executionInput，
    // default_value=编译产物原文）。
    void ExpectArtifactBroadcast(
        const CompiledGraphPlanDto& plan,
        const std::string&          node_id,
        const std::string&          expected_artifact)
    {
        bool found_broadcast = false;
        for (const auto& binding : plan.binding_plan.bindings)
        {
            if (binding.source_node_id != "$graph_input"
                || binding.target_node_id != node_id
                || binding.target_port_id != "executionInput")
            {
                continue;
            }
            const auto artifact = binding.default_value.as_string();
            ASSERT_TRUE(artifact.has_value());
            // 产物原文逐字节比对（string 标量承载编译产物 shell）。
            EXPECT_EQ(*artifact, expected_artifact);
            found_broadcast = true;
        }
        EXPECT_TRUE(found_broadcast)
            << "compiled plan must carry the reference artifact broadcast "
               "binding";
    }

    // 父图任务文档（graph 契约形状——nodes/edges 原文随 properties 播种）。
    std::string MakeChainNode(
        const std::string& node_id,
        const std::string& component_guid,
        const std::string& settings_json)
    {
        return "{\"nodeId\":\"" + node_id
               + "\",\"target\":{\"targetKind\":\"componentRef\","
                 "\"componentRef\":{\"kind\":\"componentRef\",\"componentGuid\":\""
               + component_guid
               + "\",\"pluginGuid\":\"C8A4B7D0-1E2F-3A5B-6C9D-7E8F0A1B2C3D\"}},"
                 "\"settings\":"
               + settings_json + "}";
    }

    std::string MakeChainEdge(
        const std::string& edge_id,
        const std::string& source_node,
        const std::string& source_port,
        const std::string& target_node,
        const std::string& target_port)
    {
        return "{\"edgeId\":\"" + edge_id + "\",\"sourceNodeId\":\""
               + source_node + "\",\"sourcePortId\":\"" + source_port
               + "\",\"targetNodeId\":\"" + target_node
               + "\",\"targetPortId\":\"" + target_port
               + "\",\"edgeType\":\"data\"}";
    }

    std::string MakeChainContext(
        const std::string& parent_document_json,
        const std::string& catalog_json)
    {
        return std::string(R"({"properties":)") + parent_document_json
               + R"(,"actionCatalog":)" + catalog_json + "}";
    }

    // GraphTask 组件的 catalog 条目（repositoryRef 绑定判据 + executionInput
    // 声明口）。
    std::string MakeGraphTaskCatalogEntry()
    {
        return std::string(R"({"actionId":")")
               + std::string{REF_CHAIN_GRAPH_TASK_GUID}
               + R"(","ports":[{"portId":"executionInput","portType":"object","direction":"input"}],)"
                 R"("config":[{"name":"repositoryRef","type":"taskRepositoryRef","taskGuid":")"
               + std::string{REF_CHAIN_GRAPH_TASK_GUID} + R"("}]})";
    }

    std::string MakePortCatalogEntry(
        const std::string& guid,
        const std::string& ports_json)
    {
        return R"({"actionId":")" + guid + R"(","ports":[)" + ports_json
               + R"(]})";
    }

    // 被引条目 Compile 响应：{compile:{executionInput:<shell 串>},
    // diagnostics:[]}。
    std::string MakeEntryCompileResponse(const std::string& child_shell_text)
    {
        yyjson::value compile_response(Das::Utils::MakeYyjsonObject());
        auto          response_obj = compile_response.as_object();
        yyjson::value compile_section(Das::Utils::MakeYyjsonObject());
        (*compile_section.as_object())[std::string_view("executionInput")] =
            yyjson::value(child_shell_text);
        (*response_obj)[std::string_view("compile")] =
            std::move(compile_section);
        (*response_obj)[std::string_view("diagnostics")] =
            Das::Utils::MakeYyjsonArray();
        return Das::Utils::SerializeYyjsonValue(
                   Das::Utils::CloneYyjsonValue(compile_response))
            .value_or("");
    }

    // 被引条目文档（根 ports 自述——注入 pass 的节点口面唯一源）。
    std::string MakeEntryDocument(const std::string& root_ports_json)
    {
        return std::string(R"({"document":{"viewKind":"graph","ports":[)")
               + root_ports_json + R"(]},"diagnostics":[]})";
    }
} // namespace

TEST_F(E2EGraphFlowTest, ReferenceClosureTypedImageFlowKeepsIdentity)
{
    const auto typed_state = std::make_shared<TypedChainState>();
    typed_state->source_image = E2ECountingFakeImage::Make();

    // 子图产物：单节点中继子图——p1 口的 $graph_input 广播声明（类型化直
    // 传覆盖层命中父图喂入的 image 引用），p2 出口（boundaryPorts.outputs
    // 声明）。
    std::string child_shell_text;
    {
        CompiledGraphPlanDto child_plan;
        child_plan.source_fingerprint = "fp_typed_child";
        CompiledNodeSnapshotDto child_node;
        child_node.node_id = "n_relay";
        child_node.component_guid = TYPED_RELAY_CHILD_GUID;
        child_plan.node_snapshots.push_back(std::move(child_node));
        child_plan.execution_order = {"n_relay"};

        PortBindingDto input_broadcast;
        input_broadcast.source_node_id = "$graph_input";
        input_broadcast.source_port_id = "p1";
        input_broadcast.target_node_id = "n_relay";
        input_broadcast.target_port_id = "p1";
        input_broadcast.expected_type = "image";
        child_plan.binding_plan.bindings.push_back(std::move(input_broadcast));

        BoundaryPortDto child_boundary_output;
        child_boundary_output.port_id = "p2";
        child_boundary_output.port_type = "image";
        child_boundary_output.node_ids = {"n_relay"};
        child_plan.boundary_ports.outputs.push_back(
            std::move(child_boundary_output));

        child_shell_text =
            Das::Utils::SerializeYyjsonValue(
                BuildCompiledPlanShell(yyjson::object(child_plan)))
                .value_or("");
        ASSERT_FALSE(child_shell_text.empty());
    }

    auto creator = TypedChainCreatorHost::Make(
        std::make_shared<ReferenceChainState>(),
        typed_state,
        MakeEntryDocument(
            R"({"id":"p1","type":"image","direction":"input"},)"
            R"({"id":"p2","type":"image","direction":"output"})"),
        MakeEntryCompileResponse(child_shell_text));

    // 父图文档：begin + image 源 → 绑条目 GraphTask 节点（entry 42）→
    // 消费节点 + end。
    const std::string parent_document =
        std::string(R"({"documentId":"typed-chain","version":1,"nodes":[)")
        + MakeChainNode("n_begin", GRAPH_BEGIN_NODE_GUID, "{}") + ","
        + MakeChainNode("n_source", TYPED_IMAGE_SOURCE_GUID, "{}") + ","
        + MakeChainNode(
            "n_bound",
            std::string{REF_CHAIN_GRAPH_TASK_GUID},
            R"({"repositoryRef":42})")
        + "," + MakeChainNode("n_sink", TYPED_IMAGE_SINK_GUID, "{}") + ","
        + MakeChainNode("n_end", GRAPH_END_NODE_GUID, "{}") + R"(],"edges":[)"
        + MakeChainEdge("e1", "n_source", "img", "n_bound", "p1") + ","
        + MakeChainEdge("e2", "n_bound", "p2", "n_sink", "in")
        + R"(],"tags":[]})";
    const std::string catalog =
        std::string(R"([)") + MakeGraphTaskCatalogEntry() + ","
        + MakePortCatalogEntry(
            TYPED_IMAGE_SOURCE_GUID,
            R"({"portId":"img","portType":"image","direction":"output"})")
        + ","
        + MakePortCatalogEntry(
            TYPED_IMAGE_SINK_GUID,
            R"({"portId":"in","portType":"image","direction":"input"})")
        + "]";

    ChainPlanHandle parent;
    CompileChainPlan(
        creator.Get(),
        MakeChainContext(parent_document, catalog),
        parent);
    ASSERT_NO_FATAL_FAILURE(
        ExpectArtifactBroadcast(parent.plan, "n_bound", child_shell_text));

    // 执行：image 引用经「父图数据边 → 类型化直传覆盖层 → 子图中继 →
    // 类型化边界输出 → 父图数据边」全程同一实例。
    GraphRuntime runtime;
    const auto   hr = runtime.RunWithHost(
        parent.plan,
        parent.fingerprint,
        token_.Get(),
        creator.Get());
    ASSERT_EQ(hr, DAS_S_OK) << runtime.GetLastErrorMessage();

    EXPECT_EQ(typed_state->received_execution_input, child_shell_text)
        << "the bound node must receive the injected child artifact";
    ASSERT_NE(typed_state->received_image, nullptr)
        << "the sink never received the projected image";
    EXPECT_EQ(typed_state->received_image, typed_state->source_image.Get());
}

TEST_F(E2EGraphFlowTest, ReferenceClosureProjectsMultiValueTypedOutputs)
{
    const auto multivalue_state = std::make_shared<InvokeMultiValueE2EState>();

    // 子图产物：单节点子图，boundaryPorts.outputs 声明 e_1（int）/ e_2
    // （string）双出口（类型化导出与逐口投影的消费清单）。
    std::string child_shell_text;
    {
        CompiledGraphPlanDto child_plan;
        child_plan.source_fingerprint = "fp_multivalue_child";
        CompiledNodeSnapshotDto child_node;
        child_node.node_id = "child_node";
        child_node.component_guid = MULTIVALUE_CHILD_GUID;
        child_plan.node_snapshots.push_back(std::move(child_node));
        child_plan.execution_order = {"child_node"};
        BoundaryPortDto int_boundary_output;
        int_boundary_output.port_id = "e_1";
        int_boundary_output.port_type = "int";
        int_boundary_output.node_ids = {"child_node"};
        child_plan.boundary_ports.outputs.push_back(
            std::move(int_boundary_output));
        BoundaryPortDto string_boundary_output;
        string_boundary_output.port_id = "e_2";
        string_boundary_output.port_type = "string";
        string_boundary_output.node_ids = {"child_node"};
        child_plan.boundary_ports.outputs.push_back(
            std::move(string_boundary_output));

        child_shell_text =
            Das::Utils::SerializeYyjsonValue(
                BuildCompiledPlanShell(yyjson::object(child_plan)))
                .value_or("");
        ASSERT_FALSE(child_shell_text.empty());
    }

    // 多值链 host：复用 TypedChainCreatorHost 的分发（多值子/汇组件落
    // recorder 分支不行——需要专用分发）。直接构造派生 host。
    const auto typed_state = std::make_shared<TypedChainState>();
    const auto recorder_state = std::make_shared<ReferenceChainState>();

    class MultiValueChainCreatorHost final
        : public Das::PluginInterface::DasTaskComponentCreatorImplBase<
              MultiValueChainCreatorHost>
    {
    public:
        MultiValueChainCreatorHost(
            std::shared_ptr<ReferenceChainState>      recorder_state,
            std::shared_ptr<TypedChainState>          typed_state,
            std::shared_ptr<InvokeMultiValueE2EState> multivalue_state,
            std::string                               entry_document_json,
            std::string                               entry_compile_json)
            : recorder_state_(std::move(recorder_state)),
              typed_state_(std::move(typed_state)),
              multivalue_state_(std::move(multivalue_state)),
              entry_document_json_(std::move(entry_document_json)),
              entry_compile_json_(std::move(entry_compile_json))
        {
        }

        DasResult CreateTaskComponent(
            const DasGuid&      component_guid,
            IDasTaskComponent** pp_out_component) override
        {
            if (pp_out_component == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_component = nullptr;
            const std::string guid_text =
                Das::Core::ForeignInterfaceHost::DasGuidToStdString(
                    component_guid);

            IDasTaskComponent* component = nullptr;
            if (guid_text == REF_CHAIN_GRAPH_TASK_GUID)
            {
                component =
                    new E2EGraphTaskStandInComponent(this, typed_state_);
            }
            else if (guid_text == MULTIVALUE_CHILD_GUID)
            {
                component = new E2EMultiValueChildComponent();
            }
            else if (guid_text == INT_STRING_SINK_GUID)
            {
                component = new E2EIntStringSinkComponent(multivalue_state_);
            }
            else
            {
                component = new ExecutionInputRecorderComponent(
                    recorder_state_,
                    component_guid);
            }
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }

        DasResult CreateAuthoringSession(
            const DasGuid&                                   task_guid,
            Das::ExportInterface::IDasJson*                  p_context_json,
            Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session)
            override
        {
            std::ignore = task_guid;
            std::ignore = p_context_json;
            if (pp_out_session == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            auto* session = new ReferenceChainFakeSession(
                entry_document_json_,
                entry_compile_json_);
            session->AddRef();
            *pp_out_session = session;
            return DAS_S_OK;
        }

    private:
        std::shared_ptr<ReferenceChainState>      recorder_state_;
        std::shared_ptr<TypedChainState>          typed_state_;
        std::shared_ptr<InvokeMultiValueE2EState> multivalue_state_;
        std::string                               entry_document_json_;
        std::string                               entry_compile_json_;
    };

    auto creator = MultiValueChainCreatorHost::Make(
        recorder_state,
        typed_state,
        multivalue_state,
        MakeEntryDocument(
            R"({"id":"e_1","type":"int","direction":"output"},)"
            R"({"id":"e_2","type":"string","direction":"output"})"),
        MakeEntryCompileResponse(child_shell_text));

    // 父图文档：begin + 绑条目 GraphTask 节点（entry 42）→ 两个分型消费
    // 节点 + end。
    const std::string parent_document =
        std::string(R"({"documentId":"multivalue-chain","version":1,"nodes":[)")
        + MakeChainNode("n_begin", GRAPH_BEGIN_NODE_GUID, "{}") + ","
        + MakeChainNode(
            "n_bound",
            std::string{REF_CHAIN_GRAPH_TASK_GUID},
            R"({"repositoryRef":42})")
        + "," + MakeChainNode("n_sink_int", INT_STRING_SINK_GUID, "{}") + ","
        + MakeChainNode("n_sink_str", INT_STRING_SINK_GUID, "{}") + ","
        + MakeChainNode("n_end", GRAPH_END_NODE_GUID, "{}") + R"(],"edges":[)"
        + MakeChainEdge("e1", "n_bound", "e_1", "n_sink_int", "in_int") + ","
        + MakeChainEdge("e2", "n_bound", "e_2", "n_sink_str", "in_string")
        + R"(],"tags":[]})";
    const std::string catalog =
        std::string(R"([)") + MakeGraphTaskCatalogEntry() + ","
        + MakePortCatalogEntry(
            INT_STRING_SINK_GUID,
            R"({"portId":"in_int","portType":"int","direction":"input"},)"
            R"({"portId":"in_string","portType":"string","direction":"input"})")
        + "]";

    ChainPlanHandle parent;
    CompileChainPlan(
        creator.Get(),
        MakeChainContext(parent_document, catalog),
        parent);
    ASSERT_NO_FATAL_FAILURE(
        ExpectArtifactBroadcast(parent.plan, "n_bound", child_shell_text));

    GraphRuntime runtime;
    const auto   hr = runtime.RunWithHost(
        parent.plan,
        parent.fingerprint,
        token_.Get(),
        creator.Get());
    ASSERT_EQ(hr, DAS_S_OK) << runtime.GetLastErrorMessage();

    EXPECT_EQ(typed_state->received_execution_input, child_shell_text);
    // 两下游各收各值（类型化边界导出 + 分型直连）。
    ASSERT_TRUE(multivalue_state->received_int_present)
        << "the int downstream node never received e_1";
    EXPECT_EQ(multivalue_state->received_int_value, 4242);
    ASSERT_TRUE(multivalue_state->received_string_present)
        << "the string downstream node never received e_2";
    EXPECT_EQ(multivalue_state->received_string_value, "multi-out");
}
