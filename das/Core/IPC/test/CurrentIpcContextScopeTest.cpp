#include <das/Core/IPC/CurrentIpcContextScope.h>
#include <gtest/gtest.h>

using DAS::Core::IPC::GetCurrentIpcContext;
using DAS::Core::IPC::IResolveContext;
using DAS::Core::IPC::ScopedCurrentIpcContext;

TEST(CurrentIpcContextScopeTest, GetContext_InitiallyNull)
{
    EXPECT_EQ(GetCurrentIpcContext(), nullptr);
}

TEST(CurrentIpcContextScopeTest, GetContext_InsideScope)
{
    auto*                   fake_ctx = reinterpret_cast<IResolveContext*>(1);
    ScopedCurrentIpcContext scope{fake_ctx};
    EXPECT_EQ(GetCurrentIpcContext(), fake_ctx);
}

TEST(CurrentIpcContextScopeTest, GetContext_AfterScopeExit)
{
    auto* fake_ctx = reinterpret_cast<IResolveContext*>(1);
    {
        ScopedCurrentIpcContext scope{fake_ctx};
        EXPECT_EQ(GetCurrentIpcContext(), fake_ctx);
    }
    EXPECT_EQ(GetCurrentIpcContext(), nullptr);
}
