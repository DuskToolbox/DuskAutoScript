#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Plugins/DasGraphTask/DasGraphTaskAdapter.h>
#include <das/Plugins/DasGraphTask/DasGraphTaskImpl.h>
#include <das/Plugins/DasGraphTask/DasGraphTaskPluginPackage.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <gtest/gtest.h>

#include "../src/DasGraphTaskAuthoringSession.h"

#include <optional>
#include <string>
#include <string_view>

using namespace Das::Plugins::DasGraphTask;
using DAS::DasPtr;

// ---- Minimal mock for IDasTaskComponentHost ----

class MockTaskComponentHost final
    : public Das::PluginInterface::IDasTaskComponentHost
{
    std::atomic<uint32_t> ref_count_{0};

public:
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
        if (iid == DasIidOf<Das::PluginInterface::IDasTaskComponentHost>())
        {
            *pp_out =
                static_cast<Das::PluginInterface::IDasTaskComponentHost*>(this);
            AddRef();
            return DAS_S_OK;
        }
        *pp_out = nullptr;
        return DAS_E_NO_INTERFACE;
    }

    DasResult DAS_STD_CALL CreateTaskComponent(
        const DasGuid&                            component_guid,
        Das::PluginInterface::IDasTaskComponent** pp_out_component) override
    {
        std::ignore = component_guid;
        if (pp_out_component == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_component = nullptr;
        return DAS_E_NOT_FOUND;
    }
};

// ---- Minimal stop token mock ----

class MockStopTokenForTask final
    : public Das::PluginInterface::DasStopTokenImplBase<MockStopTokenForTask>
{
    bool cancelled_ = false;

public:
    explicit MockStopTokenForTask(bool cancelled = false)
        : cancelled_(cancelled)
    {
    }

    DAS_IMPL StopRequested(bool* canStop) override
    {
        if (!canStop)
        {
            return DAS_E_INVALID_POINTER;
        }
        *canStop = cancelled_;
        return DAS_S_OK;
    }
};

// ---- Tests ----

// 空 store 经 Core 编译链产出合法空 plan 文本（executionInput 内层）——
// 空 artifact 已被执行门拒绝，测试需要「合法的空」必须走真编译产物。
std::string EmptyCompiledPlanText()
{
    GraphAuthoringSessionState* state = nullptr;
    EXPECT_EQ(CreateGraphAuthoringSession(nullptr, &state), DAS_S_OK);
    DasPtr<Das::ExportInterface::IDasJson> plan_json;
    EXPECT_EQ(
        GraphAuthoringSessionCompile(state, nullptr, plan_json.Put()),
        DAS_S_OK);
    DestroyGraphAuthoringSession(state);
    EXPECT_NE(plan_json.Get(), nullptr);

    DasPtr<IDasReadOnlyString> plan_str;
    EXPECT_EQ(plan_json->ToString(0, plan_str.Put()), DAS_S_OK);
    const char* utf8 = nullptr;
    EXPECT_EQ(plan_str->GetUtf8(&utf8), DAS_S_OK);
    const auto envelope = yyjson::read(std::string(utf8)).as_object();
    EXPECT_TRUE(envelope.has_value());
    const auto compile_section =
        (*envelope)[std::string_view("compile")].as_object();
    EXPECT_TRUE(compile_section.has_value());
    const auto inner_plan = Das::Utils::SerializeYyjsonValue(
        (*compile_section)[std::string_view("executionInput")]);
    EXPECT_TRUE(inner_plan.has_value());
    return inner_plan.value_or(std::string{});
}

// 把 plan 文本装进 executionInput 输入口（组件读该端口的唯一喂线形态）。
DasPtr<Das::ExportInterface::IDasPortMap> MakeExecutionInputMap(
    const std::string& plan_text)
{
    DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    EXPECT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    const DasReadOnlyString execution_input_key{"executionInput"};
    const DasReadOnlyString execution_input_value{plan_text.c_str()};
    EXPECT_EQ(
        input_map->SetString(
            execution_input_key.Get(),
            execution_input_value.Get()),
        DAS_S_OK);
    return input_map;
}

TEST(DasGraphTaskTest, ConstructWithNullHost)
{
    auto task = DasGraphTaskImpl::Make(nullptr);
    ASSERT_NE(task.Get(), nullptr);
}

TEST(DasGraphTaskTest, DoReturnsErrorWhenHostIsNull)
{
    auto task = DasGraphTaskImpl::Make(nullptr);
    ASSERT_NE(task.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasPortMap> result;
    const DasResult hr = task->Do(nullptr, nullptr, result.Put());
    EXPECT_TRUE(DAS::IsFailed(hr));
}

TEST(DasGraphTaskTest, GetGuidReturnsNonEmpty)
{
    auto task = DasGraphTaskImpl::Make(nullptr);
    ASSERT_NE(task.Get(), nullptr);

    DasGuid         guid{};
    const DasResult hr = task->GetGuid(&guid);
    EXPECT_TRUE(DAS::IsOk(hr));
}

TEST(DasGraphTaskTest, DoWithMockHostReturnsSuccessOnEmptyPlan)
{
    auto host = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(
        new MockTaskComponentHost());
    auto task = DasGraphTaskImpl::Make(host.Get());
    ASSERT_NE(task.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasPortMap> result;
    const DasResult                           hr = task->Do(
        nullptr,
        MakeExecutionInputMap(EmptyCompiledPlanText()).Get(),
        result.Put());
    // Empty plan runs without error (empty graph).
    EXPECT_TRUE(DAS::IsOk(hr));
}

TEST(DasGraphTaskTest, DoStopTokenCancel)
{
    auto host = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(
        new MockTaskComponentHost());
    auto task = DasGraphTaskImpl::Make(host.Get());
    ASSERT_NE(task.Get(), nullptr);

    auto cancelled_token = MockStopTokenForTask::Make(true);
    DasPtr<Das::ExportInterface::IDasPortMap> result;
    const DasResult hr = task->Do(cancelled_token.Get(), nullptr, result.Put());
    // RunWithHost should propagate cancellation.
    EXPECT_TRUE(DAS::IsFailed(hr));
}

// Smoke: a plan produced by the authoring Compile path can be fed into
// DasGraphTaskImpl::Do (executionInput port) and execute without error on an
// empty graph. Validates the authoring→compile→Do handoff (DAS-77 Wave3 A1).
TEST(DasGraphTaskTest, AuthoringCompilePlanFeedsIntoDo)
{
    // Compile an empty store via the Core authoring session C ABI.
    GraphAuthoringSessionState* state = nullptr;
    ASSERT_EQ(CreateGraphAuthoringSession(nullptr, &state), DAS_S_OK);
    ASSERT_NE(state, nullptr);
    DasPtr<Das::ExportInterface::IDasJson> plan_json;
    ASSERT_EQ(
        GraphAuthoringSessionCompile(state, nullptr, plan_json.Put()),
        DAS_S_OK);
    DestroyGraphAuthoringSession(state);
    ASSERT_NE(plan_json.Get(), nullptr);

    // Serialise the plan to the executionInput input port. The Core Compile
    // returns the strict authoring envelope
    // {compile:{executionInput:<plan>}, diagnostics}; extract the inner plan
    // the same way the scheduler does before feeding the port.
    DasPtr<IDasReadOnlyString> plan_str;
    ASSERT_EQ(plan_json->ToString(0, plan_str.Put()), DAS_S_OK);
    ASSERT_NE(plan_str.Get(), nullptr);
    const char* plan_utf8 = nullptr;
    ASSERT_EQ(plan_str->GetUtf8(&plan_utf8), DAS_S_OK);
    ASSERT_NE(plan_utf8, nullptr);

    const auto envelope_obj = yyjson::read(std::string(plan_utf8)).as_object();
    ASSERT_TRUE(envelope_obj.has_value());
    ASSERT_TRUE(envelope_obj->contains(std::string_view("compile")));
    const auto compile_section =
        (*envelope_obj)[std::string_view("compile")].as_object();
    ASSERT_TRUE(compile_section.has_value());
    ASSERT_TRUE(compile_section->contains(std::string_view("executionInput")));
    const auto inner_plan = Das::Utils::SerializeYyjsonValue(
        (*compile_section)[std::string_view("executionInput")]);
    ASSERT_TRUE(inner_plan.has_value());

    DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    const DasReadOnlyString execution_input_key{"executionInput"};
    const DasReadOnlyString execution_input_value{inner_plan->c_str()};
    ASSERT_EQ(
        input_map->SetString(
            execution_input_key.Get(),
            execution_input_value.Get()),
        DAS_S_OK);

    // Feed the compiled plan into Do and confirm it runs (empty plan → OK).
    auto host = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(
        new MockTaskComponentHost());
    auto task = DasGraphTaskImpl::Make(host.Get());
    ASSERT_NE(task.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasPortMap> result;
    const DasResult hr = task->Do(nullptr, input_map.Get(), result.Put());
    EXPECT_TRUE(DAS::IsOk(hr));
}

// ---- Package-level host injection into factory and adapter ----

namespace
{
    // 单 componentRef 节点图：节点组件经 host 创建（itemId GUID 形态——
    // GraphRuntime 数据面要求）。
    constexpr std::string_view HOST_INJECTION_FORM_DOC = R"({
        "items": [
            {
                "itemId": "11111111-1111-1111-1111-111111111111",
                "target": {
                    "targetKind": "componentRef",
                    "componentRef": {
                        "kind": "componentRef",
                        "componentGuid": "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA",
                        "pluginGuid": "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"
                    }
                },
                "settings": {}
            }
        ]
    })";

    // 记录并放行的节点组件替身。
    class SucceedingNodeComponent final
        : public Das::PluginInterface::IDasTaskComponent
    {
    public:
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
            if (iid == DasIidOf<Das::PluginInterface::IDasTaskComponent>())
            {
                *pp_out =
                    static_cast<Das::PluginInterface::IDasTaskComponent*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            if (p_out_guid == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            DasGuid guid{};
            guid.data1 = 0xDDDDDDDD;
            guid.data2 = 0xDDDD;
            guid.data3 = 0xDDDD;
            const uint8_t tail[8] =
                {0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD, 0xDD};
            std::memcpy(guid.data4, tail, sizeof(tail));
            *p_out_guid = guid;
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            return CreateIDasReadOnlyStringFromUtf8(
                "SucceedingNodeComponent",
                pp_out_name);
        }

        DasResult DAS_STD_CALL ApplySettingsChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_result_json = nullptr;
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        Do(Das::PluginInterface::IDasStopToken*,
           Das::ExportInterface::IDasReadOnlyPortMap*,
           Das::ExportInterface::IDasPortMap** pp_out_port_map) override
        {
            if (pp_out_port_map == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_port_map = nullptr;
            ++do_count;
            return DAS_S_OK;
        }

        int do_count = 0;

    private:
        std::atomic<uint32_t> ref_count_{1};
    };

    // 服务单一组件 guid 的 host 替身（记录创建次数）。
    class NodeServingHost final
        : public Das::PluginInterface::IDasTaskComponentHost
    {
    public:
        explicit NodeServingHost(DasGuid served_guid)
            : served_guid_(served_guid)
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
            if (iid == DasIidOf<Das::PluginInterface::IDasTaskComponentHost>())
            {
                *pp_out =
                    static_cast<Das::PluginInterface::IDasTaskComponentHost*>(
                        this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL CreateTaskComponent(
            const DasGuid&                            component_guid,
            Das::PluginInterface::IDasTaskComponent** pp_out_component) override
        {
            if (pp_out_component == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_component = nullptr;
            if (!(component_guid == served_guid_))
            {
                return DAS_E_NOT_FOUND;
            }
            ++create_count;
            auto* component = new SucceedingNodeComponent();
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }

        int create_count = 0;

    private:
        DasGuid               served_guid_;
        std::atomic<uint32_t> ref_count_{1};
    };

    DasGuid NodeComponentGuid()
    {
        DasGuid guid{};
        guid.data1 = 0xAAAAAAAA;
        guid.data2 = 0xAAAA;
        guid.data3 = 0xAAAA;
        const uint8_t tail[8] =
            {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
        std::memcpy(guid.data4, tail, sizeof(tail));
        return guid;
    }

    // 编译单节点图并提取 executionInput 内层 plan 的序列化文本。
    std::string CompileOneNodePlanText()
    {
        GraphAuthoringSessionState* state = nullptr;
        const std::string           document{HOST_INJECTION_FORM_DOC};
        DasPtr<IDasReadOnlyString>  context_string;
        EXPECT_EQ(
            CreateIDasReadOnlyStringFromUtf8(
                document.c_str(),
                context_string.Put()),
            DAS_S_OK);
        EXPECT_EQ(
            CreateGraphAuthoringSession(context_string.Get(), &state),
            DAS_S_OK);
        DasPtr<Das::ExportInterface::IDasJson> compiled;
        EXPECT_EQ(
            GraphAuthoringSessionCompile(state, nullptr, compiled.Put()),
            DAS_S_OK);
        DestroyGraphAuthoringSession(state);
        EXPECT_NE(compiled.Get(), nullptr);

        DasPtr<IDasReadOnlyString> compiled_text;
        EXPECT_EQ(compiled->ToString(0, compiled_text.Put()), DAS_S_OK);
        const char* utf8 = nullptr;
        EXPECT_EQ(compiled_text->GetUtf8(&utf8), DAS_S_OK);

        const auto envelope = yyjson::read(std::string(utf8)).as_object();
        EXPECT_TRUE(envelope.has_value());
        const auto compile_section =
            (*envelope)[std::string_view("compile")].as_object();
        EXPECT_TRUE(compile_section.has_value());
        const auto inner_plan = Das::Utils::SerializeYyjsonValue(
            (*compile_section)[std::string_view("executionInput")]);
        EXPECT_TRUE(inner_plan.has_value());
        return *inner_plan;
    }
} // namespace

// 输入键统一为 executionInput：经该端口喂入编译产物时节点组件经 host
// 真实创建并执行（组件读旧键时节点永不执行——scheduler 直驱与调用组件
// 喂线两条执行路径同键的行为锁定）。
TEST(DasGraphTaskTest, ExecutionInputPortDrivesNodeExecution)
{
    const auto plan_text = CompileOneNodePlanText();
    ASSERT_FALSE(plan_text.empty());

    auto host = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(
        new NodeServingHost(NodeComponentGuid()));
    auto* serving_host = static_cast<NodeServingHost*>(host.Get());
    auto  task = DasGraphTaskImpl::Make(host.Get());
    ASSERT_NE(task.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    const DasReadOnlyString execution_input_key{"executionInput"};
    const DasReadOnlyString execution_input_value{plan_text.c_str()};
    ASSERT_EQ(
        input_map->SetString(
            execution_input_key.Get(),
            execution_input_value.Get()),
        DAS_S_OK);

    DasPtr<Das::ExportInterface::IDasPortMap> result;
    const DasResult hr = task->Do(nullptr, input_map.Get(), result.Put());
    EXPECT_TRUE(DAS::IsOk(hr));
    EXPECT_GE(serving_host->create_count, 1);
}

// 工厂经包构造拿 host：包默认存储 → CreateFeatureInterface(2) → 工厂 →
// 组件持有可用 host（无任何对工厂的直接注入调用）。
TEST(DasGraphTaskTest, PackageInjectsHostIntoFactoryCreatedComponent)
{
    auto package = DasGraphTaskPluginPackage::Make();
    ASSERT_NE(package.Get(), nullptr);
    auto host = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(
        new MockTaskComponentHost());
    ASSERT_EQ(package->SetTaskComponentHost(host.Get()), DAS_S_OK);

    DasPtr<IDasBase> feature;
    ASSERT_EQ(package->CreateFeatureInterface(2, feature.Put()), DAS_S_OK);
    DasPtr<Das::PluginInterface::IDasTaskComponentFactory> factory;
    ASSERT_EQ(feature.As(factory.Put()), DAS_S_OK);
    ASSERT_NE(factory.Get(), nullptr);

    DasPtr<Das::PluginInterface::IDasTaskComponent> component;
    ASSERT_EQ(
        factory->CreateComponent(DasIidOf<DasGraphTaskImpl>(), component.Put()),
        DAS_S_OK);
    ASSERT_NE(component.Get(), nullptr);

    // 组件持有可用 host：空 plan（真编译产物；空 artifact 已被执行门拒
    // 绝）经 host 路径执行成功（host 缺失时 Do 结构化失败）。
    DasPtr<Das::ExportInterface::IDasPortMap> result;
    EXPECT_TRUE(
        DAS::IsOk(component->Do(
            nullptr,
            MakeExecutionInputMap(EmptyCompiledPlanText()).Get(),
            result.Put())));
}

// adapter 同链：包注入 host → CreateFeatureInterface(0) → adapter 经 host
// 路径执行单节点图（hostless 回退分支不触发，节点组件经 host 创建）。
TEST(DasGraphTaskTest, PackageInjectedHostReachesAdapterExecution)
{
    const auto plan_text = CompileOneNodePlanText();
    ASSERT_FALSE(plan_text.empty());

    auto host = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(
        new NodeServingHost(NodeComponentGuid()));
    auto* serving_host = static_cast<NodeServingHost*>(host.Get());

    auto package = DasGraphTaskPluginPackage::Make();
    ASSERT_NE(package.Get(), nullptr);
    ASSERT_EQ(package->SetTaskComponentHost(host.Get()), DAS_S_OK);

    DasPtr<IDasBase> feature;
    ASSERT_EQ(package->CreateFeatureInterface(0, feature.Put()), DAS_S_OK);
    DasPtr<Das::PluginInterface::IDasTask> task;
    ASSERT_EQ(feature.As(task.Put()), DAS_S_OK);
    ASSERT_NE(task.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasJson> plan_json;
    ASSERT_EQ(
        ParseDasJsonFromString(plan_text.c_str(), plan_json.Put()),
        DAS_S_OK);
    ASSERT_NE(plan_json.Get(), nullptr);

    const DasResult hr = task->Do(nullptr, nullptr, plan_json.Get());
    EXPECT_TRUE(DAS::IsOk(hr));
    EXPECT_GE(serving_host->create_count, 1);
}

// 委托执行契约：adapter 无 host 时以内核组件的结构化错误失败
// （DAS_E_INVALID_POINTER——内核 host 检查），hostless 直驱回退随委托化
// 消亡。现状 adapter 直驱 GraphRuntime（hostless 回退），断言不成立即红。
TEST(DasGraphTaskTest, AdapterDoDelegatesToComponentKernel)
{
    DasGraphTaskAdapter adapter{nullptr};

    // 空输入 + 无 host：委托内核在 host 检查处结构化失败。
    EXPECT_EQ(adapter.Do(nullptr, nullptr, nullptr), DAS_E_INVALID_POINTER);

    // 有编译 plan 输入 + 无 host：同样经内核结构化失败（hr 透传）。
    const auto plan_text = CompileOneNodePlanText();
    ASSERT_FALSE(plan_text.empty());
    DasPtr<Das::ExportInterface::IDasJson> plan_json;
    ASSERT_EQ(
        ParseDasJsonFromString(plan_text.c_str(), plan_json.Put()),
        DAS_S_OK);
    ASSERT_NE(plan_json.Get(), nullptr);
    EXPECT_EQ(
        adapter.Do(nullptr, nullptr, plan_json.Get()),
        DAS_E_INVALID_POINTER);
}

// 会话构造携带畸形 actionCatalog（空 portType 条目）时：Core 的
// SetActionCatalog 整包拒绝，接线层记录 WARN 后会话保持可用（空目录），
// 构造与 GetDocument 均不得崩溃或中断——拒绝路径的失败分支回归锁。
TEST(DasGraphTaskTest, AuthoringSessionSurvivesMalformedCatalogContext)
{
    const std::string context =
        R"({"properties":{},"actionCatalog":[{"actionId":"{g1}","ports":[{"portId":"x","portType":"","direction":"input"}]}]})";

    DasGraphTaskAuthoringSession           session{context};
    DasPtr<Das::ExportInterface::IDasJson> request;
    ASSERT_EQ(ParseDasJsonFromString("{}", request.Put()), DAS_S_OK);
    ASSERT_NE(request.Get(), nullptr);
    DasPtr<Das::ExportInterface::IDasJson> document;
    EXPECT_EQ(session.GetDocument(request.Get(), document.Put()), DAS_S_OK);
    ASSERT_NE(document.Get(), nullptr);

    // 被拒的 catalog 不得投影：GetDocument 文档不带 actionCatalog 键。
    DasPtr<IDasReadOnlyString> document_text;
    ASSERT_EQ(document->ToString(0, document_text.Put()), DAS_S_OK);
    const char* utf8 = nullptr;
    ASSERT_EQ(document_text->GetUtf8(&utf8), DAS_S_OK);
    ASSERT_NE(utf8, nullptr);
    const std::string_view text{utf8};
    EXPECT_EQ(text.find("actionCatalog"), std::string_view::npos);
}

// ---------------------------------------------------------------------
// 调用节点端口投影：已绑 graph 条目的节点在 GetDocument 响应自带条目
// 边界端口（ports[] 幂等合入，现算现投影不落盘）；未绑节点零注入；内部
// 条目获取请求的投影关闭旗标断开引用环递归。
// ---------------------------------------------------------------------

namespace
{
    // 条目 Get 响应信封：acceptedProperties 是条目的 GraphDocumentDto 持久
    // 化源（内部节点引用目录组件 {gc}，其声明端口面在会话 catalog 中）。
    constexpr std::string_view BOUND_ENTRY_RESPONSE = R"json({
        "entryId": 42,
        "taskTypeGuid": "F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3",
        "revision": 1,
        "diagnostics": [],
        "acceptedProperties": {
            "nodes": [{
                "nodeId": "inner_1",
                "target": {
                    "targetKind": "componentRef",
                    "componentRef": {
                        "kind": "componentRef",
                        "componentGuid": "{gc}",
                        "pluginGuid": "{gp}"
                    }
                },
                "settings": {}
            }],
            "edges": []
        }
    })json";

    // 父图上下文：n_1 绑 repositoryRef=42（catalog 声明该字段类型为
    // taskRepositoryRef）；目录组件 {gc} 的端口面在 actionCatalog。
    constexpr std::string_view BOUND_NODE_CONTEXT = R"json({
        "properties": {
            "nodes": [{
                "nodeId": "n_1",
                "target": {
                    "targetKind": "componentRef",
                    "componentRef": {
                        "kind": "componentRef",
                        "componentGuid": "{gi}",
                        "pluginGuid": "{gp}"
                    }
                },
                "settings": {"repositoryRef": 42}
            }],
            "edges": []
        },
        "actionCatalog": [
            {
                "actionId": "{gi}",
                "config": [{"name": "repositoryRef", "type": "taskRepositoryRef"}]
            },
            {
                "actionId": "{gc}",
                "ports": [
                    {"portId": "seed", "portType": "int", "direction": "input"},
                    {"portId": "result", "portType": "string", "direction": "output"}
                ]
            }
        ]
    })json";

    // 获取缝注入守卫：测试结束（含断言早退）恢复生产实现。
    struct ScopedEntryDocumentFetcherOverride
    {
        explicit ScopedEntryDocumentFetcherOverride(
            Das::Plugins::DasGraphTask::RepositoryEntryDocumentFetcher fetcher)
        {
            Das::Plugins::DasGraphTask::
                SetRepositoryEntryDocumentFetcherForTest(std::move(fetcher));
        }

        ~ScopedEntryDocumentFetcherOverride()
        {
            Das::Plugins::DasGraphTask::
                SetRepositoryEntryDocumentFetcherForTest(nullptr);
        }
    };

    std::string DocumentTextOf(
        Das::Plugins::DasGraphTask::DasGraphTaskAuthoringSession& session,
        std::string_view                                          request_text)
    {
        DasPtr<Das::ExportInterface::IDasJson> request;
        if (!request_text.empty())
        {
            ParseDasJsonFromString(
                std::string(request_text).c_str(),
                request.Put());
        }
        DasPtr<Das::ExportInterface::IDasJson> document;
        EXPECT_EQ(session.GetDocument(request.Get(), document.Put()), DAS_S_OK);
        EXPECT_NE(document.Get(), nullptr);
        if (document.Get() == nullptr)
        {
            return {};
        }
        DasPtr<IDasReadOnlyString> document_text;
        EXPECT_EQ(document->ToString(0, document_text.Put()), DAS_S_OK);
        const char* utf8 = nullptr;
        if (document_text.Get() == nullptr
            || DAS::IsFailed(document_text->GetUtf8(&utf8)) || utf8 == nullptr)
        {
            return {};
        }
        return std::string(utf8);
    }

    // 统计 (portId, direction) 在首个节点 ports[] 的出现次数。
    std::size_t CountNodePort(
        const std::string& document_text,
        std::string_view   port_id,
        std::string_view   direction)
    {
        auto parsed = Das::Utils::ParseYyjsonFromString(document_text);
        if (!parsed.has_value())
        {
            return 0;
        }
        auto root = parsed->as_object();
        if (!root.has_value() || !root->contains(std::string_view("document")))
        {
            return 0;
        }
        auto document_object =
            (*root)[std::string_view("document")].as_object();
        if (!document_object.has_value())
        {
            return 0;
        }
        auto graph_object =
            (*document_object)[std::string_view("graph")].as_object();
        if (!graph_object.has_value())
        {
            return 0;
        }
        auto nodes = (*graph_object)[std::string_view("nodes")].as_array();
        if (!nodes.has_value() || nodes->empty())
        {
            return 0;
        }
        auto first_node = (*nodes)[0].as_object();
        if (!first_node.has_value()
            || !first_node->contains(std::string_view("ports")))
        {
            return 0;
        }
        auto ports = (*first_node)[std::string_view("ports")].as_array();
        if (!ports.has_value())
        {
            return 0;
        }
        std::size_t count = 0;
        for (const auto& port : *ports)
        {
            auto port_object = port.as_object();
            if (!port_object.has_value())
            {
                continue;
            }
            if ((*port_object)[std::string_view("portId")].as_string().value_or(
                    "")
                    == port_id
                && (*port_object)[std::string_view("direction")]
                           .as_string()
                           .value_or("")
                       == direction)
            {
                ++count;
            }
        }
        return count;
    }
} // namespace

TEST(DasGraphTaskTest, AuthoringSessionProjectsBoundaryPortsOnBoundCallNode)
{
    int                                      fetch_calls = 0;
    const ScopedEntryDocumentFetcherOverride fetcher_override{
        [&fetch_calls](int64_t entry_id) -> std::optional<std::string>
        {
            ++fetch_calls;
            if (entry_id != 42)
            {
                return std::nullopt;
            }
            return std::string{BOUND_ENTRY_RESPONSE};
        }};

    Das::Plugins::DasGraphTask::DasGraphTaskAuthoringSession session{
        std::string{BOUND_NODE_CONTEXT}};

    const auto document_text = DocumentTextOf(session, "{}");
    ASSERT_FALSE(document_text.empty());
    EXPECT_EQ(fetch_calls, 1);

    // 边界输入 seed / 边界输出 result 投影进该节点 ports[]（严格五键
    // wire 形，category 按 port_type 派生）。
    EXPECT_EQ(CountNodePort(document_text, "seed", "input"), 1u);
    EXPECT_EQ(CountNodePort(document_text, "result", "output"), 1u);
}

TEST(DasGraphTaskTest, AuthoringSessionBoundaryProjectionIsIdempotent)
{
    const ScopedEntryDocumentFetcherOverride fetcher_override{
        [](int64_t) -> std::optional<std::string>
        { return std::string{BOUND_ENTRY_RESPONSE}; }};

    Das::Plugins::DasGraphTask::DasGraphTaskAuthoringSession session{
        std::string{BOUND_NODE_CONTEXT}};

    // 连续两次 Get：现算现投影，端口不重复叠加。
    const auto first = DocumentTextOf(session, "{}");
    ASSERT_FALSE(first.empty());
    const auto second = DocumentTextOf(session, "{}");
    ASSERT_FALSE(second.empty());
    EXPECT_EQ(CountNodePort(second, "seed", "input"), 1u);
    EXPECT_EQ(CountNodePort(second, "result", "output"), 1u);
}

TEST(DasGraphTaskTest, AuthoringSessionSkipsProjectionForUnboundNode)
{
    int                                      fetch_calls = 0;
    const ScopedEntryDocumentFetcherOverride fetcher_override{
        [&fetch_calls](int64_t) -> std::optional<std::string>
        {
            ++fetch_calls;
            return std::string{BOUND_ENTRY_RESPONSE};
        }};

    // settings 无 repositoryRef 值：字段声明在场但值为空 → 未绑条目。
    constexpr std::string_view unbound_context = R"json({
        "properties": {
            "nodes": [{
                "nodeId": "n_1",
                "target": {
                    "targetKind": "componentRef",
                    "componentRef": {
                        "kind": "componentRef",
                        "componentGuid": "{gi}",
                        "pluginGuid": "{gp}"
                    }
                },
                "settings": {}
            }],
            "edges": []
        },
        "actionCatalog": [
            {"actionId": "{gi}", "config": [{"name": "repositoryRef", "type": "taskRepositoryRef"}]}
        ]
    })json";

    Das::Plugins::DasGraphTask::DasGraphTaskAuthoringSession session{
        std::string{unbound_context}};

    const auto document_text = DocumentTextOf(session, "{}");
    ASSERT_FALSE(document_text.empty());
    EXPECT_EQ(fetch_calls, 0);
    EXPECT_EQ(CountNodePort(document_text, "seed", "input"), 0u);
}

TEST(DasGraphTaskTest, AuthoringSessionBoundaryProjectionGuardDisablesFetch)
{
    int                                      fetch_calls = 0;
    const ScopedEntryDocumentFetcherOverride fetcher_override{
        [&fetch_calls](int64_t) -> std::optional<std::string>
        {
            ++fetch_calls;
            return std::string{BOUND_ENTRY_RESPONSE};
        }};

    Das::Plugins::DasGraphTask::DasGraphTaskAuthoringSession session{
        std::string{BOUND_NODE_CONTEXT}};

    // 内部条目获取请求形态（投影关闭旗标）：零获取零注入——引用环的
    // Get 投影递归在此断开。
    const auto document_text =
        DocumentTextOf(session, R"({"boundaryPortProjection":false})");
    ASSERT_FALSE(document_text.empty());
    EXPECT_EQ(fetch_calls, 0);
    EXPECT_EQ(CountNodePort(document_text, "seed", "input"), 0u);
}
