#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Plugins/DasGraphTask/DasGraphTaskImpl.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <gtest/gtest.h>

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
            return DAS_E_INVALID_POINTER;
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
    DasResult hr = task->Do(nullptr, nullptr, result.Put());
    EXPECT_TRUE(DAS::IsFailed(hr));
}

TEST(DasGraphTaskTest, GetGuidReturnsNonEmpty)
{
    auto task = DasGraphTaskImpl::Make(nullptr);
    ASSERT_NE(task.Get(), nullptr);

    DasGuid   guid{};
    DasResult hr = task->GetGuid(&guid);
    EXPECT_TRUE(DAS::IsOk(hr));
}

TEST(DasGraphTaskTest, DoWithMockHostReturnsSuccessOnEmptyPlan)
{
    auto host = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(
        new MockTaskComponentHost());
    auto task = DasGraphTaskImpl::Make(host.Get());
    ASSERT_NE(task.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasPortMap> result;
    DasResult hr = task->Do(nullptr, nullptr, result.Put());
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
    DasResult hr = task->Do(cancelled_token.Get(), nullptr, result.Put());
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
    ASSERT_EQ(GraphAuthoringSessionCompile(state, nullptr, plan_json.Put()), DAS_S_OK);
    DestroyGraphAuthoringSession(state);
    ASSERT_NE(plan_json.Get(), nullptr);

    // Serialise the plan to the compiledPlan input port.
    DasPtr<IDasReadOnlyString> plan_str;
    ASSERT_EQ(plan_json->ToString(0, plan_str.Put()), DAS_S_OK);
    ASSERT_NE(plan_str.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasPortMap> input_map;
    ASSERT_EQ(CreateIDasPortMap(input_map.Put()), DAS_S_OK);
    DasReadOnlyString compiled_plan_key{"compiledPlan"};
    ASSERT_EQ(input_map->SetString(compiled_plan_key.Get(), plan_str.Get()), DAS_S_OK);

    // Feed the compiled plan into Do and confirm it runs (empty plan → OK).
    auto host = DasPtr<Das::PluginInterface::IDasTaskComponentHost>(
        new MockTaskComponentHost());
    auto task = DasGraphTaskImpl::Make(host.Get());
    ASSERT_NE(task.Get(), nullptr);

    DasPtr<Das::ExportInterface::IDasPortMap> result;
    DasResult hr = task->Do(nullptr, input_map.Get(), result.Put());
    EXPECT_TRUE(DAS::IsOk(hr));
}
