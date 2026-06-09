#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/DasPtr.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <gtest/gtest.h>

#include <cstring>

using IDasGraphRuntime = Das::ExportInterface::IDasGraphRuntime;

// ---- Tests ----

TEST(GraphRuntimeFactoryTest, CreateGraphRuntimeReturnsValidPointer)
{
    Das::DasPtr<IDasGraphRuntime> runtime;
    DasResult                     hr = CreateGraphRuntime(runtime.Put());
    ASSERT_EQ(hr, DAS_S_OK);
    ASSERT_NE(runtime.Get(), nullptr);
}

TEST(GraphRuntimeFactoryTest, CreateGraphRuntimeNullOutParam)
{
    DasResult hr = CreateGraphRuntime(nullptr);
    ASSERT_EQ(hr, DAS_E_INVALID_POINTER);
}

TEST(GraphRuntimeFactoryTest, IDasGraphRuntimeGetErrorMessage)
{
    Das::DasPtr<IDasGraphRuntime> runtime;
    ASSERT_EQ(CreateGraphRuntime(runtime.Put()), DAS_S_OK);

    Das::DasPtr<IDasReadOnlyString> error_msg;
    DasResult hr = runtime->GetErrorMessage(error_msg.Put());
    EXPECT_EQ(hr, DAS_S_OK);
}

TEST(GraphRuntimeFactoryTest, IDasGraphRuntimeGetErrorMessageNullPointer)
{
    Das::DasPtr<IDasGraphRuntime> runtime;
    ASSERT_EQ(CreateGraphRuntime(runtime.Put()), DAS_S_OK);

    DasResult hr = runtime->GetErrorMessage(nullptr);
    EXPECT_EQ(hr, DAS_E_INVALID_POINTER);
}

TEST(GraphRuntimeFactoryTest, IDasGraphRuntimeRefCounting)
{
    Das::DasPtr<IDasGraphRuntime> runtime;
    ASSERT_EQ(CreateGraphRuntime(runtime.Put()), DAS_S_OK);

    uint32_t ref = runtime->AddRef();
    EXPECT_GT(ref, 1u);
    ref = runtime->Release();
    EXPECT_GE(ref, 1u);

    Das::DasPtr<IDasReadOnlyString> error_msg;
    DasResult hr = runtime->GetErrorMessage(error_msg.Put());
    EXPECT_EQ(hr, DAS_S_OK);
}

TEST(GraphRuntimeFactoryTest, CCompliance_CreateGraphRuntime)
{
    using FactoryFn = DasResult (*)(IDasGraphRuntime**);
    FactoryFn fn = &CreateGraphRuntime;
    ASSERT_NE(fn, nullptr);

    Das::DasPtr<IDasGraphRuntime> runtime;
    DasResult                     hr = fn(runtime.Put());
    ASSERT_EQ(hr, DAS_S_OK);
    ASSERT_NE(runtime.Get(), nullptr);
}
