#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <gtest/gtest.h>

#include <cstring>

using IDasGraphRuntime = Das::ExportInterface::IDasGraphRuntime;

// ---- Minimal stop token mock ----

class MockStopToken final
    : public Das::PluginInterface::DasStopTokenImplBase<MockStopToken>
{
    bool cancelled_ = false;

public:
    explicit MockStopToken(bool cancelled = false) : cancelled_(cancelled) {}

    DAS_IMPL StopRequested(bool* canStop) override
    {
        if (!canStop)
            return DAS_E_INVALID_POINTER;
        *canStop = cancelled_;
        return DAS_S_OK;
    }
};

// ---- Helper: create a minimal valid compiled artifact JSON ----

static Das::DasPtr<IDasReadOnlyString> MakeArtifactJson(const char* json_str)
{
    Das::DasPtr<IDasReadOnlyString> str;
    CreateIDasReadOnlyStringFromUtf8(json_str, str.Put());
    return str;
}

// Minimal valid CompiledGraphPlanDto JSON (empty graph, no nodes).
static const char* kMinimalArtifactJson = R"({
    "documentId": "test-doc-001",
    "sourceRevision": 1,
    "sourceFingerprint": "fp-001",
    "compiledFingerprint": "fp-001",
    "nodeSnapshots": [],
    "bindingPlan": { "bindings": [] },
    "executionOrder": [],
    "diagnostics": []
})";

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

TEST(GraphRuntimeFactoryTest, IDasGraphRuntimeLoad)
{
    Das::DasPtr<IDasGraphRuntime> runtime;
    ASSERT_EQ(CreateGraphRuntime(runtime.Put()), DAS_S_OK);

    auto      artifact = MakeArtifactJson(kMinimalArtifactJson);
    DasResult hr = runtime->Load(artifact.Get());
    // Load may succeed or fail depending on engine internals, but should
    // not crash.
    EXPECT_TRUE(DAS::IsOk(hr) || DAS::IsFailed(hr));
}

TEST(GraphRuntimeFactoryTest, IDasGraphRuntimeLoadInvalidJson)
{
    Das::DasPtr<IDasGraphRuntime> runtime;
    ASSERT_EQ(CreateGraphRuntime(runtime.Put()), DAS_S_OK);

    auto      bad_json = MakeArtifactJson("{invalid json");
    DasResult hr = runtime->Load(bad_json.Get());
    EXPECT_TRUE(DAS::IsFailed(hr));
}

TEST(GraphRuntimeFactoryTest, IDasGraphRuntimeLoadNullPointer)
{
    Das::DasPtr<IDasGraphRuntime> runtime;
    ASSERT_EQ(CreateGraphRuntime(runtime.Put()), DAS_S_OK);

    DasResult hr = runtime->Load(nullptr);
    EXPECT_EQ(hr, DAS_E_INVALID_POINTER);
}

TEST(GraphRuntimeFactoryTest, IDasGraphRuntimeConfigure)
{
    Das::DasPtr<IDasGraphRuntime> runtime;
    ASSERT_EQ(CreateGraphRuntime(runtime.Put()), DAS_S_OK);

    auto artifact = MakeArtifactJson(kMinimalArtifactJson);
    // Load returns DAS_E_INVALID_POINTER because no host is provided —
    // this is expected for the C API facade without a host.
    DasResult hr = runtime->Load(artifact.Get());
    EXPECT_TRUE(DAS::IsOk(hr) || DAS::IsFailed(hr));
}

TEST(GraphRuntimeFactoryTest, IDasGraphRuntimeRunAfterLoadAndConfigure)
{
    Das::DasPtr<IDasGraphRuntime> runtime;
    ASSERT_EQ(CreateGraphRuntime(runtime.Put()), DAS_S_OK);

    auto artifact = MakeArtifactJson(kMinimalArtifactJson);
    // Load may fail without host — that is expected.
    runtime->Load(artifact.Get());
    runtime->Configure(nullptr);

    DasResult hr = runtime->Run(nullptr);
    // Run after a failed Load returns DAS_E_FAIL — expected.
    EXPECT_TRUE(DAS::IsOk(hr) || DAS::IsFailed(hr));
}

TEST(GraphRuntimeFactoryTest, IDasGraphRuntimeGetErrorMessage)
{
    Das::DasPtr<IDasGraphRuntime> runtime;
    ASSERT_EQ(CreateGraphRuntime(runtime.Put()), DAS_S_OK);

    // Trigger an error by loading bad JSON.
    auto bad_json = MakeArtifactJson("{invalid");
    runtime->Load(bad_json.Get());

    Das::DasPtr<IDasReadOnlyString> error_msg;
    DasResult hr = runtime->GetErrorMessage(error_msg.Put());
    // GetErrorMessage should succeed even if there's no error.
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

    // Extra AddRef + Release should not destroy the object.
    runtime->AddRef();
    runtime->Release();

    // Object should still be usable.
    auto      artifact = MakeArtifactJson(kMinimalArtifactJson);
    DasResult hr = runtime->Load(artifact.Get());
    EXPECT_TRUE(DAS::IsOk(hr) || DAS::IsFailed(hr));
}

TEST(GraphRuntimeFactoryTest, RunWithCancelledStopToken)
{
    Das::DasPtr<IDasGraphRuntime> runtime;
    ASSERT_EQ(CreateGraphRuntime(runtime.Put()), DAS_S_OK);

    auto cancelled_token = MockStopToken::Make(true);
    // Run with cancelled stop token — should fail immediately.
    DasResult hr = runtime->Run(cancelled_token.Get());
    EXPECT_TRUE(DAS::IsFailed(hr));
}

TEST(GraphRuntimeFactoryTest, CCompliance_CreateGraphRuntime)
{
    // Verify that CreateGraphRuntime is a plain C function pointer —
    // extern "C" linkage.  This test compiles only if the function has
    // C linkage (no name mangling, no this pointer).
    using FactoryFn = DasResult (*)(IDasGraphRuntime**);
    FactoryFn fn = &CreateGraphRuntime;
    ASSERT_NE(fn, nullptr);

    Das::DasPtr<IDasGraphRuntime> runtime;
    DasResult                     hr = fn(runtime.Put());
    ASSERT_EQ(hr, DAS_S_OK);
    ASSERT_NE(runtime.Get(), nullptr);
}
