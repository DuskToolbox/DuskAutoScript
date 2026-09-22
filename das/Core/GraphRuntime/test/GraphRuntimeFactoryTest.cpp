#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/CompiledPlanFingerprint.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphBoundaryComponents.h>
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
                "settings": {}
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
                "settings": {}
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
            CreateGraphAuthoringSession(nullptr, context_string.Get(), &state);
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
    class NullServingTaskComponentCreator final
        : public Das::PluginInterface::IDasTaskComponentCreator
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
            if (iid
                == DasIidOf<Das::PluginInterface::IDasTaskComponentCreator>())
            {
                *pp_out = static_cast<
                    Das::PluginInterface::IDasTaskComponentCreator*>(this);
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

        DasResult DAS_STD_CALL CreateAuthoringSession(
            const DasGuid&                                   task_guid,
            Das::ExportInterface::IDasJson*                  p_context_json,
            Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session)
            override
        {
            std::ignore = task_guid;
            std::ignore = p_context_json;
            if (pp_out_session == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_session = nullptr;
            return DAS_E_NO_IMPLEMENTATION;
        }
    };

    DasResult ExecuteArtifactText(std::string_view artifact_utf8)
    {
        auto host = Das::DasPtr<Das::PluginInterface::IDasTaskComponentCreator>(
            new NullServingTaskComponentCreator());
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

// Null / empty artifacts are rejected up front: silently running an empty
// plan would mask the caller's assembly gap as success.
TEST(GraphRuntimeFactoryTest, ExecuteNullArtifactReturnsInvalidArgument)
{
    auto host = Das::DasPtr<Das::PluginInterface::IDasTaskComponentCreator>(
        new NullServingTaskComponentCreator());
    Das::DasPtr<IDasGraphRuntime> runtime;
    ASSERT_EQ(DAS_S_OK, CreateGraphRuntimeWithHost(host.Get(), runtime.Put()));
    Das::DasPtr<Das::ExportInterface::IDasJson> result_json;
    EXPECT_EQ(
        runtime->Execute(nullptr, nullptr, result_json.Put()),
        DAS_E_INVALID_ARGUMENT);
}

TEST(GraphRuntimeFactoryTest, ExecuteEmptyArtifactReturnsInvalidArgument)
{
    EXPECT_EQ(ExecuteArtifactText(""), DAS_E_INVALID_ARGUMENT);
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
    // Untampered: an empty formSequence document compiles to a zero-node
    // shell that executes cleanly (graph-mode presets Begin/End nodes that
    // a null-serving host cannot resolve — the empty-plan semantics this
    // case pins ride the linear context).
    const auto valid_shell = ExecutionInputShellText(R"({"items":[]})");
    ASSERT_FALSE(valid_shell.empty());
    // 指纹自洽分解：输入路径与 Execute 完全同构（经 IDasReadOnlyString
    // 往返 + 重算），任何一环失败都指向 mismatch 拒绝的真实来源。
    {
        Das::DasPtr<IDasReadOnlyString> artifact;
        ASSERT_EQ(
            CreateIDasReadOnlyStringFromUtf8(
                valid_shell.c_str(),
                artifact.Put()),
            DAS_S_OK);
        const char* artifact_utf8 = nullptr;
        ASSERT_EQ(artifact->GetUtf8(&artifact_utf8), DAS_S_OK);
        const std::string round_trip(
            artifact_utf8 != nullptr ? artifact_utf8 : "");
        EXPECT_EQ(round_trip, valid_shell)
            << "IDasReadOnlyString round trip altered the shell text";

        auto root = yyjson::read(round_trip);
        auto shell_obj = root.as_object();
        ASSERT_TRUE(shell_obj.has_value());
        const auto stored =
            (*shell_obj)[std::string_view("fingerprint")].as_string();
        ASSERT_TRUE(stored.has_value());
        const auto recomputed =
            Das::Core::GraphRuntime::ComputeCompiledPlanFingerprint(
                (*shell_obj)[std::string_view("plan")]);
        EXPECT_EQ(std::string_view(*stored), std::string_view(recomputed))
            << "shell recompute mismatched the stored fingerprint";

        // 对照：与 ParseVerifiedPlan 逐字相同的 const auto& 视图绑定形
        // 态——若此形态重算与值形态不同，mismatch 的根因即视图绑定。
        const auto& plan_subtree = (*shell_obj)[std::string_view("plan")];
        const auto  recomputed_via_view =
            Das::Core::GraphRuntime::ComputeCompiledPlanFingerprint(
                plan_subtree);
        EXPECT_EQ(
            std::string_view(recomputed),
            std::string_view(recomputed_via_view))
            << "view-bound and value-passed recompute diverge";
    }
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

// ===========================================================================
// 预备 pass 全链 E2E：begin/end 节点 settings → Core 边界会话（create 出
// 口）GetDocument → faces 组入图会话创建 context（nodeFaces 键）→ 编译 →
// begin/end 编译口面与边界签名（DeriveBoundaryPorts 输出）=文档口面。
// faces 是唯一动态口源；方向展开双侧对语义保留为推导规则（begin 输出
// 侧/end 输入侧由边界排除规则挡下）。
// ===========================================================================

namespace
{
    constexpr std::string_view kBoundaryBeginGuid =
        Das::Core::GraphRuntime::GRAPH_BEGIN_COMPONENT_GUID;
    constexpr std::string_view kBoundaryEndGuid =
        Das::Core::GraphRuntime::GRAPH_END_COMPONENT_GUID;

    Das::DasPtr<Das::PluginInterface::IDasTaskAuthoringSession>
    CreateBoundarySession(
        std::string_view   guid_text,
        const std::string& context)
    {
        const DasGuid guid =
            Das::Core::ForeignInterfaceHost::MakeDasGuid(guid_text);
        Das::DasPtr<IDasReadOnlyString> context_string;
        CreateIDasReadOnlyStringFromUtf8(context.c_str(), context_string.Put());
        Das::DasPtr<Das::PluginInterface::IDasTaskAuthoringSession> session;
        CreateGraphBoundaryAuthoringSession(
            &guid,
            context_string.Get(),
            session.Put());
        return session;
    }

    // GetDocument envelope（{document, diagnostics}）的 document 值。
    yyjson::value BoundaryDocumentOf(
        Das::PluginInterface::IDasTaskAuthoringSession* session)
    {
        Das::DasPtr<Das::ExportInterface::IDasJson> out;
        session->GetDocument(nullptr, out.Put());
        auto root = yyjson::read(JsonText(out.Get()));
        auto obj = root.as_object();
        if (!obj || !obj->contains(std::string_view("document")))
        {
            return Das::Utils::MakeYyjsonObject();
        }
        return Das::Utils::CloneYyjsonValue(
            (*obj)[std::string_view("document")]);
    }

    // 文档根 ports section（faces wire 形状 [{id,type,direction}]）的序列
    // 化文本——预备 pass 以此形状消费 GetDocument 投影。
    std::string PortsSectionText(const yyjson::value& document)
    {
        auto obj = document.as_object();
        if (!obj || !obj->contains(std::string_view("ports")))
        {
            return "[]";
        }
        const auto text =
            Das::Utils::SerializeYyjsonValue((*obj)[std::string_view("ports")]);
        return text.has_value() ? *text : std::string("[]");
    }

    // boundary 节点 JSON（pluginGuid 必带——ComponentRefDto 聚合 caster 对
    // 缺席键抛出，节点会被逐节点 try/catch 静默跳过）。
    std::string BoundaryNodeText(
        std::string_view   node_id,
        std::string_view   guid_text,
        const std::string& settings_boundary_ports)
    {
        return std::string(R"({"nodeId":")") + std::string(node_id)
               + R"(","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":")"
               + std::string(guid_text) + R"(","pluginGuid":")"
               + std::string(Das::Core::GraphRuntime::GRAPH_TASK_PLUGIN_GUID)
               + R"("}},"settings":{"boundaryPorts":)" + settings_boundary_ports
               + "}}";
    }

    // 模拟预备 pass 组装的全链 context：图文档（begin/end 节点携带
    // settings.boundaryPorts）+ nodeFaces 键（边界会话 GetDocument 的 ports
    // section 逐字组入）。
    std::string MakeFacesChainContext(
        const std::string& begin_settings_ports,
        const std::string& end_settings_ports,
        const std::string& begin_faces_text,
        const std::string& end_faces_text)
    {
        return std::string(
                   R"({"documentId":"faces_chain","version":3,"nodes":[)")
               + BoundaryNodeText(
                   "n_0",
                   kBoundaryBeginGuid,
                   begin_settings_ports)
               + ","
               + BoundaryNodeText("n_1", kBoundaryEndGuid, end_settings_ports)
               + R"(],"edges":[],"nodeFaces":[{"nodeId":"n_0","ports":)"
               + begin_faces_text + R"(},{"nodeId":"n_1","ports":)"
               + end_faces_text + "}]}";
    }

    Das::Core::GraphRuntime::Dto::CompiledGraphPlanDto PlanOfContext(
        const std::string& context)
    {
        const AuthoringSessionHandle session{context};
        auto compiled = CompileSessionPlan(session.state);
        auto root = yyjson::read(JsonText(compiled.Get())).as_object();
        auto compile_section = (*root)[std::string_view("compile")].as_object();
        auto shell =
            (*compile_section)[std::string_view("executionInput")].as_object();
        return yyjson::cast<Das::Core::GraphRuntime::Dto::CompiledGraphPlanDto>(
            (*shell)[std::string_view("plan")]);
    }
} // namespace

TEST(GraphRuntimeFactoryTest, BoundaryFacesFullChainDrivesBoundarySignature)
{
    // ① 以节点 settings 建 Core 边界会话——begin 声明两口、end 一口。
    const auto begin_session = CreateBoundarySession(
        kBoundaryBeginGuid,
        R"({"revision":5,"boundaryPorts":[)"
        R"({"id":"p_1","type":"string"},{"id":"p_2","type":"int"}]})");
    ASSERT_NE(begin_session.Get(), nullptr);
    const auto end_session = CreateBoundarySession(
        kBoundaryEndGuid,
        R"({"revision":7,"boundaryPorts":[{"id":"q_1","type":"image"}]})");
    ASSERT_NE(end_session.Get(), nullptr);

    // ② GetDocument 的 ports section（faces wire 形状）作为 nodeFaces 键
    // 的组入源——方向由角色固化（begin 恒 input / end 恒 output）。
    const std::string begin_faces =
        PortsSectionText(BoundaryDocumentOf(begin_session.Get()));
    const std::string end_faces =
        PortsSectionText(BoundaryDocumentOf(end_session.Get()));
    EXPECT_NE(begin_faces.find(R"("id":"p_1")"), std::string::npos);
    EXPECT_NE(begin_faces.find(R"("direction":"input")"), std::string::npos);
    EXPECT_NE(end_faces.find(R"("direction":"output")"), std::string::npos);

    // ③ 编译——边界签名=文档口面：begin faces 全量进 inputs，end faces
    // 全量进 outputs（口对展开的镜像侧由推导规则排除）。
    const auto plan = PlanOfContext(MakeFacesChainContext(
        R"([{"id":"p_1","type":"string"},{"id":"p_2","type":"int"}])",
        R"([{"id":"q_1","type":"image"}])",
        begin_faces,
        end_faces));

    ASSERT_EQ(plan.boundary_ports.inputs.size(), 2u);
    EXPECT_EQ(plan.boundary_ports.inputs[0].port_id, "p_1");
    EXPECT_EQ(plan.boundary_ports.inputs[0].port_type, "string");
    EXPECT_EQ(plan.boundary_ports.inputs[1].port_id, "p_2");
    EXPECT_EQ(plan.boundary_ports.inputs[1].port_type, "int");
    ASSERT_EQ(plan.boundary_ports.inputs[0].node_ids.size(), 1u);
    EXPECT_EQ(plan.boundary_ports.inputs[0].node_ids[0], "n_0");

    ASSERT_EQ(plan.boundary_ports.outputs.size(), 1u);
    EXPECT_EQ(plan.boundary_ports.outputs[0].port_id, "q_1");
    EXPECT_EQ(plan.boundary_ports.outputs[0].port_type, "image");
    ASSERT_EQ(plan.boundary_ports.outputs[0].node_ids.size(), 1u);
    EXPECT_EQ(plan.boundary_ports.outputs[0].node_ids[0], "n_1");

    // 广播绑定跟随 faces：begin 的悬空口各得一条 $graph_input 绑定。
    std::size_t broadcast_bindings = 0;
    for (const auto& binding : plan.binding_plan.bindings)
    {
        if (binding.source_node_id == "$graph_input")
        {
            ++broadcast_bindings;
        }
    }
    EXPECT_EQ(broadcast_bindings, 2u);
}

TEST(GraphRuntimeFactoryTest, BoundaryFacesAbsentLeavesBoundaryEmpty)
{
    // 无 nodeFaces 注入（编译宿主未跑预备 pass）：begin/end 边界贡献为空
    // ——不回退任何分区/manifest 通道（faces 是唯一动态口源）。
    const auto plan = PlanOfContext(
        std::string(R"({"documentId":"no_faces","nodes":[)")
        + BoundaryNodeText(
            "n_0",
            kBoundaryBeginGuid,
            R"([{"id":"p_1","type":"string"}])")
        + "," + BoundaryNodeText("n_1", kBoundaryEndGuid, R"([])")
        + R"(],"edges":[]})");
    EXPECT_TRUE(plan.boundary_ports.inputs.empty());
    EXPECT_TRUE(plan.boundary_ports.outputs.empty());
}

TEST(GraphRuntimeFactoryTest, CatalogDeclaredPortsRemainOnCompileSurface)
{
    // provider 闭包退守纯声明面不误删（Pitfall 3）：纯 manifest 组件的
    // catalog 声明口仍在编译口面——两 launch 形态节点间的声明口连线编译
    // 零诊断（口不解析即 PortNotFound）。
    const std::string launch_guid = "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA";
    const auto        node_of = [&launch_guid](std::string_view node_id)
    {
        return std::string(R"({"nodeId":")") + std::string(node_id)
               + R"(","target":{"targetKind":"componentRef","componentRef":{"kind":"componentRef","componentGuid":")"
               + launch_guid
               + R"(","pluginGuid":"BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB"}},)"
               + R"("settings":{}})";
    };
    const std::string context =
        std::string(R"({"documentId":"catalog_face","nodes":[)")
        + BoundaryNodeText("n_0", kBoundaryBeginGuid, R"([])") + ","
        + BoundaryNodeText("n_1", kBoundaryEndGuid, R"([])") + ","
        + node_of("n_a") + "," + node_of("n_b")
        + R"(],"edges":[{"edgeId":"e1","sourceNodeId":"n_a","sourcePortId":"out","targetNodeId":"n_b","targetPortId":"in","edgeType":"data"}],)"
        + R"("actionCatalog":[{"actionId":")" + launch_guid
        + R"(","label":"das.utils.launch","ports":[)"
        + R"({"portId":"in","portType":"int","direction":"input"},)"
        + R"({"portId":"out","portType":"int","direction":"output"}]}]})";

    const AuthoringSessionHandle session{context};
    auto                         compiled = CompileSessionPlan(session.state);
    ASSERT_NE(compiled.Get(), nullptr);
    auto root = yyjson::read(JsonText(compiled.Get())).as_object();
    ASSERT_TRUE(root.has_value());
    ASSERT_TRUE(root->contains(std::string_view("diagnostics")));
    const auto diagnostics =
        (*root)[std::string_view("diagnostics")].as_array();
    ASSERT_TRUE(diagnostics.has_value());
    for (const auto& diagnostic : *diagnostics)
    {
        const auto text = Das::Utils::SerializeYyjsonValue(diagnostic);
        ADD_FAILURE() << "unexpected diagnostic: "
                      << (text.has_value() ? *text : std::string("<null>"));
    }
}

TEST(GraphRuntimeFactoryTest, EntryFacesTypesFlowIntoTypesRegistry)
{
    // faces 通道携带的文档 types[]（entryFaces 条目键）随会话状态进图视
    // 图投影的 types 注册表——内建九词 ∪ 自定义名（消费面：连线规则表
    // 的 Core 侧供源）。
    const std::string context =
        std::string(R"({"documentId":"types_doc","nodes":[)")
        + BoundaryNodeText("n_0", kBoundaryBeginGuid, R"([])") + ","
        + BoundaryNodeText("n_1", kBoundaryEndGuid, R"([])")
        + R"(],"edges":[],"entryFaces":[)"
        + R"({"entryId":7,"ports":[],"types":[{"name":"myType","compatibleWith":["int"]}]}]})";

    const AuthoringSessionHandle                session{context};
    Das::DasPtr<Das::ExportInterface::IDasJson> document;
    ASSERT_EQ(
        GraphAuthoringSessionGetDocument(
            session.state,
            nullptr,
            document.Put()),
        DAS_S_OK);
    ASSERT_NE(document.Get(), nullptr);
    auto root = yyjson::read(JsonText(document.Get())).as_object();
    ASSERT_TRUE(root.has_value());
    ASSERT_TRUE(root->contains(std::string_view("document")));
    auto doc_obj = (*root)[std::string_view("document")].as_object();
    ASSERT_TRUE(doc_obj.has_value());
    ASSERT_TRUE(doc_obj->contains(std::string_view("types")));
    auto types = (*doc_obj)[std::string_view("types")].as_array();
    ASSERT_TRUE(types.has_value());
    EXPECT_EQ(types->size(), 10u); // 九词 + myType
    bool custom_found = false;
    for (const auto& entry : *types)
    {
        auto entry_obj = entry.as_object();
        ASSERT_TRUE(entry_obj.has_value());
        auto name = (*entry_obj)[std::string_view("name")].as_string();
        ASSERT_TRUE(name.has_value());
        if (*name == "myType")
        {
            custom_found = true;
            auto compat =
                (*entry_obj)[std::string_view("compatibleWith")].as_array();
            ASSERT_TRUE(compat.has_value());
            ASSERT_EQ(compat->size(), 1u);
            EXPECT_EQ((*compat)[0].as_string().value_or(""), "int");
        }
    }
    EXPECT_TRUE(custom_found);
}

TEST(GraphRuntimeFactoryTest, GraphViewProjectsDerivedBoundaryPortsSection)
{
    // 图条目文档自述口面：faces 注入时图视图投影根 ports section=推导
    // 边界（begin faces→direction=input / end faces→direction=output——
    // 推导层的边界排除规则挡下口对镜像侧），形状=根 ports section 契约
    // （{id,type[,label],direction}），且投影文档过调度侧文档校验器
    // （三层 lockstep 的 Core 半边——被调面文档恒合法）。
    const std::string context =
        std::string(R"({"documentId":"ports_projection","version":3,"nodes":[)")
        + BoundaryNodeText(
            "n_0",
            kBoundaryBeginGuid,
            R"([{"id":"p_1","type":"string"},{"id":"p_2","type":"int"}])")
        + ","
        + BoundaryNodeText(
            "n_1",
            kBoundaryEndGuid,
            R"([{"id":"q_1","type":"image"}])")
        + R"(],"edges":[],"nodeFaces":[)"
        + R"({"nodeId":"n_0","ports":[{"id":"p_1","type":"string","direction":"input"},{"id":"p_2","type":"int","direction":"input"}]},)"
        + R"({"nodeId":"n_1","ports":[{"id":"q_1","type":"image","direction":"output"}]}]})";

    const AuthoringSessionHandle                session{context};
    Das::DasPtr<Das::ExportInterface::IDasJson> document;
    ASSERT_EQ(
        GraphAuthoringSessionGetDocument(
            session.state,
            nullptr,
            document.Put()),
        DAS_S_OK);
    ASSERT_NE(document.Get(), nullptr);
    auto root = yyjson::read(JsonText(document.Get())).as_object();
    ASSERT_TRUE(root.has_value());
    ASSERT_TRUE(root->contains(std::string_view("document")));
    auto doc_obj = (*root)[std::string_view("document")].as_object();
    ASSERT_TRUE(doc_obj.has_value());

    ASSERT_TRUE(doc_obj->contains(std::string_view("ports")));
    auto ports = (*doc_obj)[std::string_view("ports")].as_array();
    ASSERT_TRUE(ports.has_value());
    ASSERT_EQ(ports->size(), 3u);
    const auto port_of = [&ports](std::size_t index)
    { return (*ports)[index].as_object(); };
    ASSERT_TRUE(port_of(0).has_value());
    EXPECT_EQ(
        (*port_of(0))[std::string_view("id")].as_string().value_or(""),
        "p_1");
    EXPECT_EQ(
        (*port_of(0))[std::string_view("type")].as_string().value_or(""),
        "string");
    EXPECT_EQ(
        (*port_of(0))[std::string_view("direction")].as_string().value_or(""),
        "input");
    ASSERT_TRUE(port_of(1).has_value());
    EXPECT_EQ(
        (*port_of(1))[std::string_view("id")].as_string().value_or(""),
        "p_2");
    EXPECT_EQ(
        (*port_of(1))[std::string_view("type")].as_string().value_or(""),
        "int");
    EXPECT_EQ(
        (*port_of(1))[std::string_view("direction")].as_string().value_or(""),
        "input");
    ASSERT_TRUE(port_of(2).has_value());
    EXPECT_EQ(
        (*port_of(2))[std::string_view("id")].as_string().value_or(""),
        "q_1");
    EXPECT_EQ(
        (*port_of(2))[std::string_view("type")].as_string().value_or(""),
        "image");
    EXPECT_EQ(
        (*port_of(2))[std::string_view("direction")].as_string().value_or(""),
        "output");

    // 投影文档过调度侧严格校验器（根 ports section 契约形状）。
    const auto validation = Das::Core::TaskScheduler::ValidateAuthoringDocument(
        Das::Utils::CloneYyjsonValue((*root)[std::string_view("document")]));
    EXPECT_TRUE(validation.valid)
        << validation.error_kind << ": " << validation.message;
}

TEST(GraphRuntimeFactoryTest, GraphViewPortsSectionDuplicateIdRejected)
{
    // 跨侧重名零兜底：begin/end faces 携带同名口 id（手改名的存量数据或
    // 组件会话不规范产出）时，发射端照实双双发射（无先到先得跳过垫子）
    // ——撞上根 ports section 契约的 id 同文档唯一律时，消费侧校验器整
    // 文档拒绝（duplicatePortId，与 PortsSectionDuplicateIdRejected 同一
    // 法律路径）；历史撞名数据由用户按报错手动改名。
    const std::string context =
        std::string(R"({"documentId":"ports_dup","version":3,"nodes":[)")
        + BoundaryNodeText(
            "n_0",
            kBoundaryBeginGuid,
            R"([{"id":"dup","type":"string"}])")
        + ","
        + BoundaryNodeText(
            "n_1",
            kBoundaryEndGuid,
            R"([{"id":"dup","type":"int"}])")
        + R"(],"edges":[],"nodeFaces":[)"
        + R"({"nodeId":"n_0","ports":[{"id":"dup","type":"string","direction":"input"}]},)"
        + R"({"nodeId":"n_1","ports":[{"id":"dup","type":"int","direction":"output"}]}]})";

    const AuthoringSessionHandle                session{context};
    Das::DasPtr<Das::ExportInterface::IDasJson> document;
    ASSERT_EQ(
        GraphAuthoringSessionGetDocument(
            session.state,
            nullptr,
            document.Put()),
        DAS_S_OK);
    ASSERT_NE(document.Get(), nullptr);
    auto root = yyjson::read(JsonText(document.Get())).as_object();
    ASSERT_TRUE(root.has_value());
    ASSERT_TRUE(root->contains(std::string_view("document")));
    auto doc_obj = (*root)[std::string_view("document")].as_object();
    ASSERT_TRUE(doc_obj.has_value());

    // 发射端零垫子：两个同名条目都在（先到先得跳过已废除）。
    ASSERT_TRUE(doc_obj->contains(std::string_view("ports")));
    auto ports = (*doc_obj)[std::string_view("ports")].as_array();
    ASSERT_TRUE(ports.has_value());
    EXPECT_EQ(ports->size(), 2u)
        << "duplicate port ids must both be emitted (no skip cushion)";
    std::size_t dup_count = 0;
    for (const auto& port : *ports)
    {
        auto port_obj = port.as_object();
        ASSERT_TRUE(port_obj.has_value());
        if ((*port_obj)[std::string_view("id")].as_string().value_or("")
            == "dup")
        {
            ++dup_count;
        }
    }
    ASSERT_EQ(dup_count, 2u);

    // 重名→消费侧整文档拒绝（根 ports section 契约 id 同文档唯一）。
    const auto validation = Das::Core::TaskScheduler::ValidateAuthoringDocument(
        Das::Utils::CloneYyjsonValue((*root)[std::string_view("document")]));
    EXPECT_FALSE(validation.valid);
    EXPECT_EQ(validation.error_kind, "duplicatePortId")
        << validation.error_kind << ": " << validation.message;
}

TEST(GraphRuntimeFactoryTest, GraphViewOmitsPortsSectionWithoutFaces)
{
    // faces 缺席（standalone 打开、无预备 pass 注入）：根 ports section
    // 缺省——engaged-only，与现状投影逐字节一致（types 注册表照投——
    // 既有先例，不受影响）。
    const std::string context =
        std::string(R"({"documentId":"ports_standalone","version":3,"nodes":[)")
        + BoundaryNodeText(
            "n_0",
            kBoundaryBeginGuid,
            R"([{"id":"p_1","type":"string"}])")
        + ","
        + BoundaryNodeText(
            "n_1",
            kBoundaryEndGuid,
            R"([{"id":"q_1","type":"int"}])")
        + R"(],"edges":[]})";

    const AuthoringSessionHandle                session{context};
    Das::DasPtr<Das::ExportInterface::IDasJson> document;
    ASSERT_EQ(
        GraphAuthoringSessionGetDocument(
            session.state,
            nullptr,
            document.Put()),
        DAS_S_OK);
    ASSERT_NE(document.Get(), nullptr);
    auto root = yyjson::read(JsonText(document.Get())).as_object();
    ASSERT_TRUE(root.has_value());
    ASSERT_TRUE(root->contains(std::string_view("document")));
    auto doc_obj = (*root)[std::string_view("document")].as_object();
    ASSERT_TRUE(doc_obj.has_value());
    EXPECT_FALSE(doc_obj->contains(std::string_view("ports")))
        << "standalone graph document must not carry a root ports section";
    EXPECT_TRUE(doc_obj->contains(std::string_view("types")));
}

TEST(GraphRuntimeFactoryTest, GraphViewPortsSectionMatchesDerivedBoundary)
{
    // 同源断言：投影的根 ports section 与 DeriveBoundaryPorts 编译输出一
    // 致（同一函数两用——投影消费与编译消费永不漂移，测试锁同源）。
    const std::string context =
        std::string(
            R"({"documentId":"ports_same_source","version":3,"nodes":[)")
        + BoundaryNodeText(
            "n_0",
            kBoundaryBeginGuid,
            R"([{"id":"p_1","type":"string"},{"id":"p_2","type":"int"}])")
        + ","
        + BoundaryNodeText(
            "n_1",
            kBoundaryEndGuid,
            R"([{"id":"q_1","type":"image"}])")
        + R"(],"edges":[],"nodeFaces":[)"
        + R"({"nodeId":"n_0","ports":[{"id":"p_1","type":"string","direction":"input"},{"id":"p_2","type":"int","direction":"input"}]},)"
        + R"({"nodeId":"n_1","ports":[{"id":"q_1","type":"image","direction":"output"}]}]})";

    const auto plan = PlanOfContext(context);

    const AuthoringSessionHandle                session{context};
    Das::DasPtr<Das::ExportInterface::IDasJson> document;
    ASSERT_EQ(
        GraphAuthoringSessionGetDocument(
            session.state,
            nullptr,
            document.Put()),
        DAS_S_OK);
    ASSERT_NE(document.Get(), nullptr);
    auto root = yyjson::read(JsonText(document.Get())).as_object();
    ASSERT_TRUE(root.has_value());
    ASSERT_TRUE(root->contains(std::string_view("document")));
    auto doc_obj = (*root)[std::string_view("document")].as_object();
    ASSERT_TRUE(doc_obj.has_value());
    ASSERT_TRUE(doc_obj->contains(std::string_view("ports")));
    auto ports = (*doc_obj)[std::string_view("ports")].as_array();
    ASSERT_TRUE(ports.has_value());

    struct ExpectedPort
    {
        std::string id;
        std::string type;
        std::string direction;
    };
    std::vector<ExpectedPort> expected;
    for (const auto& port : plan.boundary_ports.inputs)
    {
        expected.push_back({port.port_id, port.port_type, "input"});
    }
    for (const auto& port : plan.boundary_ports.outputs)
    {
        expected.push_back({port.port_id, port.port_type, "output"});
    }
    ASSERT_EQ(ports->size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        auto port_obj = (*ports)[index].as_object();
        ASSERT_TRUE(port_obj.has_value());
        EXPECT_EQ(
            (*port_obj)[std::string_view("id")].as_string().value_or(""),
            expected[index].id)
            << "port " << index << " id diverges from the derivation";
        EXPECT_EQ(
            (*port_obj)[std::string_view("type")].as_string().value_or(""),
            expected[index].type)
            << "port " << index << " type diverges from the derivation";
        EXPECT_EQ(
            (*port_obj)[std::string_view("direction")].as_string().value_or(""),
            expected[index].direction)
            << "port " << index << " direction diverges from the derivation";
    }
}
