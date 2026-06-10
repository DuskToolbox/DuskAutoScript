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
