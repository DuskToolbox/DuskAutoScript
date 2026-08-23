#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/Core/TaskScheduler/TaskAuthoringContract.h>
#include <das/DasApi.h>
#include <das/DasPtr.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <string>
#include <string_view>

using IDasGraphRuntime = Das::ExportInterface::IDasGraphRuntime;

// ---- Tests ----

TEST(GraphRuntimeFactoryTest, CreateGraphRuntimeReturnsValidPointer)
{
    Das::DasPtr<IDasGraphRuntime> runtime;
    const DasResult               hr = CreateGraphRuntime(runtime.Put());
    ASSERT_EQ(hr, DAS_S_OK);
    ASSERT_NE(runtime.Get(), nullptr);
}

TEST(GraphRuntimeFactoryTest, CreateGraphRuntimeNullOutParam)
{
    const DasResult hr = CreateGraphRuntime(nullptr);
    ASSERT_EQ(hr, DAS_E_INVALID_POINTER);
}

TEST(GraphRuntimeFactoryTest, IDasGraphRuntimeGetErrorMessage)
{
    Das::DasPtr<IDasGraphRuntime> runtime;
    ASSERT_EQ(CreateGraphRuntime(runtime.Put()), DAS_S_OK);

    Das::DasPtr<IDasReadOnlyString> error_msg;
    const DasResult hr = runtime->GetErrorMessage(error_msg.Put());
    EXPECT_EQ(hr, DAS_S_OK);
}

TEST(GraphRuntimeFactoryTest, IDasGraphRuntimeGetErrorMessageNullPointer)
{
    Das::DasPtr<IDasGraphRuntime> runtime;
    ASSERT_EQ(CreateGraphRuntime(runtime.Put()), DAS_S_OK);

    const DasResult hr = runtime->GetErrorMessage(nullptr);
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
    const DasResult hr = runtime->GetErrorMessage(error_msg.Put());
    EXPECT_EQ(hr, DAS_S_OK);
}

TEST(GraphRuntimeFactoryTest, CCompliance_CreateGraphRuntime)
{
    using FactoryFn = DasResult (*)(IDasGraphRuntime**);
    FactoryFn fn = &CreateGraphRuntime;
    ASSERT_NE(fn, nullptr);

    Das::DasPtr<IDasGraphRuntime> runtime;
    const DasResult               hr = fn(runtime.Put());
    ASSERT_EQ(hr, DAS_S_OK);
    ASSERT_NE(runtime.Get(), nullptr);
}

// ===========================================================================
// GraphAuthoringSessionCompile strict authoring envelope
// ===========================================================================

namespace
{
    constexpr std::string_view FORM_SEQUENCE_WITH_ONE_ITEM = R"({
        "items": [
            {
                "itemId": "first",
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

    constexpr std::string_view CYCLIC_GRAPH_DOC = R"({
        "nodes": [
            {
                "nodeId": "a",
                "target": {
                    "targetKind": "componentRef",
                    "componentRef": {
                        "kind": "componentRef",
                        "componentGuid": "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA",
                        "pluginGuid": "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"
                    }
                },
                "settings": {},
                "dynamicPorts": []
            },
            {
                "nodeId": "b",
                "target": {
                    "targetKind": "componentRef",
                    "componentRef": {
                        "kind": "componentRef",
                        "componentGuid": "CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC",
                        "pluginGuid": "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"
                    }
                },
                "settings": {},
                "dynamicPorts": []
            }
        ],
        "edges": [
            {
                "edgeId": "e1",
                "sourceNodeId": "a",
                "sourcePortId": "out",
                "targetNodeId": "b",
                "targetPortId": "in",
                "edgeType": "data"
            },
            {
                "edgeId": "e2",
                "sourceNodeId": "b",
                "sourcePortId": "out",
                "targetNodeId": "a",
                "targetPortId": "in",
                "edgeType": "data"
            }
        ]
    })";

    // RAII handle over a Core authoring session seeded from a raw document.
    struct AuthoringSessionHandle
    {
        GraphAuthoringSessionState* state = nullptr;

        explicit AuthoringSessionHandle(const std::string& context)
        {
            Das::DasPtr<IDasReadOnlyString> context_string;
            CreateIDasReadOnlyStringFromUtf8(
                context.c_str(),
                context_string.Put());
            CreateGraphAuthoringSession(context_string.Get(), &state);
        }

        ~AuthoringSessionHandle() { DestroyGraphAuthoringSession(state); }

        AuthoringSessionHandle(const AuthoringSessionHandle&) = delete;
        AuthoringSessionHandle& operator=(const AuthoringSessionHandle&) =
            delete;
    };

    std::string JsonText(Das::ExportInterface::IDasJson* json)
    {
        if (json == nullptr)
        {
            return {};
        }
        Das::DasPtr<IDasReadOnlyString> text;
        if (DAS::IsFailed(json->ToString(0, text.Put())) || !text.Get())
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

    Das::DasPtr<Das::ExportInterface::IDasJson> CompileSessionPlan(
        GraphAuthoringSessionState* state)
    {
        Das::DasPtr<Das::ExportInterface::IDasJson> compiled;
        GraphAuthoringSessionCompile(state, nullptr, compiled.Put());
        return compiled;
    }
} // namespace

// The Compile result must satisfy the strict scheduler-side validator
// (OnlyKeys {compile, diagnostics}, diagnostics an array) — a bare
// CompiledGraphPlanDto is rejected with missingField.
TEST(GraphRuntimeFactoryTest, CompileEnvelopePassesSchedulerValidator)
{
    const AuthoringSessionHandle session{
        std::string(FORM_SEQUENCE_WITH_ONE_ITEM)};

    auto compiled = CompileSessionPlan(session.state);
    ASSERT_NE(compiled.Get(), nullptr);

    const auto root = yyjson::read(JsonText(compiled.Get()));
    const auto validation =
        Das::Core::TaskScheduler::ValidateAuthoringCompileResult(root);
    EXPECT_TRUE(validation.valid)
        << validation.error_kind << ": " << validation.message;
}

// compile.executionInput must round-trip back into the compiled plan DTO
// with node identity and component wiring intact. The wire artifact is the
// {"fingerprint","plan"} shell; the DTO rides under the "plan" member.
TEST(GraphRuntimeFactoryTest, CompileEnvelopeExecutionInputRestoresPlan)
{
    const AuthoringSessionHandle session{
        std::string(FORM_SEQUENCE_WITH_ONE_ITEM)};

    auto compiled = CompileSessionPlan(session.state);
    ASSERT_NE(compiled.Get(), nullptr);

    const auto root = yyjson::read(JsonText(compiled.Get())).as_object();
    ASSERT_TRUE(root.has_value());
    ASSERT_TRUE(root->contains(std::string_view("compile")));
    const auto compile_section =
        (*root)[std::string_view("compile")].as_object();
    ASSERT_TRUE(compile_section.has_value());
    ASSERT_TRUE(compile_section->contains(std::string_view("executionInput")));

    const auto shell =
        (*compile_section)[std::string_view("executionInput")].as_object();
    ASSERT_TRUE(shell.has_value());
    ASSERT_TRUE(shell->contains(std::string_view("plan")));

    const auto plan =
        yyjson::cast<Das::Core::GraphRuntime::Dto::CompiledGraphPlanDto>(
            (*shell)[std::string_view("plan")]);
    ASSERT_EQ(plan.execution_order.size(), 1u);
    ASSERT_EQ(plan.node_snapshots.size(), 1u);
    EXPECT_EQ(
        plan.node_snapshots[0].component_guid,
        std::string("AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA"));
}

// plan.diagnostics (e.g. a cyclic graph) must surface in the envelope's
// top-level diagnostics array, not be swallowed by the wrapper.
TEST(GraphRuntimeFactoryTest, CompileEnvelopeCarriesDiagnostics)
{
    const AuthoringSessionHandle session{std::string(CYCLIC_GRAPH_DOC)};

    auto compiled = CompileSessionPlan(session.state);
    ASSERT_NE(compiled.Get(), nullptr);

    const auto root = yyjson::read(JsonText(compiled.Get())).as_object();
    ASSERT_TRUE(root.has_value());
    ASSERT_TRUE(root->contains(std::string_view("diagnostics")));
    const auto diagnostics =
        (*root)[std::string_view("diagnostics")].as_array();
    ASSERT_TRUE(diagnostics.has_value());
    EXPECT_FALSE(diagnostics->empty());
}

// The scheduler's execution-input extraction only reads the
// compile.executionInput key — the envelope must expose the whole plan
// there so graph tasks receive a non-null input (a bare plan has no such
// key and extraction returned null).
TEST(GraphRuntimeFactoryTest, CompileEnvelopeExposesExecutionInputKey)
{
    const AuthoringSessionHandle session{
        std::string(FORM_SEQUENCE_WITH_ONE_ITEM)};

    auto compiled = CompileSessionPlan(session.state);
    ASSERT_NE(compiled.Get(), nullptr);

    const auto root = yyjson::read(JsonText(compiled.Get()));
    const auto envelope = root.as_object();
    ASSERT_TRUE(envelope.has_value());
    const auto compile_section =
        (*envelope)[std::string_view("compile")].as_object();
    ASSERT_TRUE(compile_section.has_value());
    ASSERT_TRUE(compile_section->contains(std::string_view("executionInput")));

    // Mirror the scheduler extraction: clone the value under the key and
    // confirm it is a compiled plan object, not a null placeholder.
    const auto extracted = Das::Utils::CloneYyjsonValue(
        (*compile_section)[std::string_view("executionInput")]);
    ASSERT_TRUE(extracted.as_object().has_value());
    const auto text = Das::Utils::SerializeYyjsonValue(extracted);
    ASSERT_TRUE(text.has_value());
    EXPECT_NE(text->find("executionOrder"), std::string::npos);
}

// ===========================================================================
// GraphRuntimeImpl::Execute malformed-artifact guards
// ===========================================================================

namespace
{
    // 最小 host 替身：Execute 的引擎管线要求非空 host；空计划不会调用
    // CreateTaskComponent（有节点时返回 NOT_FOUND）。
    class NullServingTaskComponentHost final
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
            std::ignore = component_guid;
            if (pp_out_component == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_component = nullptr;
            return DAS_E_NOT_FOUND;
        }
    };

    DasResult ExecuteArtifactText(std::string_view artifact_utf8)
    {
        auto host = Das::DasPtr<Das::PluginInterface::IDasTaskComponentHost>(
            new NullServingTaskComponentHost());
        Das::DasPtr<IDasGraphRuntime> runtime;
        const DasResult               create_result =
            CreateGraphRuntimeWithHost(host.Get(), runtime.Put());
        if (DAS::IsFailed(create_result))
        {
            return create_result;
        }
        Das::DasPtr<IDasReadOnlyString> artifact;
        if (DAS::IsFailed(CreateIDasReadOnlyStringFromUtf8(
                std::string(artifact_utf8).c_str(),
                artifact.Put())))
        {
            return DAS_E_FAIL;
        }
        Das::DasPtr<Das::ExportInterface::IDasJson> result_json;
        return runtime->Execute(artifact.Get(), nullptr, result_json.Put());
    }
} // namespace

// Malformed JSON must surface as DAS_E_INVALID_JSON instead of letting the
// yyjson read exception escape across the C ABI boundary.
TEST(GraphRuntimeFactoryTest, ExecuteMalformedJsonReturnsInvalidJson)
{
    EXPECT_EQ(ExecuteArtifactText("{ not json"), DAS_E_INVALID_JSON);
}

// A structurally invalid artifact (missing required plan fields) fails the
// DTO cast and is rejected with DAS_E_INVALID_JSON rather than throwing.
TEST(GraphRuntimeFactoryTest, ExecuteIncompletePlanJsonReturnsInvalidJson)
{
    EXPECT_EQ(ExecuteArtifactText(R"({"foo":1})"), DAS_E_INVALID_JSON);
}

// ===========================================================================
// Fingerprint shell — the wire artifact is a {fingerprint, plan} envelope;
// fingerprint = FNV-1a 64 over the standard serialization of the plan
// subtree. Compile produces it, Execute re-computes and compares it before
// executing (tamper evidence for the persisted artifact).
// ===========================================================================

namespace
{
    constexpr std::string_view FORM_SEQUENCE_WITH_OTHER_COMPONENT = R"({
        "items": [
            {
                "itemId": "first",
                "target": {
                    "targetKind": "componentRef",
                    "componentRef": {
                        "kind": "componentRef",
                        "componentGuid": "CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC",
                        "pluginGuid": "BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"
                    }
                },
                "settings": {}
            }
        ]
    })";

    // Compile the given context and return the executionInput shell's
    // fingerprint member ("" when the shell/fingerprint is absent).
    std::string ExecutionInputFingerprint(std::string_view context_json)
    {
        const AuthoringSessionHandle session{std::string(context_json)};
        auto compiled = CompileSessionPlan(session.state);
        if (compiled.Get() == nullptr)
        {
            return {};
        }

        auto root = yyjson::read(JsonText(compiled.Get()));
        auto root_obj = root.as_object();
        if (!root_obj || !root_obj->contains(std::string_view("compile")))
        {
            return {};
        }
        auto compile_section =
            (*root_obj)[std::string_view("compile")].as_object();
        if (!compile_section
            || !compile_section->contains(std::string_view("executionInput")))
        {
            return {};
        }
        auto shell =
            (*compile_section)[std::string_view("executionInput")].as_object();
        if (!shell || !shell->contains(std::string_view("fingerprint")))
        {
            return {};
        }
        auto fingerprint =
            (*shell)[std::string_view("fingerprint")].as_string();
        return fingerprint ? std::string(*fingerprint) : std::string{};
    }

    // Compile the given context and return the executionInput shell as text.
    std::string ExecutionInputShellText(std::string_view context_json)
    {
        const AuthoringSessionHandle session{std::string(context_json)};
        auto compiled = CompileSessionPlan(session.state);
        if (compiled.Get() == nullptr)
        {
            return {};
        }

        auto root = yyjson::read(JsonText(compiled.Get()));
        auto root_obj = root.as_object();
        if (!root_obj || !root_obj->contains(std::string_view("compile")))
        {
            return {};
        }
        auto compile_section =
            (*root_obj)[std::string_view("compile")].as_object();
        if (!compile_section
            || !compile_section->contains(std::string_view("executionInput")))
        {
            return {};
        }
        auto shell = Das::Utils::CloneYyjsonValue(
            (*compile_section)[std::string_view("executionInput")]);
        auto text = Das::Utils::SerializeYyjsonValue(shell);
        return text ? *text : std::string{};
    }
} // namespace

// Equivalence: the same document compiled twice yields the same fingerprint.
TEST(GraphRuntimeFactoryTest, CompileShellFingerprintStableAcrossCompiles)
{
    const auto first = ExecutionInputFingerprint(FORM_SEQUENCE_WITH_ONE_ITEM);
    const auto second = ExecutionInputFingerprint(FORM_SEQUENCE_WITH_ONE_ITEM);
    EXPECT_FALSE(first.empty());
    EXPECT_EQ(first, second);
}

// Equivalence: a changed plan (different componentGuid) yields a changed
// fingerprint.
TEST(GraphRuntimeFactoryTest, CompileShellFingerprintChangesWithPlanContent)
{
    const auto first = ExecutionInputFingerprint(FORM_SEQUENCE_WITH_ONE_ITEM);
    const auto second =
        ExecutionInputFingerprint(FORM_SEQUENCE_WITH_OTHER_COMPONENT);
    EXPECT_FALSE(first.empty());
    EXPECT_FALSE(second.empty());
    EXPECT_NE(first, second);
}

// Tamper evidence: an untampered shell executes; editing the plan payload
// inside the shell (leaving the fingerprint untouched) must be rejected
// before any execution happens.
TEST(GraphRuntimeFactoryTest, ExecuteRejectsTamperedPlanPayloadInShell)
{
    // Untampered: an empty document's shell executes cleanly (empty graph).
    const auto valid_shell = ExecutionInputShellText("");
    ASSERT_FALSE(valid_shell.empty());
    EXPECT_EQ(ExecuteArtifactText(valid_shell), DAS_S_OK);

    // Tampered: modify the plan payload member inside a compiled shell.
    const AuthoringSessionHandle session{
        std::string(FORM_SEQUENCE_WITH_ONE_ITEM)};
    auto compiled = CompileSessionPlan(session.state);
    ASSERT_NE(compiled.Get(), nullptr);
    auto root = yyjson::read(JsonText(compiled.Get()));
    auto root_obj = root.as_object();
    ASSERT_TRUE(root_obj.has_value());
    auto compile_section = (*root_obj)[std::string_view("compile")].as_object();
    ASSERT_TRUE(compile_section.has_value());

    auto shell = Das::Utils::CloneYyjsonValue(
        (*compile_section)[std::string_view("executionInput")]);
    auto shell_obj = shell.as_object();
    ASSERT_TRUE(shell_obj.has_value());
    auto plan_obj = (*shell_obj)[std::string_view("plan")].as_object();
    ASSERT_TRUE(plan_obj.has_value());
    (*plan_obj)[std::string_view("documentId")] =
        std::make_pair(std::string_view("tampered"), yyjson::copy_string);

    const auto tampered_text = Das::Utils::SerializeYyjsonValue(shell);
    ASSERT_TRUE(tampered_text.has_value());
    EXPECT_TRUE(DAS::IsFailed(ExecuteArtifactText(*tampered_text)));
}
