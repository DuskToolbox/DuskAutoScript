#include <das/Core/ForeignInterfaceHost/TaskComponentFactoryManager.h>
#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/DoAdapter.h>
#include <das/Core/GraphRuntime/GraphCompiler.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/GraphRuntime/GraphRuntime.h>
#include <das/Core/GraphRuntime/PortFrame.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/Utils/TaskComponentPorts.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
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
        component->AddRef();
        *pp_out_component = component;
        return DAS_S_OK;
    }
};

// launch 执行失败（空配置）→ 图以专用错误码终止，错误文本携带节点 id
// 冒泡（GraphRuntime 现有失败接管路径，无 status=failed 继续跑）。
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
}

// ---- 真实 while + 数据回边经典形状（FlowControl 源码静态编入本测试目标） ----

constexpr std::string_view WHILE_COMPONENT_GUID_TEXT =
    "68F10005-0000-4000-8000-000000000005";
constexpr std::string_view WHILE_KIND_TEXT = "das.flow.while";
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
                std::string{CONCAT_STRING_COMPONENT_GUID_TEXT}))
        {
            kind = CONCAT_STRING_KIND_TEXT;
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
    EXPECT_EQ(plan.loop_layers[0].loop_id, "loop::" + node_while);
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

// 标量常量组件直驱：typed 输出口 + status/signal 三件套（bool/int64/
// double 三型；config 缺 value 键回落各类型零值=缺省即语义）。
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
        EXPECT_NE(
            PortMapGetString(out.Get(), "signals").find("signal_out"),
            std::string::npos);
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
