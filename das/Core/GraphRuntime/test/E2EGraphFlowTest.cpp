#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>
#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/CompiledPlanFingerprint.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/DoAdapter.h>
#include <das/Core/GraphRuntime/GraphCompiler.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/GraphRuntime/GraphRuntime.h>
#include <das/Core/GraphRuntime/PortFrame.h>
#include <das/Core/TaskScheduler/RepositoryInvokeDtos.h>
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
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentHost.Implements.hpp>
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
    : public Das::PluginInterface::DasTaskComponentHostImplBase<
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

        auto* component = new Das::DasFlowControlTaskComponent(
            LAUNCH_KIND_TEXT,
            DAS::DasPtr<Das::PluginInterface::IDasTaskComponentHost>{});
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
    : public Das::PluginInterface::DasTaskComponentHostImplBase<
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
            new Das::DasFlowControlTaskComponent(
                kind,
                Das::DasPtr<Das::PluginInterface::IDasTaskComponentHost>{}));
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
            new Das::DasFlowControlTaskComponent(
                kind,
                Das::DasPtr<Das::PluginInterface::IDasTaskComponentHost>{})};
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
        : public Das::PluginInterface::DasTaskComponentHostImplBase<
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
// 跨子图类型化直传 E2E（D-18/D-20/D-21）：父图数据边的 image 引用经真实
// invokeRepository 直塞子图、子图类型化出口直读投影回父组件输出、父图下
// 游数据边消费——全程同一实例（对象引用首次跨子图，JSON 桥的永不可达
// 半段）。
// ===========================================================================
namespace
{
    using Das::DasPtr;
    using IDasImage = Das::ExportInterface::IDasImage;
    using IDasTaskComponent = Das::PluginInterface::IDasTaskComponent;

    const std::string TYPED_IMAGE_SOURCE_GUID =
        "B0000000-0000-0000-0000-000000000001";
    const std::string TYPED_ECHO_CHILD_GUID =
        "B0000000-0000-0000-0000-000000000002";
    const std::string TYPED_IMAGE_SINK_GUID =
        "B0000000-0000-0000-0000-000000000003";
    const std::string REPOSITORY_INVOKE_NODE_GUID =
        "68F10007-0000-4000-8000-000000000007";
    constexpr std::string_view REPOSITORY_INVOKE_KIND_TEXT =
        "das.flow.invokeRepository";

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

    struct InvokeTypedE2EState
    {
        DAS::DasPtr<IDasImage> source_image;
        // sink 收到的引用（非拥有记录——身份断言面）。
        IDasImage* received_image = nullptr;
    };

    // 父图 image 源替身：Do 输出 state 内持有的同一实例到 "img" 口。
    class E2EImageSourceComponent final : public IDasTaskComponent
    {
    public:
        std::atomic<uint32_t> ref_count_{0};

        explicit E2EImageSourceComponent(
            std::shared_ptr<InvokeTypedE2EState> state)
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
        std::shared_ptr<InvokeTypedE2EState> state_;
    };

    // 子图替身（模拟 GraphTask 形态的 typed 出口面）：把收到的非
    // executionInput 的 image 条目原键回写输出——DasGraphTaskImpl 类型化
    // 直写的最小同构。
    class E2ETypedEchoChildComponent final : public IDasTaskComponent
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
                TYPED_ECHO_CHILD_GUID);
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            return CreateIDasReadOnlyStringFromUtf8(
                "E2ETypedEchoChildComponent",
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
            if (p_input_port_map != nullptr)
            {
                DAS::DasPtr<Das::ExportInterface::IDasStringVector> keys;
                if (DAS::IsOk(p_input_port_map->GetKeys(keys.Put())) && keys)
                {
                    uint64_t key_count = 0;
                    if (DAS::IsOk(keys->Size(&key_count)))
                    {
                        for (uint64_t key_index = 0; key_index < key_count;
                             ++key_index)
                        {
                            IDasReadOnlyString* key_string = nullptr;
                            if (DAS::IsOk(keys->At(key_index, &key_string))
                                && key_string != nullptr)
                            {
                                const char* key_utf8 = nullptr;
                                key_string->GetUtf8(&key_utf8);
                                const std::string port_name(
                                    key_utf8 != nullptr ? key_utf8 : "");
                                key_string->Release();
                                if (port_name.empty()
                                    || port_name == "executionInput")
                                {
                                    continue;
                                }
                                Das::Utils::CopyPortMapEntry(
                                    p_input_port_map,
                                    port_name,
                                    output_map.Get(),
                                    port_name);
                            }
                        }
                    }
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

        explicit E2EImageSinkComponent(
            std::shared_ptr<InvokeTypedE2EState> state)
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
        std::shared_ptr<InvokeTypedE2EState> state_;
    };

    class InvokeTypedHost final
        : public Das::PluginInterface::DasTaskComponentHostImplBase<
              InvokeTypedHost>
    {
    public:
        explicit InvokeTypedHost(std::shared_ptr<InvokeTypedE2EState> state)
            : state_(std::move(state))
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

            if (guid_text == TYPED_IMAGE_SOURCE_GUID)
            {
                auto* component = new E2EImageSourceComponent(state_);
                component->AddRef();
                *pp_out_component = component;
                return DAS_S_OK;
            }
            if (guid_text == TYPED_ECHO_CHILD_GUID)
            {
                auto* component = new E2ETypedEchoChildComponent();
                component->AddRef();
                *pp_out_component = component;
                return DAS_S_OK;
            }
            if (guid_text == TYPED_IMAGE_SINK_GUID)
            {
                auto* component = new E2EImageSinkComponent(state_);
                component->AddRef();
                *pp_out_component = component;
                return DAS_S_OK;
            }
            if (guid_text == REPOSITORY_INVOKE_NODE_GUID)
            {
                auto* component = new Das::DasFlowControlTaskComponent(
                    std::string{REPOSITORY_INVOKE_KIND_TEXT},
                    DasPtr<Das::PluginInterface::IDasTaskComponentHost>(this));
                component->AddRef();
                *pp_out_component = component;
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

    private:
        std::shared_ptr<InvokeTypedE2EState> state_;
    };
} // namespace

TEST_F(E2EGraphFlowTest, RepositoryInvokeProjectsImageReferenceOntoParentEdge)
{
    auto state = std::make_shared<InvokeTypedE2EState>();
    state->source_image = E2ECountingFakeImage::Make();

    // 子图快照：单节点 echo 子图，boundaryPorts.outputs 声明 p1 出口。
    std::string snapshot_json;
    {
        CompiledGraphPlanDto child_plan;
        child_plan.source_fingerprint = "fp_typed_child";
        CompiledNodeSnapshotDto child_snapshot;
        child_snapshot.node_id = "n_1";
        child_snapshot.component_guid = TYPED_ECHO_CHILD_GUID;
        child_plan.node_snapshots.push_back(std::move(child_snapshot));
        child_plan.execution_order = {"n_1"};
        BoundaryPortDto child_boundary_output;
        child_boundary_output.port_id = "p1";
        child_boundary_output.port_type = "image";
        child_boundary_output.node_ids = {"n_1"};
        child_plan.boundary_ports.outputs.push_back(
            std::move(child_boundary_output));

        namespace RepositoryInvokeDto =
            Das::Core::TaskScheduler::RepositoryInvoke::Dto;
        RepositoryInvokeDto::ChildExecutionSnapshotDto snapshot;
        snapshot.version = 1;
        snapshot.source_entry_id = 42;
        snapshot.component_guid = TYPED_ECHO_CHILD_GUID;
        snapshot.execution_input =
            BuildCompiledPlanShell(yyjson::object(child_plan));
        // compiledSnapshot 口契约=完整快照 DTO 的序列化（非裸 shell）。
        const auto serialized = Das::Utils::SerializeYyjsonValue(
            Das::Utils::CloneYyjsonValue(yyjson::object(snapshot)));
        ASSERT_TRUE(serialized.has_value());
        snapshot_json = *serialized;
    }

    // 父图：image 源 → invokeRepository(p1 边界口) → 消费节点。
    const std::string node_source{"n_source"};
    const std::string node_invoke{"n_invoke"};
    const std::string node_sink{"n_sink"};

    CompiledGraphPlanDto plan;
    plan.source_fingerprint = "fp_typed_parent";
    plan.node_snapshots.push_back(
        MakeSnapshot(node_source, TYPED_IMAGE_SOURCE_GUID));
    plan.node_snapshots.push_back(
        MakeSnapshot(node_invoke, REPOSITORY_INVOKE_NODE_GUID));
    plan.node_snapshots.push_back(
        MakeSnapshot(node_sink, TYPED_IMAGE_SINK_GUID));
    plan.execution_order = {node_source, node_invoke, node_sink};

    // 快照经哨兵广播物化到 compiledSnapshot 输入口（string 标量形态）。
    PortBindingDto snapshot_broadcast;
    snapshot_broadcast.source_node_id = "$graph_input";
    snapshot_broadcast.source_port_id = "snapshot";
    snapshot_broadcast.target_node_id = node_invoke;
    snapshot_broadcast.target_port_id = "compiledSnapshot";
    snapshot_broadcast.expected_type = "object";
    snapshot_broadcast.default_value = yyjson::value(snapshot_json);
    plan.binding_plan.bindings.push_back(std::move(snapshot_broadcast));

    plan.binding_plan.bindings.push_back(
        MakeBinding(node_source, "img", node_invoke, "p1", "image"));
    plan.binding_plan.bindings.push_back(
        MakeBinding(node_invoke, "p1", node_sink, "in", "image"));

    auto         host = InvokeTypedHost::Make(state);
    GraphRuntime runtime;
    const auto   hr =
        runtime.RunWithHost(plan, "fp_typed_parent", token_.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK) << runtime.GetLastErrorMessage();

    // 引用身份全程保持：sink 收到的即源实例（直塞→子图→typed 投影→
    // 父图数据边，四个边界一个引用）。
    ASSERT_NE(state->received_image, nullptr);
    EXPECT_EQ(state->received_image, state->source_image.Get());
}

// ===========================================================================
// 多值边界输出投影 E2E：子图双类型出口（e_1 int / e_2 string）经真实
// invokeRepository 按编译清单逐口投影回父组件输出、父图两条下游数据边
// 分型消费——int/string 同轮各达各点（单值 image 用例的通则化）。
// ===========================================================================
namespace
{
    const std::string MULTIVALUE_CHILD_GUID =
        "B0000000-0000-0000-0000-000000000004";
    const std::string INT_STRING_SINK_GUID =
        "B0000000-0000-0000-0000-000000000005";

    struct InvokeMultiValueE2EState
    {
        bool        received_int_present = false;
        int64_t     received_int_value = 0;
        bool        received_string_present = false;
        std::string received_string_value;
    };

    // 子图替身（DasGraphTaskImpl 类型化直写的最小同构）：Do 直接产出两个
    // 分型边界口条目（e_1 int / e_2 string）——值出口集合=编译清单。
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
                const DasReadOnlyString    string_key{"in_string"};
                DasPtr<IDasReadOnlyString> string_value;
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

    class InvokeMultiValueHost final
        : public Das::PluginInterface::DasTaskComponentHostImplBase<
              InvokeMultiValueHost>
    {
    public:
        explicit InvokeMultiValueHost(
            std::shared_ptr<InvokeMultiValueE2EState> state)
            : state_(std::move(state))
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

            if (guid_text == MULTIVALUE_CHILD_GUID)
            {
                auto* component = new E2EMultiValueChildComponent();
                component->AddRef();
                *pp_out_component = component;
                return DAS_S_OK;
            }
            if (guid_text == INT_STRING_SINK_GUID)
            {
                auto* component = new E2EIntStringSinkComponent(state_);
                component->AddRef();
                *pp_out_component = component;
                return DAS_S_OK;
            }
            if (guid_text == REPOSITORY_INVOKE_NODE_GUID)
            {
                auto* component = new Das::DasFlowControlTaskComponent(
                    std::string{REPOSITORY_INVOKE_KIND_TEXT},
                    DasPtr<Das::PluginInterface::IDasTaskComponentHost>(this));
                component->AddRef();
                *pp_out_component = component;
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

    private:
        std::shared_ptr<InvokeMultiValueE2EState> state_;
    };
} // namespace

TEST_F(E2EGraphFlowTest, RepositoryInvokeProjectsMultiValueTypedOutputs)
{
    auto state = std::make_shared<InvokeMultiValueE2EState>();

    // 子图快照：单节点子图，boundaryPorts.outputs 声明 e_1（int）/ e_2
    // （string）双出口（类型化导出与逐口投影的消费清单）。
    std::string snapshot_json;
    {
        CompiledGraphPlanDto child_plan;
        child_plan.source_fingerprint = "fp_multivalue_child";
        CompiledNodeSnapshotDto child_snapshot;
        child_snapshot.node_id = "child_node";
        child_snapshot.component_guid = MULTIVALUE_CHILD_GUID;
        child_plan.node_snapshots.push_back(std::move(child_snapshot));
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

        namespace RepositoryInvokeDto =
            Das::Core::TaskScheduler::RepositoryInvoke::Dto;
        RepositoryInvokeDto::ChildExecutionSnapshotDto snapshot;
        snapshot.version = 1;
        snapshot.source_entry_id = 42;
        snapshot.component_guid = MULTIVALUE_CHILD_GUID;
        snapshot.execution_input =
            BuildCompiledPlanShell(yyjson::object(child_plan));
        const auto serialized = Das::Utils::SerializeYyjsonValue(
            Das::Utils::CloneYyjsonValue(yyjson::object(snapshot)));
        ASSERT_TRUE(serialized.has_value());
        snapshot_json = *serialized;
    }

    // 父图：invokeRepository（快照哨兵广播喂线）→ 两个分型消费节点。
    const std::string node_invoke{"n_invoke"};
    const std::string node_sink_int{"n_sink_int"};
    const std::string node_sink_str{"n_sink_str"};

    CompiledGraphPlanDto plan;
    plan.source_fingerprint = "fp_multivalue_parent";
    plan.node_snapshots.push_back(
        MakeSnapshot(node_invoke, REPOSITORY_INVOKE_NODE_GUID));
    plan.node_snapshots.push_back(
        MakeSnapshot(node_sink_int, INT_STRING_SINK_GUID));
    plan.node_snapshots.push_back(
        MakeSnapshot(node_sink_str, INT_STRING_SINK_GUID));
    plan.execution_order = {node_invoke, node_sink_int, node_sink_str};

    PortBindingDto snapshot_broadcast;
    snapshot_broadcast.source_node_id = "$graph_input";
    snapshot_broadcast.source_port_id = "snapshot";
    snapshot_broadcast.target_node_id = node_invoke;
    snapshot_broadcast.target_port_id = "compiledSnapshot";
    snapshot_broadcast.expected_type = "object";
    snapshot_broadcast.default_value = yyjson::value(snapshot_json);
    plan.binding_plan.bindings.push_back(std::move(snapshot_broadcast));

    plan.binding_plan.bindings.push_back(
        MakeBinding(node_invoke, "e_1", node_sink_int, "in_int", "int"));
    plan.binding_plan.bindings.push_back(
        MakeBinding(node_invoke, "e_2", node_sink_str, "in_string", "string"));

    auto         host = InvokeMultiValueHost::Make(state);
    GraphRuntime runtime;
    const auto   hr = runtime.RunWithHost(
        plan,
        "fp_multivalue_parent",
        token_.Get(),
        host.Get());
    ASSERT_EQ(hr, DAS_S_OK) << runtime.GetLastErrorMessage();

    // 两下游各收各值（逐口投影 + 分型直连）。
    ASSERT_TRUE(state->received_int_present)
        << "the int downstream node never received e_1";
    EXPECT_EQ(state->received_int_value, 4242);
    ASSERT_TRUE(state->received_string_present)
        << "the string downstream node never received e_2";
    EXPECT_EQ(state->received_string_value, "multi-out");
}
