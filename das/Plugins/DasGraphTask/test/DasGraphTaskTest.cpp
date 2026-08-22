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
    const DasResult hr = task->Do(nullptr, nullptr, result.Put());
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
// DasGraphTaskImpl::Do (compiledPlan port) and execute without error on an
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

    // Serialise the plan to the compiledPlan input port. The Core Compile
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
    const DasReadOnlyString compiled_plan_key{"compiledPlan"};
    const DasReadOnlyString compiled_plan_value{inner_plan->c_str()};
    ASSERT_EQ(
        input_map->SetString(
            compiled_plan_key.Get(),
            compiled_plan_value.Get()),
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

    // 组件持有可用 host：空 plan 经 host 路径执行成功（host 缺失时 Do
    // 结构化失败）。
    DasPtr<Das::ExportInterface::IDasPortMap> result;
    EXPECT_TRUE(DAS::IsOk(component->Do(nullptr, nullptr, result.Put())));
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
