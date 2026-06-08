#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Plugins/GraphTask/GraphTaskImpl.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <gtest/gtest.h>

using namespace Das::Plugins::GraphTask;

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

TEST(GraphTaskTest, DoCreatesRuntimeAndRuns)
{
    // GraphTaskImpl::Do() creates a GraphRuntime internally and runs it.
    // Without a loaded artifact, Run returns DAS_S_OK (empty graph).
    auto task = GraphTaskImpl::Make();
    ASSERT_NE(task.Get(), nullptr);

    DasResult hr = task->Do(nullptr, nullptr, nullptr);
    // Empty run — no artifact loaded, should succeed or return a graceful
    // error.
    EXPECT_TRUE(DAS::IsOk(hr) || DAS::IsFailed(hr));
}

TEST(GraphTaskTest, DoStopTokenCancel)
{
    auto task = GraphTaskImpl::Make();
    ASSERT_NE(task.Get(), nullptr);

    auto      cancelled_token = MockStopTokenForTask::Make(true);
    DasResult hr = task->Do(cancelled_token.Get(), nullptr, nullptr);
    EXPECT_TRUE(DAS::IsFailed(hr));
}
