#include "../src/MaapiRunTaskComponent.h"
#include "FakeMaaApiBoundary.h"

#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/GraphRuntime.h>
#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/DasString.hpp>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentCreator.Implements.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <string_view>
#include <utility>

namespace
{
    using namespace Das;
    using namespace Das::Core::GraphRuntime;
    using namespace Das::Core::GraphRuntime::Dto;
    using namespace Das::Plugins::DasMaaPi;
    using namespace Das::Plugins::DasMaaPi::Test;

    using IDasTaskComponent = PluginInterface::IDasTaskComponent;
    using PluginInterface::IDasTaskAuthoringSession;

    // ---- Constants ----

    const std::string MAAPI_RUN_TASK_GUID =
        "69F20008-0000-4000-8000-000000000001";
    const std::string NODE_ID = "30000000-0000-0000-0000-000000000001";

    // ---- Mock StopToken ----

    class MockStopToken final
        : public PluginInterface::DasStopTokenImplBase<MockStopToken>
    {
    public:
        DasResult DAS_STD_CALL StopRequested(bool* canStop) override
        {
            if (!canStop)
            {
                return DAS_E_INVALID_POINTER;
            }
            *canStop = false;
            return DAS_S_OK;
        }
    };

    // ---- ScopedBoundaryHook ----

    class ScopedBoundaryHook final
    {
    public:
        explicit ScopedBoundaryHook(FakeMaaApiBoundary& boundary)
        {
            SetMaaApiBoundaryForTest(&boundary);
        }

        ~ScopedBoundaryHook() { SetMaaApiBoundaryForTest(nullptr); }
    };

    // ---- Plan builders ----

    GraphPortDefinitionDto MakePortDef(
        const std::string& port_id,
        const std::string& port_type = "string")
    {
        GraphPortDefinitionDto p;
        p.port_id = port_id;
        p.port_type = port_type;
        return p;
    }

    // envelope fixture 直接构造 ExecutionEnvelopeDto 并序列化——cpp_yyjson
    // 的 default_caster 经 PFR 对每个字段调 obj[key]，缺 key（如
    // focusTranslationsPath/schedule 即便为空也必须显式存在）在 cast 前抛
    // std::out_of_range，手搓 JSON 字符串极易漏键。
    yyjson::value MakeEnvelopeValue()
    {
        ExecutionEnvelopeDto envelope;
        envelope.maapi.interface_directory = "C:/maa";
        envelope.maapi.controller_name = "Android";
        envelope.maapi.controller.name = "Android";
        envelope.maapi.controller.type = "Adb";
        envelope.maapi.controller.address = "127.0.0.1:5555";

        AdbInitParams adb;
        adb.address = "127.0.0.1:5555";
        ControllerInitParams controller_init_params;
        controller_init_params.controller_type = "Adb";
        controller_init_params.adb = std::move(adb);
        envelope.maapi.controller_init_params =
            std::move(controller_init_params);

        envelope.maapi.resource_name = "Official";
        envelope.maapi.resource_paths = {"C:/maa/resource"};
        // 与 FakeMaaApiBoundary::GetResourceHash 的默认返回一致，避免
        // resource hash mismatch 警告诊断混入断言面。
        envelope.maapi.resource_hash = "hash-expected";

        MaaTaskExecutionDto task;
        task.task_name = "E2ETestTask";
        task.entry = "StartE2E";
        task.pipeline_override = Utils::MakeYyjsonObject();
        envelope.maapi.tasks.push_back(std::move(task));

        return Utils::CloneYyjsonValue(yyjson::object(envelope));
    }

    // 编译产物串：被引 maapi 条目的 compile 段 executionInput 形态=
    // envelope 原文（maapi.run 的产物通道——无 {fingerprint, plan} 外壳）。
    std::string MakeEnvelopeArtifactText()
    {
        const auto serialized = Utils::SerializeYyjsonValue(
            Utils::CloneYyjsonValue(MakeEnvelopeValue()));
        return serialized.value_or("{}");
    }

    // 被引条目 Compile 响应：{compile:{executionInput:<产物串>},
    // diagnostics:[]}。
    std::string MakeEntryCompileResponse(const std::string& artifact_text)
    {
        yyjson::value compile_response(Das::Utils::MakeYyjsonObject());
        auto          response_obj = compile_response.as_object();
        yyjson::value compile_section(Das::Utils::MakeYyjsonObject());
        (*compile_section.as_object())[std::string_view("executionInput")] =
            yyjson::value(std::string(artifact_text));
        (*response_obj)[std::string_view("compile")] =
            std::move(compile_section);
        (*response_obj)[std::string_view("diagnostics")] =
            Das::Utils::MakeYyjsonArray();
        return Das::Utils::SerializeYyjsonValue(
                   Das::Utils::CloneYyjsonValue(compile_response))
            .value_or("");
    }

    // 执行时编译链计划：单 maapi.run 节点 + 绑条目记档（node→entryId——
    // 执行时编译段的驱动数据）。产物零寄存——envelope 由运行时在节点执行
    // 期间经 creator 现编写进 executionInput 协议保留键。
    CompiledGraphPlanDto MakeSingleMaapiRunNodePlan()
    {
        CompiledGraphPlanDto plan;
        plan.source_fingerprint = "fp_e2e";

        CompiledNodeSnapshotDto snap;
        snap.node_id = NODE_ID;
        snap.component_guid = MAAPI_RUN_TASK_GUID;
        snap.resolved_ports = {
            MakePortDef("executionInput", "object"),
            MakePortDef("status", "string"),
            MakePortDef("completedTasks", "array<string>"),
            MakePortDef("stopped", "bool"),
            MakePortDef("diagnostics", "array<object>"),
        };
        plan.node_snapshots.push_back(std::move(snap));
        plan.execution_order.push_back(NODE_ID);

        plan.reference_entries.push_back(ReferenceEntryDto{NODE_ID, 42});

        return plan;
    }

    // ---- worker 模式注入链 E2E（进程内形态，D-13 验收）----

    // 预置边界节点（begin/end）的直通组件替身：RunWithHost 的实例化全部
    // 经传入 creator——母本 ReferenceChainCreatorHost 同款「三 guid 通吃」
    // 形态（无输出口，Do 恒成功）。
    class WorkerChainPassthroughComponent final : public IDasTaskComponent
    {
    public:
        explicit WorkerChainPassthroughComponent(DasGuid guid) : guid_(guid) {}

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
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult GetGuid(DasGuid* p_out_guid) override
        {
            if (p_out_guid == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_out_guid = guid_;
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            return CreateIDasReadOnlyStringFromUtf8(
                "WorkerChainPassthroughComponent",
                pp_out_name);
        }

        DasResult ApplySettingsChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json != nullptr)
            {
                *pp_out_result_json = nullptr;
            }
            return DAS_S_OK;
        }

        DasResult Do(
            PluginInterface::IDasStopToken*,
            Das::ExportInterface::IDasReadOnlyPortMap*,
            Das::ExportInterface::IDasPortMap** pp_out_port_map) override
        {
            if (pp_out_port_map == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_port_map = nullptr;
            return DAS_S_OK;
        }

    private:
        DasGuid               guid_;
        std::atomic<uint32_t> ref_count_{0};
    };

    // 固定响应的 authoring 会话替身（worker 链编译腿的后端产物）：文档=
    // 空 ports 自述，Compile=被引条目的 {compile:{executionInput}} 响应。
    // 手写实现（照 E2EGraphFlowTest 的 ReferenceChainFakeSession 先例）。
    class WorkerChainFakeSession final : public IDasTaskAuthoringSession
    {
    public:
        WorkerChainFakeSession(
            std::string document_json,
            std::string compile_json)
            : document_json_(std::move(document_json)),
              compile_json_(std::move(compile_json))
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
            if (iid == DasIidOf<IDasTaskAuthoringSession>())
            {
                *pp_out = static_cast<IDasTaskAuthoringSession*>(this);
                AddRef();
                return DAS_S_OK;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult GetGuid(DasGuid* p_out_guid) override
        {
            if (p_out_guid == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_out_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                MAAPI_RUN_TASK_GUID);
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            return CreateIDasReadOnlyStringFromUtf8(
                "WorkerChainFakeSession",
                pp_out_name);
        }

        DasResult GetDocument(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_document_json) override
        {
            if (pp_out_document_json == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_document_json =
                new Das::Core::Utils::IDasJsonImpl(document_json_.c_str());
            (*pp_out_document_json)->AddRef();
            return DAS_S_OK;
        }

        DasResult ApplyChange(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json != nullptr)
            {
                *pp_out_result_json = nullptr;
            }
            return DAS_E_NO_IMPLEMENTATION;
        }

        DasResult Compile(
            Das::ExportInterface::IDasJson*,
            Das::ExportInterface::IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_result_json =
                new Das::Core::Utils::IDasJsonImpl(compile_json_.c_str());
            (*pp_out_result_json)->AddRef();
            return DAS_S_OK;
        }

    private:
        std::string           document_json_;
        std::string           compile_json_;
        std::atomic<uint32_t> ref_count_{0};
    };

    // ---- MaapiMockHost ----
    // maapi run 组件为被测对象本体（图内直接消费 executionInput 端口喂线
    // 的 envelope——执行时编译链的产物投递形态，无中间人组件）。被引条
    // 目的 compile 响应由固定 fake 会话承载（host mock 链为指定载体）。
    class MaapiMockHost final
        : public PluginInterface::DasTaskComponentCreatorImplBase<MaapiMockHost>
    {
    public:
        explicit MaapiMockHost(std::string compile_json)
            : compile_json_(std::move(compile_json))
        {
        }

        DasResult CreateTaskComponent(
            const DasGuid&      component_guid,
            IDasTaskComponent** pp_out_component) override
        {
            if (!pp_out_component)
            {
                return DAS_E_INVALID_POINTER;
            }

            auto guid_str = DasGuidToString(component_guid);

            if (guid_str.GetUtf8() == MAAPI_RUN_TASK_GUID)
            {
                auto* component = new MaapiRunTaskComponent();
                component->AddRef();
                *pp_out_component = component;
                return DAS_S_OK;
            }

            *pp_out_component = nullptr;
            return DAS_E_NOT_FOUND;
        }

        DasResult CreateAuthoringSession(
            const DasGuid&                              task_guid,
            Das::ExportInterface::IDasJson*             p_context_json,
            PluginInterface::IDasTaskAuthoringSession** pp_out_session) override
        {
            (void)task_guid;
            (void)p_context_json;
            if (!pp_out_session)
            {
                return DAS_E_INVALID_POINTER;
            }
            // 根 ports 自述空口面（被引条目文档根 ports 自述——消费文档
            // 自述的口面通道）。
            auto* session = new WorkerChainFakeSession(
                R"({"document":{"viewKind":"graph","ports":[]},"diagnostics":[]})",
                compile_json_);
            session->AddRef();
            *pp_out_session = session;
            return DAS_S_OK;
        }

    private:
        std::string compile_json_;
    };

    // worker 链 creator：CreateTaskComponent 造真实 MaapiRunTaskComponent
    // （worker 侧组件的进程内同构替身），CreateAuthoringSession 造固定
    // 响应会话（生产中经后端 session resolver 到远端 authoring factory）。
    class WorkerChainCreatorHost final
        : public PluginInterface::DasTaskComponentCreatorImplBase<
              WorkerChainCreatorHost>
    {
    public:
        explicit WorkerChainCreatorHost(std::string compile_json)
            : compile_json_(std::move(compile_json))
        {
        }

        DasResult CreateTaskComponent(
            const DasGuid&      component_guid,
            IDasTaskComponent** pp_out_component) override
        {
            if (!pp_out_component)
            {
                return DAS_E_INVALID_POINTER;
            }

            auto guid_str = DasGuidToString(component_guid);

            if (guid_str.GetUtf8() == MAAPI_RUN_TASK_GUID)
            {
                auto* component = new MaapiRunTaskComponent();
                component->AddRef();
                *pp_out_component = component;
                return DAS_S_OK;
            }

            // 预置 begin/end 边界节点：直通组件（无输出口，Do 恒成功）。
            auto* passthrough =
                new WorkerChainPassthroughComponent(component_guid);
            passthrough->AddRef();
            *pp_out_component = passthrough;
            return DAS_S_OK;
        }

        DasResult CreateAuthoringSession(
            const DasGuid&                              task_guid,
            Das::ExportInterface::IDasJson*             p_context_json,
            PluginInterface::IDasTaskAuthoringSession** pp_out_session) override
        {
            (void)task_guid;
            (void)p_context_json;
            if (!pp_out_session)
            {
                return DAS_E_INVALID_POINTER;
            }
            // 根 ports 自述空口面（被引条目文档根 ports 自述——消费文档
            // 自述的口面通道）。
            auto* session = new WorkerChainFakeSession(
                R"({"document":{"viewKind":"graph","ports":[]},"diagnostics":[]})",
                compile_json_);
            session->AddRef();
            *pp_out_session = session;
            return DAS_S_OK;
        }

    private:
        std::string compile_json_;
    };

    Das::DasPtr<IDasReadOnlyString> WorkerChainMakeText(const std::string& text)
    {
        Das::DasPtr<IDasReadOnlyString> ro;
        CreateIDasReadOnlyStringFromUtf8(text.c_str(), ro.Put());
        return ro;
    }

    std::string WorkerChainJsonText(Das::ExportInterface::IDasJson* json)
    {
        if (json == nullptr)
        {
            return {};
        }
        Das::DasPtr<IDasReadOnlyString> text;
        if (DAS::IsFailed(json->ToString(0, text.Put())) || !text)
        {
            return {};
        }
        const char* utf8 = nullptr;
        if (DAS::IsFailed(text->GetUtf8(&utf8)) || utf8 == nullptr)
        {
            return {};
        }
        return utf8;
    }

    // worker 模式注入链的编译腿：真实 Core 会话（C ABI）+ 绑条目
    // maapi.run 节点（真实 authoring 变更流）+ 注入 pass 编译。成功时
    // out_plan 携带绑条目记档（referenceEntries——执行时编译段的驱动数
    // 据）且产物零寄存。返回 false 时失败细节已经 EXPECT 输出，out 参数
    // 无效。
    bool CompileBoundMaapiRunNodePlan(
        const std::string& envelope_text,
        Das::DasPtr<Das::PluginInterface::IDasTaskComponentCreator>&
                              out_creator,
        CompiledGraphPlanDto& out_plan,
        std::string&          out_fingerprint)
    {
        // 被引条目 Compile 响应：{compile:{executionInput:<envelope 串>},
        // diagnostics:[]}——快照随编译产物传递（envelope 原文直写）。
        std::string referenced_compile_json;
        {
            yyjson::value compile_response(Das::Utils::MakeYyjsonObject());
            auto          response_obj = compile_response.as_object();
            yyjson::value compile_section(Das::Utils::MakeYyjsonObject());
            (*compile_section.as_object())[std::string_view("executionInput")] =
                yyjson::value(std::string(envelope_text));
            (*response_obj)[std::string_view("compile")] =
                std::move(compile_section);
            (*response_obj)[std::string_view("diagnostics")] =
                Das::Utils::MakeYyjsonArray();
            referenced_compile_json =
                Das::Utils::SerializeYyjsonValue(
                    Das::Utils::CloneYyjsonValue(compile_response))
                    .value_or("");
            if (referenced_compile_json.empty())
            {
                EXPECT_FALSE(referenced_compile_json.empty());
                return false;
            }
        }

        // creator：编译腿（CreateAuthoringSession → 固定响应会话）与执行
        // 腿（CreateTaskComponent → 真实组件）共用同一本尊——被引条目
        // compile 响应即其产物源。
        auto creator = WorkerChainCreatorHost::Make(referenced_compile_json);
        if (!creator)
        {
            EXPECT_TRUE(false) << "WorkerChainCreatorHost::Make failed";
            return false;
        }

        // 会话 context：actionCatalog 声明 maapi.run 组件的
        // taskRepositoryRef 配置字段（repository_fields_ 的数据源——绑定
        // 判定的硬前置）。
        const std::string context =
            std::string(R"({"actionCatalog":[{"actionId":")")
            + MAAPI_RUN_TASK_GUID
            + R"(","ports":[{"portId":"executionInput","portType":"object","direction":"input"}],)"
              R"("config":[{"name":"repositoryRef","type":"taskRepositoryRef","taskGuid":")"
            + MAAPI_RUN_TASK_GUID + R"("}]}]})";

        GraphAuthoringSessionState* session_state = nullptr;
        {
            auto ctx = WorkerChainMakeText(context);
            if (DAS::IsFailed(CreateGraphAuthoringSession(
                    creator.Get(),
                    ctx.Get(),
                    &session_state)))
            {
                EXPECT_TRUE(false) << "CreateGraphAuthoringSession failed";
                return false;
            }
        }
        if (session_state == nullptr)
        {
            EXPECT_NE(session_state, nullptr);
            return false;
        }

        // 建绑条目节点 + 绑定 entryId 42（真实 authoring 变更流）。
        std::string bound_node_id;
        {
            Das::DasPtr<Das::ExportInterface::IDasJson> add_result;
            const auto                                  add_change =
                std::string(
                    R"({"baseRevision":0,"kind":"addGraphNode","payload":{"actionId":")")
                + MAAPI_RUN_TASK_GUID + R"("}})";
            if (DAS::IsFailed(GraphAuthoringSessionApplyChange(
                    session_state,
                    WorkerChainMakeText(add_change).Get(),
                    add_result.Put())))
            {
                EXPECT_TRUE(false) << "addGraphNode failed";
                DestroyGraphAuthoringSession(session_state);
                return false;
            }
            const auto add_text = WorkerChainJsonText(add_result.Get());
            if (add_text.find("\"errorKind\"") != std::string::npos)
            {
                EXPECT_EQ(add_text.find("\"errorKind\""), std::string::npos)
                    << add_text;
                DestroyGraphAuthoringSession(session_state);
                return false;
            }

            // 定位新节点：acceptedProperties（DTO 形状，nodes 顶层）里
            // componentGuid 命中 maapi.run guid 的即目标（预置 begin/end
            // 之外的新增节点）。
            auto add_root = yyjson::read(add_text);
            auto add_obj = add_root.as_object();
            if (!add_obj.has_value()
                || !add_obj->contains(std::string_view("acceptedProperties")))
            {
                EXPECT_TRUE(
                    add_obj.has_value()
                    && add_obj->contains(
                        std::string_view("acceptedProperties")))
                    << add_text;
                DestroyGraphAuthoringSession(session_state);
                return false;
            }
            auto accepted = (*add_obj)[std::string_view("acceptedProperties")];
            auto accepted_obj = accepted.as_object();
            auto nodes = (*accepted_obj)[std::string_view("nodes")].as_array();
            if (!nodes.has_value())
            {
                EXPECT_TRUE(nodes.has_value()) << add_text;
                DestroyGraphAuthoringSession(session_state);
                return false;
            }
            for (const auto& node : *nodes)
            {
                auto serialized = Das::Utils::SerializeYyjsonValue(
                                      Das::Utils::CloneYyjsonValue(node))
                                      .value_or("");
                if (serialized.find(MAAPI_RUN_TASK_GUID) != std::string::npos)
                {
                    auto node_obj = node.as_object();
                    auto node_id =
                        (*node_obj)[std::string_view("nodeId")].as_string();
                    if (!node_id.has_value())
                    {
                        continue;
                    }
                    bound_node_id = *node_id;
                    break;
                }
            }
            if (bound_node_id.empty())
            {
                EXPECT_FALSE(bound_node_id.empty()) << add_text;
                DestroyGraphAuthoringSession(session_state);
                return false;
            }
        }
        {
            Das::DasPtr<Das::ExportInterface::IDasJson> set_result;
            const auto                                  set_change =
                std::string(
                    R"({"baseRevision":1,"kind":"setValue","payload":{"target":{"kind":"graphNode","id":")")
                + bound_node_id
                + R"("},"fieldName":"repositoryRef","value":42}})";
            if (DAS::IsFailed(GraphAuthoringSessionApplyChange(
                    session_state,
                    WorkerChainMakeText(set_change).Get(),
                    set_result.Put())))
            {
                EXPECT_TRUE(false) << "setValue repositoryRef failed";
                DestroyGraphAuthoringSession(session_state);
                return false;
            }
            const auto set_text = WorkerChainJsonText(set_result.Get());
            if (set_text.find("\"repositoryRef\"") == std::string::npos)
            {
                EXPECT_NE(set_text.find("\"repositoryRef\""), std::string::npos)
                    << set_text;
                DestroyGraphAuthoringSession(session_state);
                return false;
            }
        }

        // 编译：注入 pass 产物广播绑定进 plan.binding_plan。
        std::string parent_shell_text;
        {
            Das::DasPtr<Das::ExportInterface::IDasJson> compiled;
            if (DAS::IsFailed(GraphAuthoringSessionCompile(
                    session_state,
                    nullptr,
                    compiled.Put())))
            {
                EXPECT_TRUE(false) << "GraphAuthoringSessionCompile failed";
                DestroyGraphAuthoringSession(session_state);
                return false;
            }
            const auto envelope_text_response =
                WorkerChainJsonText(compiled.Get());
            auto envelope = yyjson::read(envelope_text_response);
            auto envelope_obj = envelope.as_object();
            if (!envelope_obj.has_value())
            {
                EXPECT_TRUE(envelope_obj.has_value()) << envelope_text_response;
                DestroyGraphAuthoringSession(session_state);
                return false;
            }
            auto compile_obj =
                (*envelope_obj)[std::string_view("compile")].as_object();
            if (!compile_obj.has_value())
            {
                EXPECT_TRUE(compile_obj.has_value()) << envelope_text_response;
                DestroyGraphAuthoringSession(session_state);
                return false;
            }
            const auto& execution_input =
                (*compile_obj)[std::string_view("executionInput")];
            // 会话编译出口的 executionInput 为 shell 对象形态——序列化为
            // 文本（与注入 pass 的 default_value 消费形态一致）。
            if (const auto text = execution_input.as_string())
            {
                parent_shell_text = std::string(*text);
            }
            else
            {
                parent_shell_text =
                    Das::Utils::SerializeYyjsonValue(
                        Das::Utils::CloneYyjsonValue(execution_input))
                        .value_or("");
            }
            if (parent_shell_text.empty())
            {
                EXPECT_FALSE(parent_shell_text.empty());
                DestroyGraphAuthoringSession(session_state);
                return false;
            }
        }
        DestroyGraphAuthoringSession(session_state);

        // 解析父 shell：断言绑条目记档在场（referenceEntries 携 bound 节
        // 点→42——执行时编译段的驱动数据）且产物零寄存（无 executionInput
        // 目标绑定）。
        yyjson::value parent_plan_value;
        {
            auto shell = yyjson::read(parent_shell_text);
            auto shell_obj = shell.as_object();
            if (!shell_obj.has_value())
            {
                EXPECT_TRUE(shell_obj.has_value()) << parent_shell_text;
                return false;
            }
            auto fingerprint =
                (*shell_obj)[std::string_view("fingerprint")].as_string();
            if (!fingerprint.has_value())
            {
                EXPECT_TRUE(fingerprint.has_value());
                return false;
            }
            out_fingerprint = *fingerprint;

            auto plan_value = (*shell_obj)[std::string_view("plan")];
            auto plan_obj = plan_value.as_object();
            if (!plan_obj.has_value())
            {
                EXPECT_TRUE(plan_obj.has_value());
                return false;
            }
            // DTO 序列化为 camelCase 键（PFR 字段名驼峰化——
            // CompiledArtifactTest 契约）。
            auto reference_entries =
                (*plan_obj)[std::string_view("referenceEntries")].as_array();
            if (!reference_entries.has_value())
            {
                EXPECT_TRUE(reference_entries.has_value());
                return false;
            }
            bool found_record = false;
            for (const auto& entry : *reference_entries)
            {
                auto entry_obj = entry.as_object();
                if (!entry_obj.has_value())
                {
                    continue;
                }
                auto node_id =
                    (*entry_obj)[std::string_view("nodeId")].as_string();
                auto entry_id =
                    (*entry_obj)[std::string_view("entryId")].as_int();
                if (!node_id.has_value() || *node_id != bound_node_id)
                {
                    continue;
                }
                EXPECT_EQ(entry_id.value_or(0), 42);
                found_record = true;
            }
            if (!found_record)
            {
                EXPECT_TRUE(found_record)
                    << "compiled plan must record the bound entry";
                return false;
            }
            auto binding_plan =
                (*plan_obj)[std::string_view("bindingPlan")].as_object();
            if (binding_plan.has_value())
            {
                auto bindings =
                    (*binding_plan)[std::string_view("bindings")].as_array();
                if (bindings.has_value())
                {
                    for (const auto& binding : *bindings)
                    {
                        auto binding_obj = binding.as_object();
                        if (!binding_obj.has_value())
                        {
                            continue;
                        }
                        auto target_port =
                            (*binding_obj)[std::string_view("targetPortId")]
                                .as_string();
                        if (!target_port.has_value()
                            || *target_port != "executionInput")
                        {
                            continue;
                        }
                        // 声明口的派生悬空广播（null 默认，物化层无操作）
                        // 合法在场；钉死的是产物零寄存。
                        EXPECT_TRUE(
                            (*binding_obj)[std::string_view("defaultValue")]
                                .is_null())
                            << "executionInput binding must not carry an "
                               "artifact (zero artifact registration)";
                    }
                }
            }
            parent_plan_value =
                Das::Utils::CloneYyjsonValue((*shell_obj)["plan"]);
        }
        out_plan = yyjson::cast<CompiledGraphPlanDto>(parent_plan_value);
        out_creator = std::move(creator);
        return true;
    }

} // namespace

// ===================================================================
// Test 1: MaapiRunNodeExecutesInjectedEnvelopeInGraphRuntime
//
// tracer 端到端链路验收（执行时编译链形态）：
//   GraphRuntime::RunWithHost → 绑条目 maapi.run 节点执行期间经 creator
//   现编（host mock 链为指定载体）→ 产物写 executionInput 协议保留键
//   （envelope 原文）→ MaapiRunTaskComponent::Do → 共享执行内核 →
//   MaaRuntime::Run（FakeMaaApiBoundary 观测）
// ===================================================================

TEST(MaapiGraphE2ETest, MaapiRunNodeExecutesInjectedEnvelopeInGraphRuntime)
{
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    auto plan = MakeSingleMaapiRunNodePlan();

    auto host = MaapiMockHost::Make(
        MakeEntryCompileResponse(MakeEnvelopeArtifactText()));
    auto token = PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    GraphRuntime rt;
    auto         hr = rt.RunWithHost(plan, "fp_e2e", token.Get(), host.Get());

    EXPECT_EQ(hr, DAS_S_OK);

    // 组件被真实驱动：maapi run 组件经 executionInput 消费注入的 envelope
    // 并到达 MaaRuntime 的任务投递。
    EXPECT_TRUE(fake.ContainsSubstring("PostTask:StartE2E:"));
    EXPECT_TRUE(fake.Contains("CreateTasker"));
}

// ===================================================================
// Test 2: MaapiRunNodePropagatesChildFailureToGraph
//
// MaaFW 任务失败时组件 Do 返回失败码——图执行走失败接管路径（运行期返
// 回非成功码），boundary 可观测到已尝试投递。调用中介退役后无「子失败
// 收进 status 端口、自身返回成功」的中间层：节点即组件，失败直达。
// ===================================================================

TEST(MaapiGraphE2ETest, MaapiRunNodePropagatesChildFailureToGraph)
{
    FakeMaaApiBoundary fake;
    fake.wait_status_by_entry["StartE2E"] = MaaTaskStatus::Failed;
    const ScopedBoundaryHook hook(fake);

    auto plan = MakeSingleMaapiRunNodePlan();

    auto host = MaapiMockHost::Make(
        MakeEntryCompileResponse(MakeEnvelopeArtifactText()));
    auto token = PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    GraphRuntime rt;
    auto         hr = rt.RunWithHost(plan, "fp_e2e", token.Get(), host.Get());

    EXPECT_NE(hr, DAS_S_OK);
    EXPECT_TRUE(fake.ContainsSubstring("PostTask:StartE2E:"));
}

// ===================================================================
// Test 3: WorkerModeInjectionChainExecutesCompileArtifact
//
// worker 模式注入链端到端验收（D-13 验收项，进程内形态——用户终裁
// 2026-09-22：真实 loadMode=ipc 跨进程链工作量过重，跨进程真实链由统一
// IPC 测试规划承接，todo: 2026-09-22-ipc-crossprocess-test-unification）。
// 链路（WINDOWS #8 闭合的进程内证据半边）：
//   主进程侧编译（真实 Core 会话 + 注入 pass）→ 绑条目 maapi.run 节点
//   收到 $graph_input → executionInput 广播绑定（default_value=被引条目
//   compile 段 envelope 原文）→ GraphRuntime 执行 → worker 侧组件
//   （真实 MaapiRunTaskComponent，进程内同构替身）Do 只吃
//   executionInput → FakeMaaApiBoundary 观测执行绿灯。
// 全程零 das.scheduler/具名服务查询：接口本体与具名注册已整删
// （115-10 四词 grep 全零——删除后想查也没有），组件供数无回退（缺
//   executionInput 即具名错，见 Test 4）。
// ===================================================================

TEST(MaapiGraphE2ETest, WorkerModeInjectionChainExecutesCompileArtifact)
{
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    const std::string envelope_text = MakeEnvelopeArtifactText();

    auto token = PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    Das::DasPtr<Das::PluginInterface::IDasTaskComponentCreator> host;
    CompiledGraphPlanDto                                        parent_plan;
    std::string parent_fingerprint;
    ASSERT_TRUE(CompileBoundMaapiRunNodePlan(
        envelope_text,
        host,
        parent_plan,
        parent_fingerprint));

    GraphRuntime rt;
    const auto   hr = rt.RunWithHost(
        parent_plan,
        parent_fingerprint,
        token.Get(),
        host.Get());
    ASSERT_EQ(hr, DAS_S_OK) << rt.GetLastErrorMessage();

    // worker 侧组件被真实驱动：maapi run 组件经 executionInput 消费编译
    // 注入的 envelope 并到达 MaaRuntime 的任务投递（产物跨面到达证明）。
    EXPECT_TRUE(fake.ContainsSubstring("PostTask:StartE2E:"));
    EXPECT_TRUE(fake.Contains("CreateTasker"));
}

// ===================================================================
// Test 4: WorkerModeInjectionChainFailsClosedWithoutArtifactBinding
//
// 同链编译产物剥离广播绑定后执行：组件 Do 遇缺 executionInput 即具名
// 错（DAS_E_MAAPI_EXECUTION_INPUT_MISSING），零静默回退、零具名服务
// 查询路径——「删除后想查也没有」的行为面证明（供数唯一面=
// executionInput，无第二条喂线路径）。
// ===================================================================

TEST(
    MaapiGraphE2ETest,
    WorkerModeInjectionChainFailsClosedWithoutArtifactBinding)
{
    FakeMaaApiBoundary       fake;
    const ScopedBoundaryHook hook(fake);

    const std::string envelope_text = MakeEnvelopeArtifactText();

    auto token = PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    Das::DasPtr<Das::PluginInterface::IDasTaskComponentCreator> host;
    CompiledGraphPlanDto                                        parent_plan;
    std::string parent_fingerprint;
    ASSERT_TRUE(CompileBoundMaapiRunNodePlan(
        envelope_text,
        host,
        parent_plan,
        parent_fingerprint));

    // 剥离绑条目记档（唯一供数通道——执行时编译段的驱动数据）：其余
    // plan 形状原样。
    parent_plan.reference_entries.clear();

    GraphRuntime rt;
    const auto   hr = rt.RunWithHost(
        parent_plan,
        parent_fingerprint,
        token.Get(),
        host.Get());
    EXPECT_NE(hr, DAS_S_OK)
        << "missing executionInput must fail the component, not fall back";

    // 无第二供数路径：boundary 零任务投递（组件没有从任何替代通道拿到
    // envelope——回退与具名服务查询均不存在）。
    EXPECT_FALSE(fake.ContainsSubstring("PostTask:"));
    EXPECT_FALSE(fake.Contains("CreateTasker"));
}
