#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/IDasBase.h>
#include <das/Plugins/DasGraphTask/DasGraphTaskAdapter.h>
#include <das/Plugins/DasGraphTask/DasGraphTaskImpl.h>
#include <das/Plugins/DasGraphTask/DasGraphTaskPluginPackage.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasStringVector.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <das/_autogen/idl/header/IDasLogRecorder.generated.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/header/IDasStringVector.generated.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasImage.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <gtest/gtest.h>

#include "../src/DasGraphTaskAuthoringSession.h"
#include "../src/GraphTaskFocusLog.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Das::Plugins::DasGraphTask;
using DAS::DasPtr;

// Windows 下 path / "窄串字面量" 会经 ACP 隐式转换（ASCII 偶然安全，非
// ASCII 字面量不可逆乱码）——路径拼接字面量按平台宽度分流。
#ifdef _WIN32
#define GRAPH_TASK_TEST_PATH_LITERAL(text) L##text
#else
#define GRAPH_TASK_TEST_PATH_LITERAL(text) text
#endif

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

// 空 plan 文本（executionInput 内层）经 Core 编译链产出——空 artifact 已
// 被执行门拒绝，测试需要「合法的空」必须走真编译产物。空 graph 会话现
// 预置 begin/end 节点，零节点 plan 由空 items 的 formSequence 上下文提
// 供（LINEAR 文档不预置）。
std::string EmptyCompiledPlanText()
{
    GraphAuthoringSessionState* state = nullptr;
    DasPtr<IDasReadOnlyString>  context_string;
    EXPECT_EQ(
        CreateIDasReadOnlyStringFromUtf8(
            R"({"items":[]})",
            context_string.Put()),
        DAS_S_OK);
    EXPECT_EQ(
        CreateGraphAuthoringSession(context_string.Get(), &state),
        DAS_S_OK);
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
// 空 graph 会话现预置 begin/end 节点——零节点 plan 由空 items 的
// formSequence 上下文提供（LINEAR 文档不预置）。
TEST(DasGraphTaskTest, AuthoringCompilePlanFeedsIntoDo)
{
    // Compile an empty linear store via the Core authoring session C ABI.
    GraphAuthoringSessionState* state = nullptr;
    DasPtr<IDasReadOnlyString>  context_string;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8(
            R"({"items":[]})",
            context_string.Put()),
        DAS_S_OK);
    ASSERT_EQ(
        CreateGraphAuthoringSession(context_string.Get(), &state),
        DAS_S_OK);
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

// ---------------------------------------------------------------------
// begin/end 边界组件：manifest 双组件（工厂按 GUID 分派）+ 空图预置 +
// end.值(int) → DasGraphTaskImpl::Do 返回码链。
// ---------------------------------------------------------------------

namespace
{
    // 与 manifest 字面量独立钉值：常量头/manifest/预设三处任一漂移，以
    // 下断言即红。
    constexpr std::string_view BEGIN_COMPONENT_GUID_TEXT =
        "A7D3F1B5-9E2C-4A8D-B6F0-3C5E7D9B1A4E";
    constexpr std::string_view END_COMPONENT_GUID_TEXT =
        "E1B7C9D3-5F4A-4E2B-8D6C-7A9E1F3B5C2D";
    constexpr std::string_view PROBE_COMPONENT_GUID_TEXT =
        "77777777-7777-7777-7777-777777777777";
    constexpr std::string_view NEGATIVE_PROBE_COMPONENT_GUID_TEXT =
        "88888888-8888-8888-8888-888888888888";

    DasGuid GuidOfText(std::string_view guid_text)
    {
        DasGuid           guid{};
        const std::string owned{guid_text};
        DasMakeDasGuid(owned.c_str(), &guid);
        return guid;
    }

    std::string JsonTextOf(Das::ExportInterface::IDasJson* json)
    {
        if (json == nullptr)
        {
            return {};
        }
        DasPtr<IDasReadOnlyString> text;
        if (DAS::IsFailed(json->ToString(0, text.Put()))
            || text.Get() == nullptr)
        {
            return {};
        }
        const char* utf8 = nullptr;
        if (DAS::IsFailed(text->GetUtf8(&utf8)) || utf8 == nullptr)
        {
            return {};
        }
        return std::string(utf8);
    }

    // 压掉全部空白——子串断言与序列化排版解耦。
    std::string CompactText(const std::string& text)
    {
        std::string compacted;
        compacted.reserve(text.size());
        for (const char c : text)
        {
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            {
                compacted.push_back(c);
            }
        }
        return compacted;
    }

    struct GraphNodeWireShape
    {
        std::string node_id;
        std::string component_guid;
        std::string target_kind;
    };

    // Get/Apply 响应信封 → document.graph.nodes 逐节点形状（graph 视图
    // 缺席时为空集）。
    std::vector<GraphNodeWireShape> GraphNodesOfResponseText(
        const std::string& response_text)
    {
        std::vector<GraphNodeWireShape> nodes;
        auto                            root = yyjson::read(response_text);
        auto                            root_obj = root.as_object();
        if (!root_obj || !root_obj->contains(std::string_view("document")))
        {
            return nodes;
        }
        auto document_obj =
            (*root_obj)[std::string_view("document")].as_object();
        if (!document_obj || !document_obj->contains(std::string_view("graph")))
        {
            return nodes;
        }
        auto graph_obj = (*document_obj)[std::string_view("graph")].as_object();
        if (!graph_obj || !graph_obj->contains(std::string_view("nodes")))
        {
            return nodes;
        }
        auto nodes_arr = (*graph_obj)[std::string_view("nodes")].as_array();
        if (!nodes_arr.has_value())
        {
            return nodes;
        }
        for (const auto& node_value : *nodes_arr)
        {
            auto node_obj = node_value.as_object();
            if (!node_obj.has_value())
            {
                continue;
            }
            GraphNodeWireShape node;
            if (auto v = (*node_obj)[std::string_view("id")].as_string())
            {
                node.node_id = std::string(*v);
            }
            if (auto v =
                    (*node_obj)[std::string_view("componentGuid")].as_string())
            {
                node.component_guid = std::string(*v);
            }
            if (auto v =
                    (*node_obj)[std::string_view("targetKind")].as_string())
            {
                node.target_kind = std::string(*v);
            }
            nodes.push_back(std::move(node));
        }
        return nodes;
    }

    bool ResponseDiagnosticsEmpty(const std::string& response_text)
    {
        auto root = yyjson::read(response_text);
        auto root_obj = root.as_object();
        if (!root_obj || !root_obj->contains(std::string_view("diagnostics")))
        {
            return true;
        }
        auto diagnostics =
            (*root_obj)[std::string_view("diagnostics")].as_array();
        return !diagnostics.has_value() || diagnostics->empty();
    }

    std::string WrapAuthoringChange(
        std::string_view kind,
        std::string_view payload)
    {
        return std::string(R"({"baseRevision":0,"kind":")") + std::string(kind)
               + R"(","payload":)" + std::string(payload) + "}";
    }

    // 值探针节点组件：Do 输出 值=probe_value 的 int 口（子图产出端的本
    // 地替身；正值/负值两形态共用）。
    class ProbeValueComponent final
        : public Das::PluginInterface::IDasTaskComponent
    {
    public:
        explicit ProbeValueComponent(int64_t probe_value)
            : probe_value_(probe_value)
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
            *p_out_guid = GuidOfText(PROBE_COMPONENT_GUID_TEXT);
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            return CreateIDasReadOnlyStringFromUtf8(
                "ProbeValueComponent",
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
            DasPtr<Das::ExportInterface::IDasPortMap> output_map;
            const DasResult hr = CreateIDasPortMap(output_map.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            const DasReadOnlyString value_key{"值"};
            output_map->SetInt(value_key.Get(), probe_value_);
            *pp_out_port_map = output_map.Get();
            output_map.Get()->AddRef();
            return DAS_S_OK;
        }

    private:
        int64_t               probe_value_;
        std::atomic<uint32_t> ref_count_{1};
    };

    // begin/end GUID 经真工厂实例化（manifest 双组件分派是生产身份源）；
    // 探针 GUID 走本地替身。
    class BoundaryDispatchHost final
        : public Das::PluginInterface::IDasTaskComponentHost
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
            if (component_guid == GuidOfText(BEGIN_COMPONENT_GUID_TEXT)
                || component_guid == GuidOfText(END_COMPONENT_GUID_TEXT))
            {
                DasGraphTaskComponentFactory factory{this};
                return factory.CreateComponent(
                    component_guid,
                    pp_out_component);
            }
            if (!(component_guid == GuidOfText(PROBE_COMPONENT_GUID_TEXT)))
            {
                if (component_guid
                    == GuidOfText(NEGATIVE_PROBE_COMPONENT_GUID_TEXT))
                {
                    auto* component = new ProbeValueComponent(int64_t{-7});
                    component->AddRef();
                    *pp_out_component = component;
                    return DAS_S_OK;
                }
                return DAS_E_NOT_FOUND;
            }
            auto* component = new ProbeValueComponent(int64_t{42});
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }

    private:
        std::atomic<uint32_t> ref_count_{1};
    };
} // namespace

// 空 context 新建 graph 会话 → 预置 begin+end 恰两节点；加探针、连线
// 探针.值→end.值、编译零 fatal，喂 Do 后返回码 == 子图探针产出 42。
TEST(DasGraphTaskTest, PreservedBeginEndGraphExecutesAndReturnsEndValue)
{
    GraphAuthoringSessionState* state = nullptr;
    ASSERT_EQ(CreateGraphAuthoringSession(nullptr, &state), DAS_S_OK);
    ASSERT_NE(state, nullptr);

    // 预置断言：恰两节点 n_0=begin / n_1=end，均为 componentRef。
    {
        DasPtr<Das::ExportInterface::IDasJson> document_json;
        ASSERT_EQ(
            GraphAuthoringSessionGetDocument(
                state,
                nullptr,
                document_json.Put()),
            DAS_S_OK);
        const auto preset_nodes =
            GraphNodesOfResponseText(JsonTextOf(document_json.Get()));
        ASSERT_EQ(preset_nodes.size(), 2u);
        EXPECT_EQ(preset_nodes[0].node_id, "n_0");
        EXPECT_EQ(preset_nodes[0].component_guid, BEGIN_COMPONENT_GUID_TEXT);
        EXPECT_EQ(preset_nodes[0].target_kind, "componentRef");
        EXPECT_EQ(preset_nodes[1].node_id, "n_1");
        EXPECT_EQ(preset_nodes[1].component_guid, END_COMPONENT_GUID_TEXT);
        EXPECT_EQ(preset_nodes[1].target_kind, "componentRef");
    }

    // catalog：begin（无口面）/end（值 int 入出对）/探针（值 int 出）。
    const std::string action_catalog =
        std::string(R"([{"actionId":")")
        + std::string(BEGIN_COMPONENT_GUID_TEXT) + R"("},)" + R"({"actionId":")"
        + std::string(END_COMPONENT_GUID_TEXT)
        + R"(","ports":[{"portId":"值","portType":"int","direction":"input"},{"portId":"值","portType":"int","direction":"output"}]},)"
        + R"({"actionId":")" + std::string(PROBE_COMPONENT_GUID_TEXT)
        + R"(","ports":[{"portId":"值","portType":"int","direction":"output"}]}])";
    {
        DasPtr<IDasReadOnlyString> catalog_string;
        ASSERT_EQ(
            CreateIDasReadOnlyStringFromUtf8(
                action_catalog.c_str(),
                catalog_string.Put()),
            DAS_S_OK);
        ASSERT_EQ(
            GraphAuthoringSessionSetActionCatalog(state, catalog_string.Get()),
            DAS_S_OK);
    }

    // 探针节点：预置占用 n_0/n_1 后计数器顺延到 n_2。
    std::string probe_node_id;
    {
        DasPtr<Das::ExportInterface::IDasJson> apply_result;
        const std::string                      add_probe = WrapAuthoringChange(
            "addGraphNode",
            std::string(R"({"actionId":")")
                + std::string(PROBE_COMPONENT_GUID_TEXT) + R"("})");
        DasPtr<IDasReadOnlyString> add_probe_string;
        CreateIDasReadOnlyStringFromUtf8(
            add_probe.c_str(),
            add_probe_string.Put());
        ASSERT_EQ(
            GraphAuthoringSessionApplyChange(
                state,
                add_probe_string.Get(),
                apply_result.Put()),
            DAS_S_OK);
        const auto apply_text = JsonTextOf(apply_result.Get());
        ASSERT_TRUE(ResponseDiagnosticsEmpty(apply_text));
        const auto nodes = GraphNodesOfResponseText(apply_text);
        ASSERT_EQ(nodes.size(), 3u);
        for (const auto& node : nodes)
        {
            if (node.component_guid == PROBE_COMPONENT_GUID_TEXT)
            {
                probe_node_id = node.node_id;
            }
        }
        ASSERT_EQ(probe_node_id, "n_2");
    }

    // 连线：探针.值 → end.值（data 边，int→int）。
    {
        DasPtr<Das::ExportInterface::IDasJson> apply_result;
        const std::string connect_probe = WrapAuthoringChange(
            "connectGraphPorts",
            std::string(R"({"fromNodeId":")") + probe_node_id
                + R"(","fromPortId":"值","toNodeId":"n_1","toPortId":"值"})");
        DasPtr<IDasReadOnlyString> connect_probe_string;
        CreateIDasReadOnlyStringFromUtf8(
            connect_probe.c_str(),
            connect_probe_string.Put());
        ASSERT_EQ(
            GraphAuthoringSessionApplyChange(
                state,
                connect_probe_string.Get(),
                apply_result.Put()),
            DAS_S_OK);
        ASSERT_TRUE(ResponseDiagnosticsEmpty(JsonTextOf(apply_result.Get())));
    }

    // 编译：零诊断（无 fatal），取 executionInput shell 文本。
    std::string shell_text;
    {
        DasPtr<Das::ExportInterface::IDasJson> compiled;
        ASSERT_EQ(
            GraphAuthoringSessionCompile(state, nullptr, compiled.Put()),
            DAS_S_OK);
        const auto compile_text = JsonTextOf(compiled.Get());
        ASSERT_TRUE(ResponseDiagnosticsEmpty(compile_text));
        auto root = yyjson::read(compile_text);
        auto root_obj = root.as_object();
        ASSERT_TRUE(root_obj.has_value());
        auto compile_section =
            (*root_obj)[std::string_view("compile")].as_object();
        ASSERT_TRUE(compile_section.has_value());
        const auto shell = Das::Utils::SerializeYyjsonValue(
            (*compile_section)[std::string_view("executionInput")]);
        ASSERT_TRUE(shell.has_value());
        shell_text = *shell;
    }
    ASSERT_FALSE(shell_text.empty());
    DestroyGraphAuthoringSession(state);

    // 执行：begin/end 经真工厂、探针本地替身；Do 返回码 == 探针产出 42。
    auto host = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(
        new BoundaryDispatchHost());
    auto task = DasGraphTaskImpl::Make(host.Get());
    ASSERT_NE(task.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    {
        const DasReadOnlyString execution_input_key{"executionInput"};
        const DasReadOnlyString execution_input_value{shell_text.c_str()};
        ASSERT_EQ(
            input_map->SetString(
                execution_input_key.Get(),
                execution_input_value.Get()),
            DAS_S_OK);
    }

    DasPtr<Das::ExportInterface::IDasPortMap> output;
    const DasResult hr = task->Do(nullptr, input_map.Get(), output.Put());
    EXPECT_EQ(hr, static_cast<DasResult>(42));

    // 返回码语义断言零回归（hr==42）；result 串口断言随 D-21① 单通道
    // 换轨反转为缺席——边界值唯一出口=类型化 outputs。
    ASSERT_NE(output.Get(), nullptr);
    bool                    has_result_port = true;
    const DasReadOnlyString result_key{"result"};
    ASSERT_EQ(output->Has(result_key.Get(), &has_result_port), DAS_S_OK);
    EXPECT_FALSE(has_result_port);

    // 「值」以类型化条目直写（end 的出参面悬空口全量导出）。
    int64_t                 exported_value = 0;
    const DasReadOnlyString value_key{"值"};
    ASSERT_EQ(output->GetInt(value_key.Get(), &exported_value), DAS_S_OK);
    EXPECT_EQ(exported_value, 42);
}

// 图输入回声防护：begin/end 首口同为 p_1（节点内 id 计数器），end 的悬空
// 口不得被推导为图级入参——否则调用方传 p_1 会经广播绑定写进 end 转写输
// 出，原值回声进出参面（图什么都没做）；end 静态「值」同理，任务返回码
// 会被图输入直接注入。
TEST(DasGraphTaskTest, TypedGraphInputDoesNotEchoThroughUnconnectedEndPorts)
{
    GraphAuthoringSessionState* state = nullptr;
    ASSERT_EQ(CreateGraphAuthoringSession(nullptr, &state), DAS_S_OK);
    ASSERT_NE(state, nullptr);

    const std::string action_catalog =
        std::string(R"([{"actionId":")")
        + std::string(BEGIN_COMPONENT_GUID_TEXT) + R"("},)" + R"({"actionId":")"
        + std::string(END_COMPONENT_GUID_TEXT)
        + R"(","ports":[{"portId":"值","portType":"int","direction":"input"},{"portId":"值","portType":"int","direction":"output"}]}])";
    {
        DasPtr<IDasReadOnlyString> catalog_string;
        ASSERT_EQ(
            CreateIDasReadOnlyStringFromUtf8(
                action_catalog.c_str(),
                catalog_string.Put()),
            DAS_S_OK);
        ASSERT_EQ(
            GraphAuthoringSessionSetActionCatalog(state, catalog_string.Get()),
            DAS_S_OK);
    }

    // begin 长入参口「阈值」/end 长出参口「评分」——两节点首口同 id p_1，
    // 全部悬空（零连线）。
    {
        DasPtr<Das::ExportInterface::IDasJson> apply_result;
        const std::string grow_begin_port = WrapAuthoringChange(
            "addNodePort",
            R"({"nodeId":"n_0","name":"阈值","direction":"input","valueType":"int"})");
        DasPtr<IDasReadOnlyString> grow_begin_port_string;
        CreateIDasReadOnlyStringFromUtf8(
            grow_begin_port.c_str(),
            grow_begin_port_string.Put());
        ASSERT_EQ(
            GraphAuthoringSessionApplyChange(
                state,
                grow_begin_port_string.Get(),
                apply_result.Put()),
            DAS_S_OK);
        ASSERT_TRUE(ResponseDiagnosticsEmpty(JsonTextOf(apply_result.Get())));
    }
    {
        DasPtr<Das::ExportInterface::IDasJson> apply_result;
        const std::string grow_end_port = WrapAuthoringChange(
            "addNodePort",
            R"({"nodeId":"n_1","name":"评分","direction":"output","valueType":"int"})");
        DasPtr<IDasReadOnlyString> grow_end_port_string;
        CreateIDasReadOnlyStringFromUtf8(
            grow_end_port.c_str(),
            grow_end_port_string.Put());
        ASSERT_EQ(
            GraphAuthoringSessionApplyChange(
                state,
                grow_end_port_string.Get(),
                apply_result.Put()),
            DAS_S_OK);
        ASSERT_TRUE(ResponseDiagnosticsEmpty(JsonTextOf(apply_result.Get())));
    }

    std::string shell_text;
    {
        DasPtr<Das::ExportInterface::IDasJson> compiled;
        ASSERT_EQ(
            GraphAuthoringSessionCompile(state, nullptr, compiled.Put()),
            DAS_S_OK);
        ASSERT_TRUE(ResponseDiagnosticsEmpty(JsonTextOf(compiled.Get())));
        auto root = yyjson::read(JsonTextOf(compiled.Get()));
        auto root_obj = root.as_object();
        ASSERT_TRUE(root_obj.has_value());
        auto compile_section =
            (*root_obj)[std::string_view("compile")].as_object();
        ASSERT_TRUE(compile_section.has_value());
        const auto shell = Das::Utils::SerializeYyjsonValue(
            (*compile_section)[std::string_view("executionInput")]);
        ASSERT_TRUE(shell.has_value());
        shell_text = *shell;
    }
    ASSERT_FALSE(shell_text.empty());
    DestroyGraphAuthoringSession(state);

    // 执行：graph_inputs 携带 p_1=42 与 值=5（调用方图输入 + 返回码注入
    // 探针）——end 悬空口不得回声其中任何一个。
    auto host = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(
        new BoundaryDispatchHost());
    auto task = DasGraphTaskImpl::Make(host.Get());
    ASSERT_NE(task.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    {
        const DasReadOnlyString execution_input_key{"executionInput"};
        const DasReadOnlyString execution_input_value{shell_text.c_str()};
        ASSERT_EQ(
            input_map->SetString(
                execution_input_key.Get(),
                execution_input_value.Get()),
            DAS_S_OK);
        const DasReadOnlyString p1_key{"p_1"};
        ASSERT_EQ(input_map->SetInt(p1_key.Get(), int64_t{42}), DAS_S_OK);
        const DasReadOnlyString value_key{"值"};
        ASSERT_EQ(input_map->SetInt(value_key.Get(), int64_t{5}), DAS_S_OK);
    }

    DasPtr<Das::ExportInterface::IDasPortMap> output;
    const DasResult hr = task->Do(nullptr, input_map.Get(), output.Put());
    EXPECT_EQ(hr, DAS_S_OK);

    // 无回声：outputs 不携带 p_1（end 悬空口未收图输入，帧上无值）。
    ASSERT_NE(output.Get(), nullptr);
    bool                    has_echo_port = true;
    const DasReadOnlyString p1_key{"p_1"};
    ASSERT_EQ(output->Has(p1_key.Get(), &has_echo_port), DAS_S_OK);
    EXPECT_FALSE(has_echo_port);
}

// formSequence 上下文（LINEAR 文档）不预置 begin/end。
TEST(DasGraphTaskTest, FormSequenceContextDoesNotPresetBoundaryNodes)
{
    constexpr std::string_view context = R"({
        "items": [
            {
                "itemId": "a",
                "target": {"targetKind": "componentRef", "componentRef": {"kind": "componentRef", "componentGuid": "{ga}", "pluginGuid": "{p}"}},
                "settings": {}
            },
            {
                "itemId": "b",
                "target": {"targetKind": "componentRef", "componentRef": {"kind": "componentRef", "componentGuid": "{gb}", "pluginGuid": "{p}"}},
                "settings": {}
            }
        ]
    })";

    GraphAuthoringSessionState* state = nullptr;
    DasPtr<IDasReadOnlyString>  context_string;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8(
            std::string(context).c_str(),
            context_string.Put()),
        DAS_S_OK);
    ASSERT_EQ(
        CreateGraphAuthoringSession(context_string.Get(), &state),
        DAS_S_OK);
    ASSERT_NE(state, nullptr);

    DasPtr<Das::ExportInterface::IDasJson> document_json;
    ASSERT_EQ(
        GraphAuthoringSessionGetDocument(state, nullptr, document_json.Put()),
        DAS_S_OK);
    const auto document_text = CompactText(JsonTextOf(document_json.Get()));
    ASSERT_NE(
        document_text.find("\"viewKind\":\"formSequence\""),
        std::string::npos);
    EXPECT_EQ(document_text.find(BEGIN_COMPONENT_GUID_TEXT), std::string::npos);
    EXPECT_EQ(document_text.find(END_COMPONENT_GUID_TEXT), std::string::npos);
    // formSequence 视图无 graph 节点段——预置若误入会以 graph 节点暴露。
    const auto nodes = GraphNodesOfResponseText(document_text);
    EXPECT_TRUE(nodes.empty());
    DestroyGraphAuthoringSession(state);
}

// 非空 graph 上下文不追加 begin/end（预置仅限新建空图）。
TEST(DasGraphTaskTest, NonEmptyGraphContextDoesNotAppendBoundaryNodes)
{
    constexpr std::string_view context = R"({
        "nodes": [
            {
                "nodeId": "x1",
                "target": {"targetKind": "componentRef", "componentRef": {"kind": "componentRef", "componentGuid": "{g9}", "pluginGuid": "{p}"}},
                "settings": {}
            }
        ],
        "edges": []
    })";

    GraphAuthoringSessionState* state = nullptr;
    DasPtr<IDasReadOnlyString>  context_string;
    ASSERT_EQ(
        CreateIDasReadOnlyStringFromUtf8(
            std::string(context).c_str(),
            context_string.Put()),
        DAS_S_OK);
    ASSERT_EQ(
        CreateGraphAuthoringSession(context_string.Get(), &state),
        DAS_S_OK);
    ASSERT_NE(state, nullptr);

    DasPtr<Das::ExportInterface::IDasJson> document_json;
    ASSERT_EQ(
        GraphAuthoringSessionGetDocument(state, nullptr, document_json.Put()),
        DAS_S_OK);
    const auto nodes =
        GraphNodesOfResponseText(JsonTextOf(document_json.Get()));
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].node_id, "x1");
    EXPECT_EQ(nodes[0].component_guid, "{g9}");
    DestroyGraphAuthoringSession(state);
}

// begin 组件经真工厂实例化：GetGuid 按 kind 分派 begin GUID；Do 把输入
// map 的四类型同名键逐项转写进输出 map（类型与值逐项相等）。
TEST(DasGraphTaskTest, BoundaryComponentTransposesTypedPorts)
{
    auto host = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(
        new MockTaskComponentHost());
    DasGraphTaskComponentFactory factory{host.Get()};

    DasPtr<Das::PluginInterface::IDasTaskComponent> begin_component;
    ASSERT_EQ(
        factory.CreateComponent(
            GuidOfText(BEGIN_COMPONENT_GUID_TEXT),
            begin_component.Put()),
        DAS_S_OK);
    ASSERT_NE(begin_component.Get(), nullptr);

    DasGuid component_guid{};
    ASSERT_EQ(begin_component->GetGuid(&component_guid), DAS_S_OK);
    EXPECT_TRUE(component_guid == GuidOfText(BEGIN_COMPONENT_GUID_TEXT));

    DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    {
        const DasReadOnlyString int_key{"a_int"};
        const DasReadOnlyString float_key{"a_float"};
        const DasReadOnlyString bool_key{"a_bool"};
        const DasReadOnlyString string_key{"a_str"};
        ASSERT_EQ(input_map->SetInt(int_key.Get(), int64_t{7}), DAS_S_OK);
        ASSERT_EQ(input_map->SetFloat(float_key.Get(), 1.5), DAS_S_OK);
        ASSERT_EQ(input_map->SetBool(bool_key.Get(), true), DAS_S_OK);
        DasPtr<IDasReadOnlyString> string_value;
        CreateIDasReadOnlyStringFromUtf8("hello", string_value.Put());
        ASSERT_EQ(
            input_map->SetString(string_key.Get(), string_value.Get()),
            DAS_S_OK);
    }

    DasPtr<Das::ExportInterface::IDasPortMap> output_map;
    ASSERT_EQ(
        begin_component->Do(nullptr, input_map.Get(), output_map.Put()),
        DAS_S_OK);
    ASSERT_NE(output_map.Get(), nullptr);

    const DasReadOnlyString int_key{"a_int"};
    const DasReadOnlyString float_key{"a_float"};
    const DasReadOnlyString bool_key{"a_bool"};
    const DasReadOnlyString string_key{"a_str"};

    Das::ExportInterface::DasVariantType value_type =
        Das::ExportInterface::DAS_VARIANT_TYPE_NULL;
    ASSERT_EQ(output_map->GetType(int_key.Get(), &value_type), DAS_S_OK);
    EXPECT_EQ(value_type, Das::ExportInterface::DAS_VARIANT_TYPE_INT);
    ASSERT_EQ(output_map->GetType(float_key.Get(), &value_type), DAS_S_OK);
    EXPECT_EQ(value_type, Das::ExportInterface::DAS_VARIANT_TYPE_FLOAT);
    ASSERT_EQ(output_map->GetType(bool_key.Get(), &value_type), DAS_S_OK);
    EXPECT_EQ(value_type, Das::ExportInterface::DAS_VARIANT_TYPE_BOOL);
    ASSERT_EQ(output_map->GetType(string_key.Get(), &value_type), DAS_S_OK);
    EXPECT_EQ(value_type, Das::ExportInterface::DAS_VARIANT_TYPE_STRING);

    int64_t out_int = 0;
    ASSERT_EQ(output_map->GetInt(int_key.Get(), &out_int), DAS_S_OK);
    EXPECT_EQ(out_int, 7);
    double out_float = 0.0;
    ASSERT_EQ(output_map->GetFloat(float_key.Get(), &out_float), DAS_S_OK);
    EXPECT_DOUBLE_EQ(out_float, 1.5);
    bool out_bool = false;
    ASSERT_EQ(output_map->GetBool(bool_key.Get(), &out_bool), DAS_S_OK);
    EXPECT_TRUE(out_bool);
    DasPtr<IDasReadOnlyString> out_string;
    ASSERT_EQ(
        output_map->GetString(string_key.Get(), out_string.Put()),
        DAS_S_OK);
    const char* out_string_utf8 = nullptr;
    ASSERT_EQ(out_string->GetUtf8(&out_string_utf8), DAS_S_OK);
    ASSERT_NE(out_string_utf8, nullptr);
    EXPECT_STREQ(out_string_utf8, "hello");
}

// end 组件经真工厂实例化：GetGuid 分派 end GUID；空输入 map → Do 成功且
// 输出 map 为空。
TEST(DasGraphTaskTest, BoundaryComponentEmptyInputProducesEmptyOutput)
{
    auto host = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(
        new MockTaskComponentHost());
    DasGraphTaskComponentFactory factory{host.Get()};

    DasPtr<Das::PluginInterface::IDasTaskComponent> end_component;
    ASSERT_EQ(
        factory.CreateComponent(
            GuidOfText(END_COMPONENT_GUID_TEXT),
            end_component.Put()),
        DAS_S_OK);
    ASSERT_NE(end_component.Get(), nullptr);

    DasGuid component_guid{};
    ASSERT_EQ(end_component->GetGuid(&component_guid), DAS_S_OK);
    EXPECT_TRUE(component_guid == GuidOfText(END_COMPONENT_GUID_TEXT));

    DasPtr<Das::ExportInterface::IDasPortMap> output_map;
    ASSERT_EQ(end_component->Do(nullptr, nullptr, output_map.Put()), DAS_S_OK);
    ASSERT_NE(output_map.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasStringVector> keys;
    ASSERT_EQ(output_map->GetKeys(keys.Put()), DAS_S_OK);
    uint64_t key_count = 0;
    ASSERT_EQ(keys->Size(&key_count), DAS_S_OK);
    EXPECT_EQ(key_count, 0u);
}

// 全枚举转写：GetKeys 逐键分派，五个混合键（含静态口面之外的未知键）
// 全部同名落到输出 map。
TEST(DasGraphTaskTest, BoundaryComponentTransposesEveryInputKey)
{
    auto host = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(
        new MockTaskComponentHost());
    DasGraphTaskComponentFactory factory{host.Get()};

    DasPtr<Das::PluginInterface::IDasTaskComponent> begin_component;
    ASSERT_EQ(
        factory.CreateComponent(
            GuidOfText(BEGIN_COMPONENT_GUID_TEXT),
            begin_component.Put()),
        DAS_S_OK);

    DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    {
        const DasReadOnlyString key_one{"k_one"};
        const DasReadOnlyString key_two{"k_two"};
        const DasReadOnlyString key_three{"k_three"};
        const DasReadOnlyString key_four{"k_four"};
        const DasReadOnlyString key_five{"k_five"};
        ASSERT_EQ(input_map->SetInt(key_one.Get(), int64_t{1}), DAS_S_OK);
        ASSERT_EQ(input_map->SetInt(key_two.Get(), int64_t{2}), DAS_S_OK);
        ASSERT_EQ(input_map->SetFloat(key_three.Get(), 3.5), DAS_S_OK);
        ASSERT_EQ(input_map->SetBool(key_four.Get(), false), DAS_S_OK);
        DasPtr<IDasReadOnlyString> string_value;
        CreateIDasReadOnlyStringFromUtf8("five", string_value.Put());
        ASSERT_EQ(
            input_map->SetString(key_five.Get(), string_value.Get()),
            DAS_S_OK);
    }

    DasPtr<Das::ExportInterface::IDasPortMap> output_map;
    ASSERT_EQ(
        begin_component->Do(nullptr, input_map.Get(), output_map.Put()),
        DAS_S_OK);

    DasPtr<Das::ExportInterface::IDasStringVector> keys;
    ASSERT_EQ(output_map->GetKeys(keys.Put()), DAS_S_OK);
    uint64_t key_count = 0;
    ASSERT_EQ(keys->Size(&key_count), DAS_S_OK);
    EXPECT_EQ(key_count, 5u);

    const DasReadOnlyString key_one{"k_one"};
    const DasReadOnlyString key_five{"k_five"};
    int64_t                 out_int = 0;
    ASSERT_EQ(output_map->GetInt(key_one.Get(), &out_int), DAS_S_OK);
    EXPECT_EQ(out_int, 1);
    DasPtr<IDasReadOnlyString> out_string;
    ASSERT_EQ(
        output_map->GetString(key_five.Get(), out_string.Put()),
        DAS_S_OK);
    const char* out_string_utf8 = nullptr;
    ASSERT_EQ(out_string->GetUtf8(&out_string_utf8), DAS_S_OK);
    EXPECT_STREQ(out_string_utf8, "five");
}

// 返回码负值形态：子图探针产出 -7 → Do 返回码原样透传 -7（负值是「任
// 务失败」的用户表达通道，不得夹 0）。
TEST(DasGraphTaskTest, EndValuePassthroughCarriesNegativeReturnCode)
{
    GraphAuthoringSessionState* state = nullptr;
    ASSERT_EQ(CreateGraphAuthoringSession(nullptr, &state), DAS_S_OK);

    const std::string action_catalog =
        std::string(R"([{"actionId":")")
        + std::string(BEGIN_COMPONENT_GUID_TEXT) + R"("},)" + R"({"actionId":")"
        + std::string(END_COMPONENT_GUID_TEXT)
        + R"(","ports":[{"portId":"值","portType":"int","direction":"input"},{"portId":"值","portType":"int","direction":"output"}]},)"
        + R"({"actionId":")" + std::string(NEGATIVE_PROBE_COMPONENT_GUID_TEXT)
        + R"(","ports":[{"portId":"值","portType":"int","direction":"output"}]}])";
    {
        DasPtr<IDasReadOnlyString> catalog_string;
        ASSERT_EQ(
            CreateIDasReadOnlyStringFromUtf8(
                action_catalog.c_str(),
                catalog_string.Put()),
            DAS_S_OK);
        ASSERT_EQ(
            GraphAuthoringSessionSetActionCatalog(state, catalog_string.Get()),
            DAS_S_OK);
    }

    std::string probe_node_id;
    {
        DasPtr<Das::ExportInterface::IDasJson> apply_result;
        const std::string                      add_probe = WrapAuthoringChange(
            "addGraphNode",
            std::string(R"({"actionId":")")
                + std::string(NEGATIVE_PROBE_COMPONENT_GUID_TEXT) + R"("})");
        DasPtr<IDasReadOnlyString> add_probe_string;
        CreateIDasReadOnlyStringFromUtf8(
            add_probe.c_str(),
            add_probe_string.Put());
        ASSERT_EQ(
            GraphAuthoringSessionApplyChange(
                state,
                add_probe_string.Get(),
                apply_result.Put()),
            DAS_S_OK);
        const auto nodes =
            GraphNodesOfResponseText(JsonTextOf(apply_result.Get()));
        ASSERT_EQ(nodes.size(), 3u);
        for (const auto& node : nodes)
        {
            if (node.component_guid == NEGATIVE_PROBE_COMPONENT_GUID_TEXT)
            {
                probe_node_id = node.node_id;
            }
        }
        ASSERT_EQ(probe_node_id, "n_2");
    }

    {
        DasPtr<Das::ExportInterface::IDasJson> apply_result;
        const std::string connect_probe = WrapAuthoringChange(
            "connectGraphPorts",
            std::string(R"({"fromNodeId":")") + probe_node_id
                + R"(","fromPortId":"值","toNodeId":"n_1","toPortId":"值"})");
        DasPtr<IDasReadOnlyString> connect_probe_string;
        CreateIDasReadOnlyStringFromUtf8(
            connect_probe.c_str(),
            connect_probe_string.Put());
        ASSERT_EQ(
            GraphAuthoringSessionApplyChange(
                state,
                connect_probe_string.Get(),
                apply_result.Put()),
            DAS_S_OK);
    }

    std::string shell_text;
    {
        DasPtr<Das::ExportInterface::IDasJson> compiled;
        ASSERT_EQ(
            GraphAuthoringSessionCompile(state, nullptr, compiled.Put()),
            DAS_S_OK);
        const auto compile_text = JsonTextOf(compiled.Get());
        ASSERT_TRUE(ResponseDiagnosticsEmpty(compile_text));
        auto root = yyjson::read(compile_text);
        auto root_obj = root.as_object();
        ASSERT_TRUE(root_obj.has_value());
        auto compile_section =
            (*root_obj)[std::string_view("compile")].as_object();
        ASSERT_TRUE(compile_section.has_value());
        const auto shell = Das::Utils::SerializeYyjsonValue(
            (*compile_section)[std::string_view("executionInput")]);
        ASSERT_TRUE(shell.has_value());
        shell_text = *shell;
    }
    DestroyGraphAuthoringSession(state);

    auto host = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(
        new BoundaryDispatchHost());
    auto task = DasGraphTaskImpl::Make(host.Get());
    ASSERT_NE(task.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    const DasReadOnlyString execution_input_key{"executionInput"};
    const DasReadOnlyString execution_input_value{shell_text.c_str()};
    ASSERT_EQ(
        input_map->SetString(
            execution_input_key.Get(),
            execution_input_value.Get()),
        DAS_S_OK);

    DasPtr<Das::ExportInterface::IDasPortMap> output;
    const DasResult hr = task->Do(nullptr, input_map.Get(), output.Put());
    EXPECT_EQ(hr, static_cast<DasResult>(-7));
}

// 返回码缺值形态：end 节点在场但 值 口无帧值（无上游连线）→ Do 返回 0
// （宽容语义；WARN 记录缺口，不引入新失败面）。
TEST(DasGraphTaskTest, EndNodeWithoutValuePortYieldsZeroReturnCode)
{
    GraphAuthoringSessionState* state = nullptr;
    ASSERT_EQ(CreateGraphAuthoringSession(nullptr, &state), DAS_S_OK);

    const std::string action_catalog =
        std::string(R"([{"actionId":")")
        + std::string(BEGIN_COMPONENT_GUID_TEXT) + R"("},)" + R"({"actionId":")"
        + std::string(END_COMPONENT_GUID_TEXT)
        + R"(","ports":[{"portId":"值","portType":"int","direction":"input"},{"portId":"值","portType":"int","direction":"output"}]}])";
    {
        DasPtr<IDasReadOnlyString> catalog_string;
        ASSERT_EQ(
            CreateIDasReadOnlyStringFromUtf8(
                action_catalog.c_str(),
                catalog_string.Put()),
            DAS_S_OK);
        ASSERT_EQ(
            GraphAuthoringSessionSetActionCatalog(state, catalog_string.Get()),
            DAS_S_OK);
    }

    std::string shell_text;
    {
        DasPtr<Das::ExportInterface::IDasJson> compiled;
        ASSERT_EQ(
            GraphAuthoringSessionCompile(state, nullptr, compiled.Put()),
            DAS_S_OK);
        const auto compile_text = JsonTextOf(compiled.Get());
        ASSERT_TRUE(ResponseDiagnosticsEmpty(compile_text));
        auto root = yyjson::read(compile_text);
        auto root_obj = root.as_object();
        ASSERT_TRUE(root_obj.has_value());
        auto compile_section =
            (*root_obj)[std::string_view("compile")].as_object();
        ASSERT_TRUE(compile_section.has_value());
        const auto shell = Das::Utils::SerializeYyjsonValue(
            (*compile_section)[std::string_view("executionInput")]);
        ASSERT_TRUE(shell.has_value());
        shell_text = *shell;
    }
    DestroyGraphAuthoringSession(state);

    auto host = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(
        new BoundaryDispatchHost());
    auto task = DasGraphTaskImpl::Make(host.Get());
    ASSERT_NE(task.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    const DasReadOnlyString execution_input_key{"executionInput"};
    const DasReadOnlyString execution_input_value{shell_text.c_str()};
    ASSERT_EQ(
        input_map->SetString(
            execution_input_key.Get(),
            execution_input_value.Get()),
        DAS_S_OK);

    DasPtr<Das::ExportInterface::IDasPortMap> output;
    const DasResult hr = task->Do(nullptr, input_map.Get(), output.Put());
    EXPECT_EQ(hr, DAS_S_OK);
}

// ===========================================================================
// 跨子图类型化直传 E2E（D-18/D-21 单通道）：int 直传经覆盖层到达子图组
// 件并沿返回码链回传；边界输出按口名类型化直写 output；result 串口退役。
// ===========================================================================
namespace
{
    // document 投影 → 指定组件节点的首个动态口 portId（动态口经
    // Get/Apply 响应的 wire 键形 {portId, dynamic} 呈现）。
    std::string FirstDynamicPortIdOfComponent(
        const std::string& response_text,
        std::string_view   component_guid)
    {
        auto root = yyjson::read(response_text);
        auto root_obj = root.as_object();
        if (!root_obj || !root_obj->contains(std::string_view("document")))
        {
            return {};
        }
        auto document_obj =
            (*root_obj)[std::string_view("document")].as_object();
        if (!document_obj || !document_obj->contains(std::string_view("graph")))
        {
            return {};
        }
        auto graph_obj = (*document_obj)[std::string_view("graph")].as_object();
        if (!graph_obj || !graph_obj->contains(std::string_view("nodes")))
        {
            return {};
        }
        auto nodes_arr = (*graph_obj)[std::string_view("nodes")].as_array();
        if (!nodes_arr.has_value())
        {
            return {};
        }
        for (const auto& node_value : *nodes_arr)
        {
            auto node_obj = node_value.as_object();
            if (!node_obj.has_value())
            {
                continue;
            }
            const auto node_component =
                (*node_obj)[std::string_view("componentGuid")].as_string();
            if (!node_component.has_value()
                || std::string_view(*node_component) != component_guid)
            {
                continue;
            }
            auto ports_arr = (*node_obj)[std::string_view("ports")].as_array();
            if (!ports_arr.has_value())
            {
                continue;
            }
            for (const auto& port_value : *ports_arr)
            {
                auto port_obj = port_value.as_object();
                if (!port_obj.has_value())
                {
                    continue;
                }
                const auto is_dynamic =
                    (*port_obj)[std::string_view("dynamic")].as_bool();
                if (!is_dynamic.has_value() || !*is_dynamic)
                {
                    continue;
                }
                const auto port_id =
                    (*port_obj)[std::string_view("portId")].as_string();
                if (port_id.has_value())
                {
                    return std::string(*port_id);
                }
            }
        }
        return {};
    }

    // 计数型 fake IDasImage（DoAdapterTest 同款形态）：跨子图引用身份断
    // 言的对象锚点。
    class CountingFakeImage final
        : public Das::ExportInterface::DasImageImplBase<CountingFakeImage>
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

    constexpr std::string_view IMAGE_ECHO_COMPONENT_GUID_TEXT =
        "99999999-9999-9999-9999-999999999999";

    // image 直通替身：in→out 引用透传（子图内的消费/产出端，身份不断言
    // 只搬运——断言在父侧 output 上做）。
    class ImageEchoComponent final
        : public Das::PluginInterface::IDasTaskComponent
    {
    public:
        std::atomic<uint32_t> ref_count_{1};

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
            *p_out_guid = GuidOfText(IMAGE_ECHO_COMPONENT_GUID_TEXT);
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            return CreateIDasReadOnlyStringFromUtf8(
                "ImageEchoComponent",
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
           Das::ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
           Das::ExportInterface::IDasPortMap**        pp_out_port_map) override
        {
            if (pp_out_port_map == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_port_map = nullptr;
            DasPtr<Das::ExportInterface::IDasPortMap> output_map;
            DasResult hr = CreateIDasPortMap(output_map.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            if (p_input_port_map != nullptr)
            {
                DAS::DasPtr<Das::ExportInterface::IDasImage> image;
                const DasReadOnlyString                      in_key{"in"};
                if (DAS::IsOk(
                        p_input_port_map->GetImage(in_key.Get(), image.Put())))
                {
                    const DasReadOnlyString out_key{"out"};
                    hr = output_map->SetImage(out_key.Get(), image.Get());
                    if (DAS::IsFailed(hr))
                    {
                        return hr;
                    }
                }
            }
            *pp_out_port_map = output_map.Get();
            output_map.Get()->AddRef();
            return DAS_S_OK;
        }
    };

    // begin/end 经真工厂实例化 + image 直通替身分派。
    class TypedDirectTransferHost final
        : public Das::PluginInterface::IDasTaskComponentHost
    {
    public:
        std::atomic<uint32_t> ref_count_{1};

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
            if (component_guid == GuidOfText(BEGIN_COMPONENT_GUID_TEXT)
                || component_guid == GuidOfText(END_COMPONENT_GUID_TEXT))
            {
                DasGraphTaskComponentFactory factory{this};
                return factory.CreateComponent(
                    component_guid,
                    pp_out_component);
            }
            if (component_guid == GuidOfText(IMAGE_ECHO_COMPONENT_GUID_TEXT))
            {
                auto* component = new ImageEchoComponent();
                component->AddRef();
                *pp_out_component = component;
                return DAS_S_OK;
            }
            return DAS_E_NOT_FOUND;
        }
    };
} // namespace

// begin 动态口「左值」(int) 直传：input map 的 p_1=77 经覆盖层物化到
// begin，转写 → 连线 → end.值 → 返回码 77 + 边界口「值」类型化直写
// output；result 串口退役（缺席）。
TEST(DasGraphTaskTest, TypedDirectTransferCarriesIntAndRetiresResultPort)
{
    GraphAuthoringSessionState* state = nullptr;
    ASSERT_EQ(CreateGraphAuthoringSession(nullptr, &state), DAS_S_OK);

    const std::string action_catalog =
        std::string(R"([{"actionId":")")
        + std::string(BEGIN_COMPONENT_GUID_TEXT) + R"("},)" + R"({"actionId":")"
        + std::string(END_COMPONENT_GUID_TEXT)
        + R"(","ports":[{"portId":"值","portType":"int","direction":"input"},{"portId":"值","portType":"int","direction":"output"}]}])";
    {
        DasPtr<IDasReadOnlyString> catalog_string;
        ASSERT_EQ(
            CreateIDasReadOnlyStringFromUtf8(
                action_catalog.c_str(),
                catalog_string.Put()),
            DAS_S_OK);
        ASSERT_EQ(
            GraphAuthoringSessionSetActionCatalog(state, catalog_string.Get()),
            DAS_S_OK);
    }

    std::string dynamic_port_id;
    {
        DasPtr<Das::ExportInterface::IDasJson> apply_result;
        const std::string                      add_port = WrapAuthoringChange(
            "addNodePort",
            R"({"nodeId":"n_0","name":"左值","direction":"input","valueType":"int"})");
        DasPtr<IDasReadOnlyString> add_port_string;
        CreateIDasReadOnlyStringFromUtf8(
            add_port.c_str(),
            add_port_string.Put());
        ASSERT_EQ(
            GraphAuthoringSessionApplyChange(
                state,
                add_port_string.Get(),
                apply_result.Put()),
            DAS_S_OK);
        const auto apply_text = JsonTextOf(apply_result.Get());
        ASSERT_TRUE(ResponseDiagnosticsEmpty(apply_text));
        dynamic_port_id = FirstDynamicPortIdOfComponent(
            apply_text,
            BEGIN_COMPONENT_GUID_TEXT);
        ASSERT_EQ(dynamic_port_id, "p_1");
    }

    {
        DasPtr<Das::ExportInterface::IDasJson> apply_result;
        const std::string connect_port = WrapAuthoringChange(
            "connectGraphPorts",
            std::string(R"({"fromNodeId":"n_0","fromPortId":")")
                + dynamic_port_id + R"(","toNodeId":"n_1","toPortId":"值"})");
        DasPtr<IDasReadOnlyString> connect_port_string;
        CreateIDasReadOnlyStringFromUtf8(
            connect_port.c_str(),
            connect_port_string.Put());
        ASSERT_EQ(
            GraphAuthoringSessionApplyChange(
                state,
                connect_port_string.Get(),
                apply_result.Put()),
            DAS_S_OK);
        ASSERT_TRUE(ResponseDiagnosticsEmpty(JsonTextOf(apply_result.Get())));
    }

    std::string shell_text;
    {
        DasPtr<Das::ExportInterface::IDasJson> compiled;
        ASSERT_EQ(
            GraphAuthoringSessionCompile(state, nullptr, compiled.Put()),
            DAS_S_OK);
        const auto compile_text = JsonTextOf(compiled.Get());
        ASSERT_TRUE(ResponseDiagnosticsEmpty(compile_text));
        auto root = yyjson::read(compile_text);
        auto root_obj = root.as_object();
        ASSERT_TRUE(root_obj.has_value());
        auto compile_section =
            (*root_obj)[std::string_view("compile")].as_object();
        ASSERT_TRUE(compile_section.has_value());
        const auto shell = Das::Utils::SerializeYyjsonValue(
            (*compile_section)[std::string_view("executionInput")]);
        ASSERT_TRUE(shell.has_value());
        shell_text = *shell;
    }
    DestroyGraphAuthoringSession(state);

    auto host = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(
        new BoundaryDispatchHost());
    auto task = DasGraphTaskImpl::Make(host.Get());
    ASSERT_NE(task.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    {
        const DasReadOnlyString execution_input_key{"executionInput"};
        const DasReadOnlyString execution_input_value{shell_text.c_str()};
        ASSERT_EQ(
            input_map->SetString(
                execution_input_key.Get(),
                execution_input_value.Get()),
            DAS_S_OK);
        // 直传条目：键=边界口稳定 id（动态口 p_1）。
        const DasReadOnlyString port_key{dynamic_port_id.c_str()};
        ASSERT_EQ(input_map->SetInt(port_key.Get(), int64_t{77}), DAS_S_OK);
    }

    DasPtr<Das::ExportInterface::IDasPortMap> output;
    const DasResult hr = task->Do(nullptr, input_map.Get(), output.Put());
    EXPECT_EQ(hr, static_cast<DasResult>(77));

    // 边界口「值」以类型化条目直写 output（end 的出参面悬空口全量导出）。
    ASSERT_NE(output.Get(), nullptr);
    int64_t                 exported_value = 0;
    const DasReadOnlyString value_key{"值"};
    ASSERT_EQ(output->GetInt(value_key.Get(), &exported_value), DAS_S_OK);
    EXPECT_EQ(exported_value, 77);

    // result 串口退役（D-21①）：边界值唯一出口=类型化 outputs。
    bool                    has_result_port = true;
    const DasReadOnlyString result_key{"result"};
    ASSERT_EQ(output->Has(result_key.Get(), &has_result_port), DAS_S_OK);
    EXPECT_FALSE(has_result_port);
}

// begin 动态口 image 直传：p_1 的 image 引用经覆盖层 → begin 转写 → 图内
// 数据边 → 直通替身 → 边界导出 → output 的「out」口——全程同一实例（跨
// 子图引用身份，JSON 桥做不到的半段）；缺 end 宽容返回 0。
TEST(DasGraphTaskTest, TypedDirectTransferCarriesImageReferenceAcrossSubgraph)
{
    GraphAuthoringSessionState* state = nullptr;
    ASSERT_EQ(CreateGraphAuthoringSession(nullptr, &state), DAS_S_OK);

    const std::string action_catalog =
        std::string(R"([{"actionId":")")
        + std::string(BEGIN_COMPONENT_GUID_TEXT) + R"("},)" + R"({"actionId":")"
        + std::string(IMAGE_ECHO_COMPONENT_GUID_TEXT)
        + R"(","ports":[{"portId":"in","portType":"image","direction":"input"},{"portId":"out","portType":"image","direction":"output"}]}])";
    {
        DasPtr<IDasReadOnlyString> catalog_string;
        ASSERT_EQ(
            CreateIDasReadOnlyStringFromUtf8(
                action_catalog.c_str(),
                catalog_string.Put()),
            DAS_S_OK);
        ASSERT_EQ(
            GraphAuthoringSessionSetActionCatalog(state, catalog_string.Get()),
            DAS_S_OK);
    }

    std::string dynamic_port_id;
    {
        DasPtr<Das::ExportInterface::IDasJson> apply_result;
        const std::string                      add_port = WrapAuthoringChange(
            "addNodePort",
            R"({"nodeId":"n_0","name":"帧","direction":"input","valueType":"image"})");
        DasPtr<IDasReadOnlyString> add_port_string;
        CreateIDasReadOnlyStringFromUtf8(
            add_port.c_str(),
            add_port_string.Put());
        ASSERT_EQ(
            GraphAuthoringSessionApplyChange(
                state,
                add_port_string.Get(),
                apply_result.Put()),
            DAS_S_OK);
        const auto apply_text = JsonTextOf(apply_result.Get());
        ASSERT_TRUE(ResponseDiagnosticsEmpty(apply_text));
        dynamic_port_id = FirstDynamicPortIdOfComponent(
            apply_text,
            BEGIN_COMPONENT_GUID_TEXT);
        ASSERT_EQ(dynamic_port_id, "p_1");
    }

    // image 直通替身节点（预置占用 n_0/n_1 后顺延 n_2；end 缺席宽容）。
    std::string echo_node_id;
    {
        DasPtr<Das::ExportInterface::IDasJson> apply_result;
        const std::string                      add_echo = WrapAuthoringChange(
            "addGraphNode",
            std::string(R"({"actionId":")")
                + std::string(IMAGE_ECHO_COMPONENT_GUID_TEXT) + R"("})");
        DasPtr<IDasReadOnlyString> add_echo_string;
        CreateIDasReadOnlyStringFromUtf8(
            add_echo.c_str(),
            add_echo_string.Put());
        ASSERT_EQ(
            GraphAuthoringSessionApplyChange(
                state,
                add_echo_string.Get(),
                apply_result.Put()),
            DAS_S_OK);
        const auto apply_text = JsonTextOf(apply_result.Get());
        ASSERT_TRUE(ResponseDiagnosticsEmpty(apply_text));
        const auto nodes = GraphNodesOfResponseText(apply_text);
        for (const auto& node : nodes)
        {
            if (node.component_guid == IMAGE_ECHO_COMPONENT_GUID_TEXT)
            {
                echo_node_id = node.node_id;
            }
        }
        ASSERT_EQ(echo_node_id, "n_2");
    }

    {
        DasPtr<Das::ExportInterface::IDasJson> apply_result;
        const std::string connect_port = WrapAuthoringChange(
            "connectGraphPorts",
            std::string(R"({"fromNodeId":"n_0","fromPortId":")")
                + dynamic_port_id + R"(","toNodeId":")" + echo_node_id
                + R"(","toPortId":"in"})");
        DasPtr<IDasReadOnlyString> connect_port_string;
        CreateIDasReadOnlyStringFromUtf8(
            connect_port.c_str(),
            connect_port_string.Put());
        ASSERT_EQ(
            GraphAuthoringSessionApplyChange(
                state,
                connect_port_string.Get(),
                apply_result.Put()),
            DAS_S_OK);
        ASSERT_TRUE(ResponseDiagnosticsEmpty(JsonTextOf(apply_result.Get())));
    }

    std::string shell_text;
    {
        DasPtr<Das::ExportInterface::IDasJson> compiled;
        ASSERT_EQ(
            GraphAuthoringSessionCompile(state, nullptr, compiled.Put()),
            DAS_S_OK);
        const auto compile_text = JsonTextOf(compiled.Get());
        ASSERT_TRUE(ResponseDiagnosticsEmpty(compile_text));
        auto root = yyjson::read(compile_text);
        auto root_obj = root.as_object();
        ASSERT_TRUE(root_obj.has_value());
        auto compile_section =
            (*root_obj)[std::string_view("compile")].as_object();
        ASSERT_TRUE(compile_section.has_value());
        const auto shell = Das::Utils::SerializeYyjsonValue(
            (*compile_section)[std::string_view("executionInput")]);
        ASSERT_TRUE(shell.has_value());
        shell_text = *shell;
    }
    DestroyGraphAuthoringSession(state);

    DAS::DasPtr<Das::ExportInterface::IDasImage> image =
        CountingFakeImage::Make();

    auto host = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(
        new TypedDirectTransferHost());
    auto task = DasGraphTaskImpl::Make(host.Get());
    ASSERT_NE(task.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    {
        const DasReadOnlyString execution_input_key{"executionInput"};
        const DasReadOnlyString execution_input_value{shell_text.c_str()};
        ASSERT_EQ(
            input_map->SetString(
                execution_input_key.Get(),
                execution_input_value.Get()),
            DAS_S_OK);
        const DasReadOnlyString port_key{dynamic_port_id.c_str()};
        ASSERT_EQ(input_map->SetImage(port_key.Get(), image.Get()), DAS_S_OK);
    }

    DasPtr<Das::ExportInterface::IDasPortMap> output;
    const DasResult hr = task->Do(nullptr, input_map.Get(), output.Put());
    EXPECT_EQ(hr, DAS_S_OK);

    // 跨子图 image 引用身份：output 的「out」口即直传的同一实例。
    ASSERT_NE(output.Get(), nullptr);
    DAS::DasPtr<Das::ExportInterface::IDasImage> exported;
    const DasReadOnlyString                      out_key{"out"};
    ASSERT_EQ(output->GetImage(out_key.Get(), exported.Put()), DAS_S_OK);
    EXPECT_EQ(exported.Get(), image.Get());

    bool                    has_result_port = true;
    const DasReadOnlyString result_key{"result"};
    ASSERT_EQ(output->Has(result_key.Get(), &has_result_port), DAS_S_OK);
    EXPECT_FALSE(has_result_port);
}

// ---- 完成事件 focus 记录（user.jsonl → UI 事件日志面板）----

namespace
{

    // 真实 recorder + 临时 session 目录（DasMaaPi AgentOnOutputTest 同款
    // 基建）：Record 落盘 user.jsonl，测试读文件断言渲染文本。
    class GraphTaskFocusLogFixture : public ::testing::Test
    {
    protected:
        DasPtr<Das::ExportInterface::IDasLogNotifier>        notifier_;
        DasPtr<Das::ExportInterface::IDasLogSession>         session_;
        DasPtr<Das::ExportInterface::IDasProcessLogRecorder> recorder_;
        DasPtr<IDasReadOnlyString>                           saved_locale_;

        void SetUp() override
        {
            ASSERT_EQ(GetDasLogNotifier(notifier_.Put()), DAS_S_OK);
            const auto stamp =
                std::chrono::steady_clock::now().time_since_epoch().count();
            const auto stamp_directory =
                std::filesystem::path{GRAPH_TASK_TEST_PATH_LITERAL("focus-")}
                    .concat(std::to_string(stamp));
            const auto u8_session_directory =
                (std::filesystem::current_path()
                 / GRAPH_TASK_TEST_PATH_LITERAL("graphtask-focus-test-output")
                 / stamp_directory)
                    .u8string();
            const std::string session_directory_utf8(
                u8_session_directory.begin(),
                u8_session_directory.end());
            DasPtr<IDasReadOnlyString> session_directory;
            ASSERT_EQ(
                CreateIDasReadOnlyStringFromUtf8(
                    session_directory_utf8.c_str(),
                    session_directory.Put()),
                DAS_S_OK);
            ASSERT_EQ(
                CreateLogSession(session_directory.Get(), session_.Put()),
                DAS_S_OK);
            ASSERT_EQ(
                CreateIDasProcessLogRecorder(
                    notifier_.Get(),
                    session_.Get(),
                    recorder_.Put()),
                DAS_S_OK);
            // GlobalLocale 是进程级状态：save/restore 防跨测试污染。
            ASSERT_EQ(DasGetDefaultLocale(saved_locale_.Put()), DAS_S_OK);
        }

        void TearDown() override
        {
            if (saved_locale_.Get() != nullptr)
            {
                (void)DasSetDefaultLocale(saved_locale_.Get());
            }
        }

        void SetDefaultLocale(const char* locale_name)
        {
            DasPtr<IDasReadOnlyString> locale;
            ASSERT_EQ(
                CreateIDasReadOnlyStringFromUtf8(locale_name, locale.Put()),
                DAS_S_OK);
            ASSERT_EQ(DasSetDefaultLocale(locale.Get()), DAS_S_OK);
        }

        // en/zh-CN 双 locale 夹具表（{0} 位置占位符）。
        std::filesystem::path WriteI18nFixture()
        {
            const std::filesystem::path directory =
                std::filesystem::current_path()
                / GRAPH_TASK_TEST_PATH_LITERAL("graphtask-focus-i18n-fixture");
            std::filesystem::create_directories(directory);
            {
                std::ofstream file(
                    directory / GRAPH_TASK_TEST_PATH_LITERAL("en.json"),
                    std::ios::binary);
                file << "{\"graphTask.finished\": \"Graph task finished: "
                        "end value = {0}\"}";
            }
            {
                std::ofstream file(
                    directory / GRAPH_TASK_TEST_PATH_LITERAL("zh-CN.json"),
                    std::ios::binary);
                file << "{\"graphTask.finished\": \"图任务完成：结束值 = "
                        "{0}\"}";
            }
            return directory;
        }

        std::string ReadUserJsonlText()
        {
            DasPtr<IDasReadOnlyString> path_string;
            if (DAS::IsFailed(session_->GetUserJsonlPath(path_string.Put()))
                || !path_string)
            {
                return {};
            }
            const char* utf8 = nullptr;
            if (DAS::IsFailed(path_string->GetUtf8(&utf8)) || utf8 == nullptr)
            {
                return {};
            }
            const std::filesystem::path jsonl_path{
                reinterpret_cast<const char8_t*>(utf8)};
            std::ifstream file(jsonl_path, std::ios::binary);
            return std::string(
                (std::istreambuf_iterator<char>(file)),
                std::istreambuf_iterator<char>());
        }
    };

    TEST_F(GraphTaskFocusLogFixture, RecordsFinishedEntryWithEnglishTemplate)
    {
        SetDefaultLocale("en");
        const std::filesystem::path i18n_directory = WriteI18nFixture();

        EXPECT_EQ(
            RecordGraphTaskFinishedFocusEntry(
                recorder_.Get(),
                i18n_directory,
                42),
            DAS_S_OK);

        const std::string jsonl_text = ReadUserJsonlText();
        EXPECT_NE(
            jsonl_text.find("Graph task finished: end value = 42"),
            std::string::npos);
    }

    TEST_F(GraphTaskFocusLogFixture, RecordsFinishedEntryWithZhCnTemplate)
    {
        SetDefaultLocale("zh-CN");
        const std::filesystem::path i18n_directory = WriteI18nFixture();

        EXPECT_EQ(
            RecordGraphTaskFinishedFocusEntry(
                recorder_.Get(),
                i18n_directory,
                42),
            DAS_S_OK);

        const std::string jsonl_text = ReadUserJsonlText();
        EXPECT_NE(
            jsonl_text.find("图任务完成：结束值 = 42"),
            std::string::npos);
    }

    TEST_F(
        GraphTaskFocusLogFixture,
        FallsBackToLiteralTemplateWhenI18nTableMissing)
    {
        SetDefaultLocale("zh-CN");
        const std::filesystem::path missing_directory =
            std::filesystem::current_path()
            / GRAPH_TASK_TEST_PATH_LITERAL("graphtask-focus-i18n-absent");

        EXPECT_EQ(
            RecordGraphTaskFinishedFocusEntry(
                recorder_.Get(),
                missing_directory,
                -7),
            DAS_S_OK);

        // 表缺失回落 en 字面模板（base locale 语义），负值原样渲染。
        const std::string jsonl_text = ReadUserJsonlText();
        EXPECT_NE(
            jsonl_text.find("Graph task finished: end value = -7"),
            std::string::npos);
    }

    TEST(GraphTaskFocusLogPublish, PublishNeverFailsTheTaskPath)
    {
        // 生产入口在无 recorder 服务/无 i18n 表的环境（单测进程）不抛不
        // 崩，降级只落调试日志——Do 返回码不受 focus 发布影响。
        PublishGraphTaskFinishedFocusEntry(0);
        SUCCEED();
    }

} // namespace
