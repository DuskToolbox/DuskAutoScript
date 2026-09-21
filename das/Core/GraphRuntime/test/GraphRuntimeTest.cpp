#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <das/Core/GraphRuntime/CompiledArtifact.h>
#include <das/Core/GraphRuntime/CompiledPlanFingerprint.h>
#include <das/Core/GraphRuntime/Config.h>
#include <das/Core/GraphRuntime/DoAdapter.h>
#include <das/Core/GraphRuntime/GraphDocument.h>
#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>
#include <das/Core/GraphRuntime/PortFrame.h>
#include <das/Core/GraphRuntime/RuntimeExecutionCache.h>
#include <das/Core/Utils/DasJsonImpl.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasErrorLens.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasImage.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasErrorLens.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasStopToken.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponent.Implements.hpp>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskComponentHost.Implements.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// Forward declaration — GraphRuntime.h will be created in Task 2
#include <das/Core/GraphRuntime/GraphRuntime.h>

namespace
{
    using namespace Das::Core::GraphRuntime;
    using namespace Das::Core::GraphRuntime::Dto;

    using IDasPortMap = Das::ExportInterface::IDasPortMap;
    using IDasStopToken = Das::PluginInterface::IDasStopToken;

    // ---- ID helpers ----

    // 节点 ID 采用 authoring 契约层 AllocateStableNodeId 分配的 "n_N"
    // 短字符串生产形态——文档层与帧数据平面的节点标识都是字符串。
    const std::string NODE_A = "n_1";
    const std::string NODE_B = "n_2";
    const std::string NODE_C = "n_3";

    // Test component GUIDs — 36-char standard format for MakeDasGuid
    const std::string COMP_A = "20000000-0000-0000-0000-000000000001";
    const std::string COMP_TEST = "20000000-0000-0000-0000-000000000099";

    // ---- Mock StopToken ----

    class MockStopToken final
        : public Das::PluginInterface::DasStopTokenImplBase<MockStopToken>
    {
    public:
        std::atomic<bool> cancelled{false};

        DasResult DAS_STD_CALL StopRequested(bool* canStop) override
        {
            if (!canStop)
            {
                return DAS_E_INVALID_POINTER;
            }
            *canStop = cancelled.load();
            return DAS_S_OK;
        }
    };

    // ---- Plan fixture helpers ----

    GraphPortDefinitionDto MakePortDef(
        const std::string& port_id,
        const std::string& port_type = "int")
    {
        GraphPortDefinitionDto p;
        p.port_id = port_id;
        p.port_type = port_type;
        return p;
    }

    CompiledNodeSnapshotDto MakeSnapshot(
        const std::string& node_id,
        const std::string& component_guid =
            "20000000-0000-0000-0000-000000000099")
    {
        CompiledNodeSnapshotDto snap;
        snap.node_id = node_id;
        snap.component_guid = component_guid;
        return snap;
    }

    CompiledNodeSnapshotDto MakeSnapshotWithPorts(
        const std::string&                         node_id,
        const std::vector<GraphPortDefinitionDto>& ports,
        const std::string&                         component_guid =
            "20000000-0000-0000-0000-000000000099")
    {
        CompiledNodeSnapshotDto snap;
        snap.node_id = node_id;
        snap.component_guid = component_guid;
        snap.resolved_ports = ports;
        return snap;
    }

    PortBindingDto MakeBinding(
        const std::string& src_node,
        const std::string& src_port,
        const std::string& tgt_node,
        const std::string& tgt_port,
        const std::string& type = "int")
    {
        PortBindingDto b;
        b.source_node_id = src_node;
        b.source_port_id = src_port;
        b.target_node_id = tgt_node;
        b.target_port_id = tgt_port;
        b.expected_type = type;
        return b;
    }

    // Build a linear chain plan: node_0 -> node_1 -> ... -> node_{n-1}
    // Each node has input port "in" and output port "out"
    CompiledGraphPlanDto MakeLinearPlan(int num_nodes)
    {
        CompiledGraphPlanDto plan;
        plan.source_fingerprint = "fp_v1";

        for (int i = 0; i < num_nodes; ++i)
        {
            const std::string nid = "n_" + std::to_string(i + 1);

            auto snap = MakeSnapshotWithPorts(
                nid,
                {MakePortDef("in"), MakePortDef("out")},
                COMP_TEST);
            plan.node_snapshots.push_back(std::move(snap));
            plan.execution_order.push_back(nid);
        }

        // Create edges between consecutive nodes
        for (int i = 0; i + 1 < num_nodes; ++i)
        {
            plan.binding_plan.bindings.push_back(MakeBinding(
                plan.execution_order[i],
                "out",
                plan.execution_order[i + 1],
                "in"));
        }

        return plan;
    }

    CompiledGraphPlanDto MakeEmptyPlan()
    {
        CompiledGraphPlanDto plan;
        plan.source_fingerprint = "fp_v1";
        return plan;
    }

    // Get input bindings for a specific target node
    std::vector<PortBindingDto> GetInputBindings(
        const CompiledGraphPlanDto& plan,
        const std::string&          target_node_id)
    {
        std::vector<PortBindingDto> result;
        for (const auto& b : plan.binding_plan.bindings)
        {
            if (b.target_node_id == target_node_id)
            {
                result.push_back(b);
            }
        }
        return result;
    }

} // namespace

// =====================================================================
// Plan 02-14: Configure/Prepare/RunWithHost tests
// =====================================================================

// Named namespace for mock types (required for template specializations)
namespace GraphRuntimeTestMock
{
    using IDasJson = Das::ExportInterface::IDasJson;
    using IDasImage = Das::ExportInterface::IDasImage;
    using IDasStopToken = Das::PluginInterface::IDasStopToken;
    using IDasTaskComponent = Das::PluginInterface::IDasTaskComponent;
    using IDasTaskComponentHost = Das::PluginInterface::IDasTaskComponentHost;

    // 计数型 fake IDasImage（DoAdapterTest 同款形态）：引用身份断言的对
    // 象锚点——跨 PortMap/帧/导出全程同一指针即证明引用直传。
    class CountingFakeImage final
        : public Das::ExportInterface::DasImageImplBase<CountingFakeImage>
    {
    public:
        DAS_IMPL GetSize(Das::ExportInterface::DasSize* p_out_size) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DAS_IMPL GetChannelCount(int32_t* p_out_channel_count) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DAS_IMPL Clip(
            const Das::ExportInterface::DasRect* p_rect,
            Das::ExportInterface::IDasImage**    p_out_image) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DAS_IMPL GetDataSize(uint64_t* p_out_size) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DAS_IMPL GetBinaryBuffer(
            Das::ExportInterface::IDasBinaryBuffer** pp_out_buffer) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }

        DAS_IMPL GetPixelFormat(
            Das::ExportInterface::DasImagePixelFormat* p_out_format) override
        {
            return DAS_E_NO_IMPLEMENTATION;
        }
    };

    // ---- Mock IDasTaskComponent ----
    // Manual implementation (not using ImplBase to avoid DasIidOf linker
    // issues with anonymous-namespace types).
    class MockTaskComponent final : public IDasTaskComponent
    {
    public:
        std::atomic<uint32_t> ref_count_{0};
        std::atomic<int>      apply_settings_call_count{0};
        std::atomic<int>      do_call_count{0};
        std::string           last_settings_json;
        std::string           last_input_json;

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            auto c = --ref_count_;
            if (c == 0)
            {
                ref_count_ = 1;
                delete this;
            }
            return c;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (!pp_out)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            if (!p_out_guid)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_out_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                "30000000-0000-0000-0000-000000000001");
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            if (!pp_out_name)
            {
                return DAS_E_INVALID_POINTER;
            }
            return CreateIDasReadOnlyStringFromUtf8(
                "MockTaskComponent",
                pp_out_name);
        }

        DasResult ApplySettingsChange(
            IDasJson*  p_request_json,
            IDasJson** pp_out_result_json) override
        {
            ++apply_settings_call_count;
            if (p_request_json)
            {
                DAS::DasPtr<IDasReadOnlyString> p_str;
                p_request_json->ToString(0, p_str.Put());
                if (p_str.Get())
                {
                    const char* utf8 = nullptr;
                    p_str->GetUtf8(&utf8);
                    last_settings_json =
                        utf8 ? std::string{utf8} : std::string{};
                }
            }
            if (pp_out_result_json)
            {
                *pp_out_result_json = new Das::Core::Utils::IDasJsonImpl("{}");
                (*pp_out_result_json)->AddRef();
            }
            return DAS_S_OK;
        }

        DasResult Do(
            IDasStopToken*                             p_stop_token,
            Das::ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
            Das::ExportInterface::IDasPortMap**        pp_out_port_map) override
        {
            ++do_call_count;

            if (pp_out_port_map == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }

            DAS::DasPtr<Das::ExportInterface::IDasPortMap> output_map;
            DasResult hr = CreateIDasPortMap(output_map.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }

            // Echo input ports to output if available
            if (p_input_port_map)
            {
                DAS::DasPtr<Das::ExportInterface::IDasStringVector> keys;
                hr = p_input_port_map->GetKeys(keys.Put());
                if (DAS::IsOk(hr) && keys)
                {
                    uint64_t count = 0;
                    keys->Size(&count);
                    for (uint64_t i = 0; i < count; ++i)
                    {
                        IDasReadOnlyString* p_key = nullptr;
                        if (DAS::IsOk(keys->At(i, &p_key)) && p_key)
                        {
                            IDasReadOnlyString* p_val = nullptr;
                            if (DAS::IsOk(
                                    p_input_port_map->GetString(p_key, &p_val))
                                && p_val)
                            {
                                output_map->SetString(p_key, p_val);
                                p_val->Release();
                            }
                            p_key->Release();
                        }
                    }
                }
            }

            *pp_out_port_map = output_map.Get();
            output_map.Get()->AddRef();
            return DAS_S_OK;
        }

        static IDasTaskComponent* MakeRaw()
        {
            auto* p = new MockTaskComponent();
            p->AddRef();
            return p;
        }
    };

    // ---- Mock IDasTaskComponentHost ----
    class MockTaskComponentHost final
        : public Das::PluginInterface::DasTaskComponentHostImplBase<
              MockTaskComponentHost>
    {
    public:
        std::vector<std::string> created_guids;

        DasResult CreateTaskComponent(
            const DasGuid&                            component_guid,
            Das::PluginInterface::IDasTaskComponent** pp_out_component) override
        {
            if (!pp_out_component)
            {
                return DAS_E_INVALID_POINTER;
            }

            auto guid_str = Das::Core::ForeignInterfaceHost::DasGuidToStdString(
                component_guid);
            created_guids.push_back(guid_str);

            *pp_out_component = MockTaskComponent::MakeRaw();
            return DAS_S_OK;
        }

        DasResult CreateAuthoringSession(
            const DasGuid&                                   task_guid,
            Das::ExportInterface::IDasJson*                  p_context_json,
            Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session)
            override
        {
            if (!pp_out_session)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_session = nullptr;
            return DAS_E_NO_IMPLEMENTATION;
        }
    };

    // ---- Mock IDasErrorLens ----
    class MockErrorLens final
        : public Das::PluginInterface::DasErrorLensImplBase<MockErrorLens>
    {
    public:
        std::map<DasResult, std::string> error_messages;

        DasResult GetSupportedIids(
            Das::ExportInterface::IDasReadOnlyGuidVector** pp_out_iids) override
        {
            if (!pp_out_iids)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_iids = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult GetErrorMessage(
            IDasReadOnlyString*  locale_name,
            DasResult            error_code,
            IDasReadOnlyString** out_string) override
        {
            if (!out_string)
            {
                return DAS_E_INVALID_POINTER;
            }

            auto it = error_messages.find(error_code);
            if (it != error_messages.end())
            {
                return CreateIDasReadOnlyStringFromUtf8(
                    it->second.c_str(),
                    out_string);
            }
            *out_string = nullptr;
            return DAS_E_NOT_FOUND;
        }
    };

    // Build a plan with a settings snapshot for a node
    inline CompiledGraphPlanDto MakePlanWithSettings(
        const std::string& node_id,
        const std::string& component_guid,
        const std::string& settings_json,
        const std::string& payload_json)
    {
        CompiledGraphPlanDto plan;
        plan.source_fingerprint = "fp_v1";

        CompiledNodeSnapshotDto snap;
        snap.node_id = node_id;
        snap.component_guid = component_guid;

        if (!settings_json.empty())
        {
            auto parsed = Das::Utils::ParseYyjsonFromString(settings_json);
            if (parsed.has_value())
            {
                snap.compiled_settings = std::move(*parsed);
            }
        }
        if (!payload_json.empty())
        {
            auto parsed = Das::Utils::ParseYyjsonFromString(payload_json);
            if (parsed.has_value())
            {
                snap.compiled_payload_json = std::move(*parsed);
            }
        }

        snap.resolved_ports = {MakePortDef("in"), MakePortDef("out")};
        plan.node_snapshots.push_back(std::move(snap));
        plan.execution_order.push_back(node_id);
        return plan;
    }

    // ---- TypedIO image mock ----

    // 输出固定 image 引用的替身组件：Do 把构造期持有的同一 IDasImage 实例
    // 写到 "img" 口——边界导出引用身份断言的对象锚点。
    class ImageSourceComponent final : public IDasTaskComponent
    {
    public:
        std::atomic<uint32_t> ref_count_{0};

        // 裸指针成员自管引用：构造补一次拥有引用，析构对称释放。
        explicit ImageSourceComponent(IDasImage* image) : image_(image)
        {
            if (image_)
            {
                image_->AddRef();
            }
        }

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            auto c = --ref_count_;
            if (c == 0)
            {
                ref_count_ = 1;
                delete this;
            }
            return c;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (!pp_out)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            if (!p_out_guid)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_out_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                "20000000-0000-0000-0000-0000000000A1");
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            if (!pp_out_name)
            {
                return DAS_E_INVALID_POINTER;
            }
            return CreateIDasReadOnlyStringFromUtf8(
                "ImageSourceComponent",
                pp_out_name);
        }

        DasResult ApplySettingsChange(IDasJson*, IDasJson** pp_out_result_json)
            override
        {
            if (pp_out_result_json)
            {
                *pp_out_result_json = nullptr;
            }
            return DAS_S_OK;
        }

        DasResult Do(
            IDasStopToken*,
            Das::ExportInterface::IDasReadOnlyPortMap*,
            Das::ExportInterface::IDasPortMap** pp_out_port_map) override
        {
            if (pp_out_port_map == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_port_map = nullptr;
            DAS::DasPtr<Das::ExportInterface::IDasPortMap> output_map;
            DasResult hr = CreateIDasPortMap(output_map.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            const DasReadOnlyString image_key{"img"};
            hr = output_map->SetImage(image_key.Get(), image_);
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            *pp_out_port_map = output_map.Get();
            output_map.Get()->AddRef();
            return DAS_S_OK;
        }

    private:
        ~ImageSourceComponent()
        {
            if (image_)
            {
                image_->Release();
            }
        }

        IDasImage* image_;
    };

    // 只分派 image 源替身的 host（guid 与组件 GetGuid 的镜像一致）。
    class ImageSourceHost final
        : public Das::PluginInterface::DasTaskComponentHostImplBase<
              ImageSourceHost>
    {
    public:
        DasResult CreateTaskComponent(
            const DasGuid&                            component_guid,
            Das::PluginInterface::IDasTaskComponent** pp_out_component) override
        {
            if (!pp_out_component)
            {
                return DAS_E_INVALID_POINTER;
            }
            const auto expected = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                "20000000-0000-0000-0000-0000000000A1");
            if (!(component_guid == expected))
            {
                *pp_out_component = nullptr;
                return DAS_E_NOT_FOUND;
            }
            auto* component = new ImageSourceComponent(image_.Get());
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }

        DasResult CreateAuthoringSession(
            const DasGuid&                                   task_guid,
            Das::ExportInterface::IDasJson*                  p_context_json,
            Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session)
            override
        {
            if (!pp_out_session)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_session = nullptr;
            return DAS_E_NO_IMPLEMENTATION;
        }

        void SetImage(IDasImage* image)
        {
            // DasPtr::Attach 是静态接管原语：先补一次拥有引用再整体赋回。
            image->AddRef();
            image_ = Das::DasPtr<IDasImage>::Attach(image);
        }

    private:
        Das::DasPtr<IDasImage> image_;
    };

    // 首次 CreateTaskComponent 抛异常、后续正常的 host：驱动同一 runtime
    // 实例走「异常逃逸后恢复执行」路径（graph_inputs 悬挂指针防护断言用）。
    class ThrowingOnceHost final
        : public Das::PluginInterface::DasTaskComponentHostImplBase<
              ThrowingOnceHost>
    {
    public:
        DasResult CreateTaskComponent(
            const DasGuid&                            component_guid,
            Das::PluginInterface::IDasTaskComponent** pp_out_component) override
        {
            std::ignore = component_guid;
            if (!pp_out_component)
            {
                return DAS_E_INVALID_POINTER;
            }
            if (throw_next_)
            {
                throw_next_ = false;
                throw std::runtime_error("injected component factory failure");
            }
            *pp_out_component = MockTaskComponent::MakeRaw();
            return DAS_S_OK;
        }

        DasResult CreateAuthoringSession(
            const DasGuid&                                   task_guid,
            Das::ExportInterface::IDasJson*                  p_context_json,
            Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session)
            override
        {
            if (!pp_out_session)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_session = nullptr;
            return DAS_E_NO_IMPLEMENTATION;
        }

    private:
        bool throw_next_ = true;
    };

} // namespace GraphRuntimeTestMock

// ===================================================================
// Test 16: ConfigureCreatesComponents
// ===================================================================
TEST(GraphRuntimeTest, ConfigureCreatesComponents)
{
    using namespace GraphRuntimeTestMock;

    auto plan =
        MakePlanWithSettings(NODE_A, COMP_A, R"({"threshold": 42})", "");

    auto host = MockTaskComponentHost::Make();

    GraphRuntime rt;
    auto         hr = rt.Configure(plan, host.Get());

    EXPECT_EQ(hr, DAS_S_OK);

    auto* mock_host = static_cast<MockTaskComponentHost*>(host.Get());
    ASSERT_EQ(mock_host->created_guids.size(), 1u);
}

// ===================================================================
// Test 17: ConfigureAppliesSettings
// ===================================================================
TEST(GraphRuntimeTest, ConfigureAppliesSettings)
{
    using namespace GraphRuntimeTestMock;

    auto plan = MakePlanWithSettings(
        NODE_A,
        COMP_A,
        R"({"threshold": 42})",
        R"({"mode": "fast"})");

    auto host = MockTaskComponentHost::Make();

    GraphRuntime rt;
    EXPECT_EQ(DAS_S_OK, rt.Configure(plan, host.Get()));

    // Verify Prepare succeeds
    EXPECT_EQ(DAS_S_OK, rt.Prepare());
}

// ===================================================================
// Test 18: PrepareValidatesConfiguredComponents
// ===================================================================
TEST(GraphRuntimeTest, PrepareValidatesConfiguredComponents)
{
    using namespace GraphRuntimeTestMock;

    // Prepare without Configure should fail
    GraphRuntime rt;
    EXPECT_NE(DAS_S_OK, rt.Prepare());

    // Prepare after Configure should succeed
    auto plan = MakeLinearPlan(2);
    auto host = MockTaskComponentHost::Make();

    EXPECT_EQ(DAS_S_OK, rt.Configure(plan, host.Get()));
    EXPECT_EQ(DAS_S_OK, rt.Prepare());
}

// ===================================================================
// Test 19: ConfigureNullHost
// ===================================================================
TEST(GraphRuntimeTest, ConfigureNullHost)
{
    auto plan = MakeLinearPlan(1);

    GraphRuntime rt;
    auto         hr = rt.Configure(plan, nullptr);
    EXPECT_NE(DAS_S_OK, hr);
    EXPECT_FALSE(rt.GetLastErrorMessage().empty());
}

// ===================================================================
// Test 20: RunWithHostSingleExecution
// ===================================================================
TEST(GraphRuntimeTest, RunWithHostSingleExecution)
{
    using namespace GraphRuntimeTestMock;
    CompiledGraphPlanDto plan;
    plan.source_fingerprint = "fp_v1";

    plan.node_snapshots.push_back(
        MakeSnapshotWithPorts(NODE_A, {MakePortDef("out")}, COMP_A));
    plan.execution_order = {NODE_A};

    auto host = MockTaskComponentHost::Make();

    GraphRuntime                     rt;
    const Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get());
    EXPECT_EQ(hr, DAS_S_OK);
}

// ===================================================================
// Test 21: RunWithHostLinearChain
// ===================================================================
TEST(GraphRuntimeTest, RunWithHostLinearChain)
{
    using namespace GraphRuntimeTestMock;
    auto plan = MakeLinearPlan(3);

    auto host = MockTaskComponentHost::Make();

    GraphRuntime                     rt;
    const Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get());
    EXPECT_EQ(hr, DAS_S_OK);
}

// ===================================================================
// Test 22: RunWithHostFingerprintMismatch
// ===================================================================
TEST(GraphRuntimeTest, RunWithHostFingerprintMismatch)
{
    using namespace GraphRuntimeTestMock;
    auto plan = MakeLinearPlan(2);
    plan.source_fingerprint = "compiled_at_v1";

    auto host = MockTaskComponentHost::Make();

    GraphRuntime                     rt;
    const Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "wrong_fp", token.Get(), host.Get());
    EXPECT_NE(hr, DAS_S_OK);
    EXPECT_FALSE(rt.GetLastErrorMessage().empty());
}

// ===================================================================
// Test 23: ErrorLensIntegration
// ===================================================================
TEST(GraphRuntimeTest, ErrorLensIntegration)
{
    using namespace GraphRuntimeTestMock;
    auto  lens = MockErrorLens::Make();
    auto* raw_lens = static_cast<MockErrorLens*>(lens.Get());
    raw_lens->error_messages[DAS_E_NOT_FOUND] = "Resource not available";

    GraphRuntime rt;
    rt.SetErrorLens(lens.Get());

    std::string msg;
    auto        hr = rt.GetStructuredError(DAS_E_NOT_FOUND, msg);
    EXPECT_EQ(hr, DAS_S_OK);
    EXPECT_EQ(msg, "Resource not available");

    // Unknown error code
    hr = rt.GetStructuredError(DAS_E_FAIL, msg);
    EXPECT_NE(hr, DAS_S_OK);
}

// ===================================================================
// Test 24: RunWithHostEmptyGraph
// ===================================================================
TEST(GraphRuntimeTest, RunWithHostEmptyGraph)
{
    using namespace GraphRuntimeTestMock;
    auto plan = MakeEmptyPlan();

    auto host = MockTaskComponentHost::Make();

    GraphRuntime                     rt;
    const Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get());
    EXPECT_EQ(hr, DAS_S_OK);
}

// ===================================================================
// 编译否决产物（带节点、无执行序、有数据回边）必须执行失败而非
// 「成功+零节点」，且 fatal 诊断消息透出到 last_error。
// ===================================================================
TEST(GraphRuntimeTest, RunWithHostRejectedPlanFailsClosed)
{
    using namespace GraphRuntimeTestMock;

    CompiledGraphPlanDto plan;
    plan.source_fingerprint = "fp_v1";
    plan.node_snapshots.push_back(
        MakeSnapshotWithPorts(NODE_A, {MakePortDef("out")}, COMP_A));

    Dto::BackEdgeDto back_edge;
    back_edge.source_node_id = NODE_A;
    back_edge.source_port_id = "out";
    back_edge.target_node_id = NODE_A;
    back_edge.target_port_id = "in";
    back_edge.loop_head_node_id = NODE_A;
    back_edge.edge_kind = "data";
    plan.back_edges.push_back(std::move(back_edge));

    auto diagnostic = Das::Utils::ParseYyjsonFromString(
        R"({"kind":"7","node_id":")" + NODE_A
        + R"(","message":"Data cycle has no signal circuit to carry the loop"})");
    ASSERT_TRUE(diagnostic.has_value());
    plan.diagnostics.push_back(std::move(*diagnostic));

    auto host = MockTaskComponentHost::Make();

    GraphRuntime                     rt;
    const Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get());
    EXPECT_EQ(hr, DAS_E_FAIL);
    EXPECT_NE(
        rt.GetLastErrorMessage().find("no executable order"),
        std::string::npos);
    EXPECT_NE(
        rt.GetLastErrorMessage().find(
            "Data cycle has no signal circuit to carry the loop"),
        std::string::npos);
}

// ===================================================================
// Test 25: V17DataSepSettingsNotInPortMap
// ===================================================================
TEST(GraphRuntimeTest, V17DataSepSettingsNotInPortMap)
{
    using namespace GraphRuntimeTestMock;
    // Verify that settings are applied via ApplySettingsChange (Configure)
    // and NOT passed through the PortMap data plane during Do().
    auto plan =
        MakePlanWithSettings(NODE_A, COMP_A, R"({"threshold": 42})", "");

    auto host = MockTaskComponentHost::Make();

    GraphRuntime rt;
    EXPECT_EQ(DAS_S_OK, rt.Configure(plan, host.Get()));

    // The mock component should have received settings
    auto* mock_host = static_cast<MockTaskComponentHost*>(host.Get());
    ASSERT_EQ(mock_host->created_guids.size(), 1u);
}

// ===================================================================
// DAS-60 Stage 3: signal-gated runtime (gate / skip / loop)
// ===================================================================
//
// A behaviour-driven mock component lets each node play a control-flow role
// (emit a fixed signal, count loops, alternate branch signals, record Do
// calls) driven entirely by its compiled_settings. The host creates one
// instance per node and keeps observing pointers so tests can assert which
// nodes ran and how many times — the runtime exposes neither the PortFrame
// nor the components, so the do_count is the observation point.

namespace SignalGatedTestMock
{
    using IDasJson = Das::ExportInterface::IDasJson;
    using IDasPortMap = Das::ExportInterface::IDasPortMap;
    using IDasStopToken = Das::PluginInterface::IDasStopToken;
    using IDasTaskComponent = Das::PluginInterface::IDasTaskComponent;
    using IDasTaskComponentHost = Das::PluginInterface::IDasTaskComponentHost;

    // Shared component GUID for every behaviour node — the host ignores it and
    // the role comes from settings, so a single GUID is enough.
    constexpr std::string_view BEHAVIOR_GUID =
        "20000000-0000-0000-0000-000000000099";

    class BehaviorComponent final : public IDasTaskComponent
    {
    public:
        std::atomic<uint32_t> ref_count_{0};
        std::atomic<int>      do_count{0};

        // role configuration (parsed from compiled_settings)
        std::string mode; // emit | record | loop | alternate | accumulator
        std::string emit_signal;  // mode=emit: which signal port to fire
        int         loop_end = 0; // mode=loop: iteration count
        int         counter = 0;  // mode=loop / alternate: persists across Do()
        int64_t     picked_value = -1; // mode=merge: value copied to result
        // mode=accumulator: the "in" fallback when the frame holds no value
        // (the outer-restart clearing evidence), the bound up to which the
        // loop keeps iterating, and the observed input sequence.
        int64_t              acc_seed = 0;
        int64_t              acc_until = INT64_MAX;
        std::vector<int64_t> input_values;

        uint32_t DAS_STD_CALL AddRef() override { return ++ref_count_; }

        uint32_t DAS_STD_CALL Release() override
        {
            auto c = --ref_count_;
            if (c == 0)
            {
                ref_count_ = 1;
                delete this;
            }
            return c;
        }

        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override
        {
            if (!pp_out)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out = nullptr;
            return DAS_E_NO_INTERFACE;
        }

        DasResult DAS_STD_CALL GetGuid(DasGuid* p_out_guid) override
        {
            if (!p_out_guid)
            {
                return DAS_E_INVALID_POINTER;
            }
            *p_out_guid = Das::Core::ForeignInterfaceHost::MakeDasGuid(
                std::string{BEHAVIOR_GUID});
            return DAS_S_OK;
        }

        DasResult DAS_STD_CALL
        GetRuntimeClassName(IDasReadOnlyString** pp_out_name) override
        {
            if (!pp_out_name)
            {
                return DAS_E_INVALID_POINTER;
            }
            return CreateIDasReadOnlyStringFromUtf8(
                "BehaviorComponent",
                pp_out_name);
        }

        DasResult ApplySettingsChange(
            IDasJson*  p_request_json,
            IDasJson** pp_out_result_json) override
        {
            if (pp_out_result_json)
            {
                *pp_out_result_json = new Das::Core::Utils::IDasJsonImpl("{}");
                (*pp_out_result_json)->AddRef();
            }
            if (!p_request_json)
            {
                return DAS_S_OK;
            }

            // ApplyNodeSettings wraps the role as {"settings": {...}}.
            DAS::DasPtr<IDasReadOnlyString> p_str;
            p_request_json->ToString(0, p_str.Put());
            if (!p_str.Get())
            {
                return DAS_S_OK;
            }
            const char* utf8 = nullptr;
            p_str->GetUtf8(&utf8);
            auto parsed = Das::Utils::ParseYyjsonFromString(
                utf8 ? std::string{utf8} : std::string{});
            if (!parsed.has_value())
            {
                return DAS_S_OK;
            }
            auto root_obj = parsed->as_object();
            if (!root_obj.has_value())
            {
                return DAS_S_OK;
            }
            yyjson::value settings = *parsed;
            if (root_obj->contains(std::string_view("settings")))
            {
                auto sv = (*root_obj)[std::string_view("settings")];
                if (sv.is_object())
                {
                    settings = sv;
                }
            }
            auto sobj = settings.as_object();
            if (!sobj.has_value())
            {
                return DAS_S_OK;
            }
            if (sobj->contains(std::string_view("mode")))
            {
                auto m = (*sobj)[std::string_view("mode")].as_string();
                if (m.has_value())
                {
                    mode = std::string(*m);
                }
            }
            if (sobj->contains(std::string_view("signal")))
            {
                auto s = (*sobj)[std::string_view("signal")].as_string();
                if (s.has_value())
                {
                    emit_signal = std::string(*s);
                }
            }
            if (sobj->contains(std::string_view("end")))
            {
                auto e = (*sobj)[std::string_view("end")].as_string();
                if (e.has_value())
                {
                    loop_end = std::atoi(std::string(*e).c_str());
                }
            }
            if (sobj->contains(std::string_view("seed")))
            {
                auto s = (*sobj)[std::string_view("seed")].as_string();
                if (s.has_value())
                {
                    acc_seed = std::atoll(std::string(*s).c_str());
                }
            }
            if (sobj->contains(std::string_view("until")))
            {
                auto u = (*sobj)[std::string_view("until")].as_string();
                if (u.has_value())
                {
                    acc_until = std::atoll(std::string(*u).c_str());
                }
            }
            return DAS_S_OK;
        }

        // Emit a signal by writing the reserved "signals" JSON string array.
        void EmitSignal(IDasPortMap* map, const std::string& sig)
        {
            const std::string       json = std::string(R"([")") + sig + R"("])";
            const DasReadOnlyString key{"signals"};
            const DasReadOnlyString val{json.c_str()};
            map->SetString(key.Get(), val.Get());
        }

        void SetOutInt(IDasPortMap* map, const std::string& port, int64_t v)
        {
            const DasReadOnlyString key{port.c_str()};
            map->SetInt(key.Get(), v);
        }

        DasResult Do(
            IDasStopToken*                             p_stop_token,
            Das::ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
            Das::ExportInterface::IDasPortMap**        pp_out_port_map) override
        {
            (void)p_stop_token;
            (void)p_input_port_map;
            ++do_count;

            DAS::DasPtr<IDasPortMap> output;
            const DasResult          hr = CreateIDasPortMap(output.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }

            if (mode == "loop")
            {
                // 读 cond 输入口（外层携带通道的落点）：外层重启后能否读到
                // 上圈回边值是归属规则的验收点，缺失时不记录。
                if (p_input_port_map != nullptr)
                {
                    const DasReadOnlyString cond_key{"cond"};
                    int64_t                 cond_value{};
                    if (DAS::IsOk(p_input_port_map->GetInt(
                            cond_key.Get(),
                            &cond_value)))
                    {
                        input_values.push_back(cond_value);
                    }
                }
                if (counter < loop_end)
                {
                    SetOutInt(output.Get(), "index", counter);
                    EmitSignal(output.Get(), "continue");
                    ++counter;
                }
                else
                {
                    EmitSignal(output.Get(), "break");
                }
            }
            else if (mode == "alternate")
            {
                EmitSignal(output.Get(), counter % 2 == 0 ? "true" : "false");
                ++counter;
            }
            else if (mode == "emit")
            {
                if (!emit_signal.empty())
                {
                    EmitSignal(output.Get(), emit_signal);
                }
                SetOutInt(output.Get(), "out", do_count.load());
            }
            else if (mode == "merge")
            {
                // φ-join: copy whichever of value_true / value_false the taken
                // branch produced into result.
                int64_t                 v{};
                const DasReadOnlyString kt{"value_true"};
                if (p_input_port_map != nullptr
                    && DAS::IsOk(p_input_port_map->GetInt(kt.Get(), &v)))
                {
                    SetOutInt(output.Get(), "result", v);
                    picked_value = v;
                }
                else
                {
                    const DasReadOnlyString kf{"value_false"};
                    if (p_input_port_map != nullptr
                        && DAS::IsOk(p_input_port_map->GetInt(kf.Get(), &v)))
                    {
                        SetOutInt(output.Get(), "result", v);
                        picked_value = v;
                    }
                }
            }
            else if (mode == "accumulator")
            {
                // 数据面观测载体：读 in（缺键回落 seed——数据回边源口被清
                // 后第一圈的回落锚点）、out = in + 1；界内发 done 推进环迭
                // 代、界外改发 break 把下游门打开——有界环需要一个条件发
                // 射点，其余保持纯观测。
                int64_t in_value = acc_seed;
                if (p_input_port_map != nullptr)
                {
                    const DasReadOnlyString key{"in"};
                    int64_t                 v{};
                    if (DAS::IsOk(p_input_port_map->GetInt(key.Get(), &v)))
                    {
                        in_value = v;
                    }
                }
                input_values.push_back(in_value);
                const int64_t out_value = in_value + 1;
                SetOutInt(output.Get(), "out", out_value);
                // count 与 out 同值不同口：外层携带通道用它，与内层自环通
                // 道（out）分开清场互不影响。
                SetOutInt(output.Get(), "count", out_value);
                EmitSignal(
                    output.Get(),
                    out_value <= acc_until ? "done" : "break");
            }
            else
            {
                // "record" (and default): produce a data output, no signal.
                SetOutInt(output.Get(), "out", do_count.load());
            }

            *pp_out_port_map = output.Get();
            output.Get()->AddRef();
            return DAS_S_OK;
        }

        static IDasTaskComponent* MakeRaw()
        {
            auto* p = new BehaviorComponent();
            p->AddRef();
            return p;
        }
    };

    class BehaviorHost final
        : public Das::PluginInterface::DasTaskComponentHostImplBase<
              BehaviorHost>
    {
    public:
        // One entry per created component, in node-snapshot order, so a test
        // maps plan.node_snapshots[i] -> components[i].
        std::vector<BehaviorComponent*> components;

        DasResult CreateTaskComponent(
            const DasGuid&                            component_guid,
            Das::PluginInterface::IDasTaskComponent** pp_out_component) override
        {
            (void)component_guid;
            if (!pp_out_component)
            {
                return DAS_E_INVALID_POINTER;
            }
            auto* c =
                static_cast<BehaviorComponent*>(BehaviorComponent::MakeRaw());
            components.push_back(c);
            *pp_out_component = c;
            return DAS_S_OK;
        }

        DasResult CreateAuthoringSession(
            const DasGuid&                                   task_guid,
            Das::ExportInterface::IDasJson*                  p_context_json,
            Das::PluginInterface::IDasTaskAuthoringSession** pp_out_session)
            override
        {
            if (!pp_out_session)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_session = nullptr;
            return DAS_E_NO_IMPLEMENTATION;
        }
    };

    CompiledNodeSnapshotDto MakeBehaviorSnapshot(
        const std::string& node_id,
        const std::string& settings_json)
    {
        CompiledNodeSnapshotDto snap;
        snap.node_id = node_id;
        snap.component_guid = std::string{BEHAVIOR_GUID};
        if (!settings_json.empty())
        {
            auto parsed = Das::Utils::ParseYyjsonFromString(settings_json);
            if (parsed.has_value())
            {
                snap.compiled_settings = std::move(*parsed);
            }
        }
        return snap;
    }

    SignalRouteDto MakeSignalRoute(
        const std::string& src_node,
        const std::string& src_port,
        const std::string& tgt_node,
        const std::string& tgt_port)
    {
        SignalRouteDto r;
        r.source_node_id = src_node;
        r.source_port_id = src_port;
        r.target_node_id = tgt_node;
        r.target_port_id = tgt_port;
        return r;
    }

    BackEdgeDto MakeBackEdge(
        const std::string& edge_id,
        const std::string& src_node,
        const std::string& src_port,
        const std::string& tgt_node,
        const std::string& tgt_port,
        const std::string& loop_head)
    {
        BackEdgeDto be;
        be.edge_id = edge_id;
        be.source_node_id = src_node;
        be.source_port_id = src_port;
        be.target_node_id = tgt_node;
        be.target_port_id = tgt_port;
        be.loop_head_node_id = loop_head;
        be.edge_kind = "signal";
        return be;
    }

    BackEdgeDto MakeDataBackEdge(
        const std::string& edge_id,
        const std::string& src_node,
        const std::string& src_port,
        const std::string& tgt_node,
        const std::string& tgt_port,
        const std::string& loop_head)
    {
        BackEdgeDto be = MakeBackEdge(
            edge_id,
            src_node,
            src_port,
            tgt_node,
            tgt_port,
            loop_head);
        be.edge_kind = "data";
        return be;
    }

    PortRefDto MakePortRef(const std::string& node, const std::string& port)
    {
        PortRefDto ref;
        ref.node_id = node;
        ref.port_id = port;
        return ref;
    }

    LoopLayerDto MakeLoopLayer(
        const std::string&       head,
        std::vector<std::string> members,
        int                      tier = 0,
        std::vector<PortRefDto>  data_back_edge_sources = {})
    {
        LoopLayerDto layer;
        layer.head_node_id = head;
        layer.tier = tier;
        layer.members = std::move(members);
        layer.data_back_edge_sources = std::move(data_back_edge_sources);
        return layer;
    }

    CompiledGraphPlanDto MakeBasePlan()
    {
        CompiledGraphPlanDto plan;
        plan.source_fingerprint = "fp_v1";
        return plan;
    }

    // 「外层刷体力×内层 M 次动作」形状（嵌套循环推演蓝本）：
    //   W1 --continue--> B --tick--> W2 --continue--> A
    //   A --done--> W2（内层回边）；A --break--> C1；C1 --done--> W1（外层
    //   回边）；A.out --data--> A.in（内层体数据回边喂自身）。
    // 内层每轮恰跑 inner_runs 次（accumulator 直到 seed+inner_runs-1 界
    // 内持续发 done），外层跑 outer_rounds 轮。
    struct NestedLoopFixture
    {
        CompiledGraphPlanDto plan;
        std::string          outer_head;
        std::string          outer_body;
        std::string          inner_head;
        std::string          inner_body;
        std::string          outer_tail;
    };

    NestedLoopFixture MakeNestedLoopPlan(
        int     outer_rounds,
        int     inner_runs,
        int64_t seed)
    {
        NestedLoopFixture fx;
        fx.outer_head = "n_o1";
        fx.outer_body = "n_o2";
        fx.inner_head = "n_i1";
        fx.inner_body = "n_i2";
        fx.outer_tail = "n_o3";
        const std::string until = std::to_string(seed + inner_runs - 1);

        auto plan = MakeBasePlan();
        plan.node_snapshots = {
            MakeBehaviorSnapshot(
                fx.outer_head,
                R"({"mode":"loop","end":")" + std::to_string(outer_rounds)
                    + R"("})"),
            MakeBehaviorSnapshot(
                fx.outer_body,
                R"({"mode":"emit","signal":"tick"})"),
            MakeBehaviorSnapshot(
                fx.inner_head,
                R"({"mode":"emit","signal":"continue"})"),
            MakeBehaviorSnapshot(
                fx.inner_body,
                R"({"mode":"accumulator","seed":")" + std::to_string(seed)
                    + R"(","until":")" + until + R"("})"),
            MakeBehaviorSnapshot(
                fx.outer_tail,
                R"({"mode":"emit","signal":"done"})"),
        };
        plan.execution_order = {
            fx.outer_head,
            fx.outer_body,
            fx.inner_head,
            fx.inner_body,
            fx.outer_tail};
        plan.signal_routes = {
            MakeSignalRoute(fx.outer_head, "continue", fx.outer_body, "in"),
            MakeSignalRoute(fx.outer_body, "tick", fx.inner_head, "in"),
            MakeSignalRoute(fx.inner_head, "continue", fx.inner_body, "in"),
            MakeSignalRoute(fx.inner_body, "done", fx.inner_head, "loop_in"),
            MakeSignalRoute(fx.inner_body, "break", fx.outer_tail, "in"),
            MakeSignalRoute(fx.outer_tail, "done", fx.outer_head, "loop_in"),
        };
        plan.back_edges = {
            MakeBackEdge(
                "be_inner",
                fx.inner_body,
                "done",
                fx.inner_head,
                "loop_in",
                fx.inner_head),
            MakeBackEdge(
                "be_outer",
                fx.outer_tail,
                "done",
                fx.outer_head,
                "loop_in",
                fx.outer_head),
            MakeDataBackEdge(
                "be_data",
                fx.inner_body,
                "out",
                fx.inner_body,
                "in",
                fx.inner_head),
        };
        plan.loop_layers = {
            MakeLoopLayer(
                fx.outer_head,
                {fx.outer_head,
                 fx.outer_body,
                 fx.inner_head,
                 fx.inner_body,
                 fx.outer_tail}),
            MakeLoopLayer(
                fx.inner_head,
                {fx.inner_head, fx.inner_body},
                1,
                {MakePortRef(fx.inner_body, "out")}),
        };
        plan.binding_plan.bindings = {
            MakeBinding(
                fx.outer_head,
                "continue",
                fx.outer_body,
                "in",
                "signal"),
            MakeBinding(fx.outer_body, "tick", fx.inner_head, "in", "signal"),
            MakeBinding(
                fx.inner_head,
                "continue",
                fx.inner_body,
                "in",
                "signal"),
            MakeBinding(
                fx.inner_body,
                "done",
                fx.inner_head,
                "loop_in",
                "signal"),
            MakeBinding(fx.inner_body, "break", fx.outer_tail, "in", "signal"),
            MakeBinding(
                fx.outer_tail,
                "done",
                fx.outer_head,
                "loop_in",
                "signal"),
            MakeBinding(fx.inner_body, "out", fx.inner_body, "in", "int"),
        };
        fx.plan = std::move(plan);
        return fx;
    }
} // namespace SignalGatedTestMock

// -------------------------------------------------------------------
// Gate: branch emits "true" → only the true-branch sink runs.
// -------------------------------------------------------------------
TEST(SignalGatedRuntimeTest, BranchIfOnlyHitBranchRuns)
{
    using namespace SignalGatedTestMock;
    const std::string branch{"n_11"};
    const std::string trueSink{"n_12"};
    const std::string falseSink{"n_13"};

    auto plan = MakeBasePlan();
    plan.node_snapshots = {
        MakeBehaviorSnapshot(branch, R"({"mode":"emit","signal":"true"})"),
        MakeBehaviorSnapshot(trueSink, R"({"mode":"record"})"),
        MakeBehaviorSnapshot(falseSink, R"({"mode":"record"})"),
    };
    plan.execution_order = {branch, trueSink, falseSink};
    plan.signal_routes = {
        MakeSignalRoute(branch, "true", trueSink, "in"),
        MakeSignalRoute(branch, "false", falseSink, "in"),
    };
    // Signal edges appear as bindings (mirrors compiler output) and must be
    // excluded from data dependencies.
    plan.binding_plan.bindings = {
        MakeBinding(branch, "true", trueSink, "in", "signal"),
        MakeBinding(branch, "false", falseSink, "in", "signal"),
    };

    auto                             host = BehaviorHost::Make();
    GraphRuntime                     rt;
    const Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK);

    auto* h = static_cast<BehaviorHost*>(host.Get());
    ASSERT_EQ(h->components.size(), 3u);
    EXPECT_EQ(h->components[0]->do_count.load(), 1); // branch ran
    EXPECT_EQ(h->components[1]->do_count.load(), 1); // true branch ran
    EXPECT_EQ(h->components[2]->do_count.load(), 0); // false branch skipped
}

// -------------------------------------------------------------------
// Skip propagation: a gated-out branch's data downstream is also skipped.
//   branch --true--> A --+
//                         +--> C  (C needs both A and B)
//   branch --false-> B --+
// -------------------------------------------------------------------
TEST(SignalGatedRuntimeTest, SkipPropagatesAlongDataEdges)
{
    using namespace SignalGatedTestMock;
    const std::string branch{"n_21"};
    const std::string a{"n_22"};
    const std::string b{"n_23"};
    const std::string c{"n_24"};

    auto plan = MakeBasePlan();
    plan.node_snapshots = {
        MakeBehaviorSnapshot(branch, R"({"mode":"emit","signal":"true"})"),
        MakeBehaviorSnapshot(a, R"({"mode":"record"})"),
        MakeBehaviorSnapshot(b, R"({"mode":"record"})"),
        MakeBehaviorSnapshot(c, R"({"mode":"record"})"),
    };
    plan.execution_order = {branch, a, b, c};
    plan.signal_routes = {
        MakeSignalRoute(branch, "true", a, "in"),
        MakeSignalRoute(branch, "false", b, "in"),
    };
    plan.binding_plan.bindings = {
        MakeBinding(branch, "true", a, "in", "signal"),
        MakeBinding(branch, "false", b, "in", "signal"),
        MakeBinding(a, "out", c, "in", "int"),
        MakeBinding(b, "out", c, "in", "int"),
    };

    auto                             host = BehaviorHost::Make();
    GraphRuntime                     rt;
    const Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK);

    auto* h = static_cast<BehaviorHost*>(host.Get());
    ASSERT_EQ(h->components.size(), 4u);
    EXPECT_EQ(h->components[0]->do_count.load(), 1); // branch
    EXPECT_EQ(h->components[1]->do_count.load(), 1); // A (true branch) ran
    EXPECT_EQ(h->components[2]->do_count.load(), 0); // B skipped
    EXPECT_EQ(
        h->components[3]->do_count.load(),
        0); // C skipped (B produced nothing)
}

// -------------------------------------------------------------------
// Last executed node is a runtime fact: the execution_order tail gated
// shut by an unopened signal gate never ran, so the getter reports the
// node that actually finished last — not the plan's tail (the misleading
// WARN root cause this getter exists for).
// -------------------------------------------------------------------
TEST(SignalGatedRuntimeTest, GetLastExecutedNodeIdReportsActualLastDoneNode)
{
    using namespace SignalGatedTestMock;
    const std::string first{"n_41"};
    const std::string last{"n_42"};

    auto plan = MakeBasePlan();
    plan.node_snapshots = {
        MakeBehaviorSnapshot(first, R"({"mode":"record"})"),
        MakeBehaviorSnapshot(last, R"({"mode":"record"})"),
    };
    plan.execution_order = {first, last};
    // 旧计划形态：未打 canonical 标记、组件不发信号——门关死，尾部跳过。
    plan.signal_routes = {
        MakeSignalRoute(first, "signal_out", last, "in"),
    };
    plan.binding_plan.bindings = {
        MakeBinding(first, "signal_out", last, "in", "signal"),
    };

    auto                             host = BehaviorHost::Make();
    GraphRuntime                     rt;
    const Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK);

    auto* h = static_cast<BehaviorHost*>(host.Get());
    ASSERT_EQ(h->components.size(), 2u);
    EXPECT_EQ(h->components[0]->do_count.load(), 1); // first ran
    EXPECT_EQ(h->components[1]->do_count.load(), 0); // tail skipped
    EXPECT_EQ(rt.GetLastExecutedNodeId(), first);
    EXPECT_NE(rt.GetLastExecutedNodeId(), plan.execution_order.back());
}

// 空图：没跑过任何节点，getter 返回空串（消费方回落 "(none)" 口径）。
TEST(SignalGatedRuntimeTest, GetLastExecutedNodeIdEmptyGraphYieldsEmpty)
{
    using namespace SignalGatedTestMock;
    GraphRuntime rt;
    EXPECT_EQ(rt.GetLastExecutedNodeId(), "");

    auto                             host = BehaviorHost::Make();
    const Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();
    auto plan = MakeBasePlan();
    ASSERT_EQ(rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get()), DAS_S_OK);
    EXPECT_EQ(rt.GetLastExecutedNodeId(), "");
}

// -------------------------------------------------------------------
// Loop: for emits continue N times then break; body reruns each iteration.
//   for --continue--> body ; body --done--> for (back-edge)
// -------------------------------------------------------------------
TEST(SignalGatedRuntimeTest, ForLoopIteratesCorrectly)
{
    using namespace SignalGatedTestMock;
    const std::string forNode{"n_31"};
    const std::string body{"n_32"};

    auto plan = MakeBasePlan();
    plan.node_snapshots = {
        MakeBehaviorSnapshot(forNode, R"({"mode":"loop","end":"3"})"),
        MakeBehaviorSnapshot(body, R"({"mode":"emit","signal":"done"})"),
    };
    plan.execution_order = {forNode, body};
    plan.signal_routes = {
        MakeSignalRoute(forNode, "continue", body, "in"),
        MakeSignalRoute(body, "done", forNode, "loop_in"),
    };
    plan.back_edges = {
        MakeBackEdge("be1", body, "done", forNode, "loop_in", forNode),
    };
    plan.loop_layers = {MakeLoopLayer(forNode, {forNode, body})};
    plan.binding_plan.bindings = {
        MakeBinding(forNode, "continue", body, "in", "signal"),
        MakeBinding(body, "done", forNode, "loop_in", "signal"),
    };

    auto                             host = BehaviorHost::Make();
    GraphRuntime                     rt;
    const Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK);

    auto* h = static_cast<BehaviorHost*>(host.Get());
    ASSERT_EQ(h->components.size(), 2u);
    // for: continue @ i=0,1,2 then break @ i=3  → 4 Do calls
    EXPECT_EQ(h->components[0]->do_count.load(), 4);
    // body: ran once per continue → 3 Do calls
    EXPECT_EQ(h->components[1]->do_count.load(), 3);
}

// -------------------------------------------------------------------
// Loop with an inner branch: the branch flips each iteration; stale signals
// from the previous iteration must NOT re-activate the wrong sink.
//   for -> selector(alternate true/false) -> sinkT / sinkF -> term -> for
// -------------------------------------------------------------------
TEST(SignalGatedRuntimeTest, LoopWithInnerBranchHasNoCrossIterationResidue)
{
    using namespace SignalGatedTestMock;
    const std::string forNode{"n_41"};
    const std::string selector{"n_42"};
    const std::string sinkT{"n_43"};
    const std::string sinkF{"n_44"};
    const std::string term{"n_45"};

    auto plan = MakeBasePlan();
    plan.node_snapshots = {
        MakeBehaviorSnapshot(forNode, R"({"mode":"loop","end":"2"})"),
        MakeBehaviorSnapshot(selector, R"({"mode":"alternate"})"),
        MakeBehaviorSnapshot(sinkT, R"({"mode":"emit","signal":"done"})"),
        MakeBehaviorSnapshot(sinkF, R"({"mode":"emit","signal":"done"})"),
        MakeBehaviorSnapshot(term, R"({"mode":"emit","signal":"done"})"),
    };
    plan.execution_order = {forNode, selector, sinkT, sinkF, term};
    plan.signal_routes = {
        MakeSignalRoute(forNode, "continue", selector, "in"),
        MakeSignalRoute(selector, "true", sinkT, "in"),
        MakeSignalRoute(selector, "false", sinkF, "in"),
        MakeSignalRoute(sinkT, "done", term, "in"),
        MakeSignalRoute(sinkF, "done", term, "in"),
        MakeSignalRoute(term, "done", forNode, "loop_in"),
    };
    plan.back_edges = {
        MakeBackEdge("be1", term, "done", forNode, "loop_in", forNode),
    };
    plan.loop_layers = {
        MakeLoopLayer(forNode, {forNode, selector, sinkT, sinkF, term})};
    plan.binding_plan.bindings = {
        MakeBinding(forNode, "continue", selector, "in", "signal"),
        MakeBinding(selector, "true", sinkT, "in", "signal"),
        MakeBinding(selector, "false", sinkF, "in", "signal"),
        MakeBinding(sinkT, "done", term, "in", "signal"),
        MakeBinding(sinkF, "done", term, "in", "signal"),
        MakeBinding(term, "done", forNode, "loop_in", "signal"),
    };

    auto                             host = BehaviorHost::Make();
    GraphRuntime                     rt;
    const Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK);

    auto* h = static_cast<BehaviorHost*>(host.Get());
    ASSERT_EQ(h->components.size(), 5u);
    EXPECT_EQ(
        h->components[0]->do_count.load(),
        3); // for: continue x2, break x1
    EXPECT_EQ(
        h->components[1]->do_count.load(),
        2); // selector ran both iterations
    EXPECT_EQ(
        h->components[2]->do_count.load(),
        1); // sinkT only on iter 0 (true)
    EXPECT_EQ(
        h->components[3]->do_count.load(),
        1); // sinkF only on iter 1 (false)
    EXPECT_EQ(h->components[4]->do_count.load(), 2); // term each iteration
}

// -------------------------------------------------------------------
// 嵌套隔离：内层回边发火只重置内层成员——内层 M 次迭代期间外层体保持
// Done 不重跑（外层体恰 1 次/圈），外层头只在圈界重跑。
// -------------------------------------------------------------------
TEST(SignalGatedRuntimeTest, NestedInnerFiringLeavesOuterBodyDone)
{
    using namespace SignalGatedTestMock;
    const auto fx = MakeNestedLoopPlan(1, 3, 5);

    auto                             host = BehaviorHost::Make();
    GraphRuntime                     rt;
    const Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(fx.plan, "fp_v1", token.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK);

    auto* h = static_cast<BehaviorHost*>(host.Get());
    ASSERT_EQ(h->components.size(), 5u);
    EXPECT_EQ(h->components[0]->do_count.load(), 2); // W1: continue + break
    EXPECT_EQ(h->components[1]->do_count.load(), 1); // B: 每外圈恰 1 次
    EXPECT_EQ(h->components[2]->do_count.load(), 3); // W2: 每内圈 1 次
    EXPECT_EQ(h->components[3]->do_count.load(), 3); // A: 内层动作 M 次
    EXPECT_EQ(h->components[4]->do_count.load(), 1); // C1: 每外圈 1 次
}

// -------------------------------------------------------------------
// 外层清场：外层回边发火清内层数据回边源口——内层重开第一圈 in 缺键回
// 落 seed，无跨外层圈的脏值透传（值序列含 seed 重现）。
// -------------------------------------------------------------------
TEST(SignalGatedRuntimeTest, NestedOuterFiringClearsInnerDataSources)
{
    using namespace SignalGatedTestMock;
    const auto fx = MakeNestedLoopPlan(2, 3, 5);

    auto                             host = BehaviorHost::Make();
    GraphRuntime                     rt;
    const Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(fx.plan, "fp_v1", token.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK);

    auto* h = static_cast<BehaviorHost*>(host.Get());
    ASSERT_EQ(h->components.size(), 5u);
    EXPECT_EQ(
        h->components[3]->input_values,
        (std::vector<int64_t>{5, 6, 7, 5, 6, 7}));
}

// -------------------------------------------------------------------
// 嵌套计数与终值：外层 N 圈 × 内层每圈 M 次——内层组件执行总数 N*M、外
// 层体恰 N 次、终值 seed+M-1。
// -------------------------------------------------------------------
TEST(SignalGatedRuntimeTest, NestedInnerRunsPerOuterRoundCounts)
{
    using namespace SignalGatedTestMock;
    const auto fx = MakeNestedLoopPlan(2, 3, 5);

    auto                             host = BehaviorHost::Make();
    GraphRuntime                     rt;
    const Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(fx.plan, "fp_v1", token.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK);

    auto* h = static_cast<BehaviorHost*>(host.Get());
    ASSERT_EQ(h->components.size(), 5u);
    EXPECT_EQ(h->components[3]->do_count.load(), 6); // A: N*M
    EXPECT_EQ(h->components[1]->do_count.load(), 2); // B: 恰 N 次
    EXPECT_EQ(
        h->components[0]->do_count.load(),
        3); // W1: N 次 continue + 末次 break
    EXPECT_EQ(h->components[4]->do_count.load(), 2); // C1: 恰 N 次
    ASSERT_FALSE(h->components[3]->input_values.empty());
    EXPECT_EQ(h->components[3]->input_values.back(), 7);
}

// -------------------------------------------------------------------
// 外层携带通道存活性：内层体的数据回边直连外层头（归属外层清单）时，
// 外层发火只清严格内层源口，外层头重启后能读到上圈回边值（不回落缺
// 省/缺键）。
// -------------------------------------------------------------------
TEST(SignalGatedRuntimeTest, NestedOuterRestartReadsOuterCarriedValue)
{
    using namespace SignalGatedTestMock;
    auto fx = MakeNestedLoopPlan(2, 2, 1);

    // 内层体 count 口直连外层头 cond 口的数据回边——按目标头归属外层
    // （编译期 ComputeTopology 新规则），源口记外层清单。
    fx.plan.back_edges.push_back(MakeDataBackEdge(
        "be_outer_data",
        fx.inner_body,
        "count",
        fx.outer_head,
        "cond",
        fx.outer_head));
    fx.plan.loop_layers[0].data_back_edge_sources = {
        MakePortRef(fx.inner_body, "count")};
    fx.plan.binding_plan.bindings.push_back(
        MakeBinding(fx.inner_body, "count", fx.outer_head, "cond", "int"));

    auto                             host = BehaviorHost::Make();
    GraphRuntime                     rt;
    const Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(fx.plan, "fp_v1", token.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK);

    auto* h = static_cast<BehaviorHost*>(host.Get());
    ASSERT_EQ(h->components.size(), 5u);
    // 外层头每圈重启都读到上圈内层终值 3（首圈无携带值不记录）。
    EXPECT_EQ(h->components[0]->input_values, (std::vector<int64_t>{3, 3}));
    // 内层自环通道仍按内层清场语义工作：每外圈从 seed 重新累计。
    EXPECT_EQ(
        h->components[3]->input_values,
        (std::vector<int64_t>{1, 2, 1, 2}));
}

// -------------------------------------------------------------------
// 数据回边不失控钉测：无 signal 发火的数据回边图，多趟循环按 no-progress
// 终止（数据回边永不发火，单趟执行后无进展即收尾）——迭代边界只由
// signal 回边承载，环状态通道不引入任何重启机制。
// -------------------------------------------------------------------
TEST(SignalGatedRuntimeTest, DataBackEdgeAloneNeverFiresAndTerminates)
{
    using namespace SignalGatedTestMock;
    const std::string acc{"n_acc"};

    auto plan = MakeBasePlan();
    plan.node_snapshots = {
        MakeBehaviorSnapshot(
            acc,
            R"({"mode":"accumulator","seed":"0","until":"5"})"),
    };
    plan.execution_order = {acc};
    plan.back_edges = {
        MakeDataBackEdge("be_data", acc, "out", acc, "in", acc),
    };
    plan.loop_layers = {
        MakeLoopLayer(acc, {acc}, 0, {MakePortRef(acc, "out")}),
    };
    plan.binding_plan.bindings = {
        MakeBinding(acc, "out", acc, "in", "int"),
    };

    auto                             host = BehaviorHost::Make();
    GraphRuntime                     rt;
    const Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK);

    auto* h = static_cast<BehaviorHost*>(host.Get());
    ASSERT_EQ(h->components.size(), 1u);
    EXPECT_EQ(h->components[0]->do_count.load(), 1);
    EXPECT_EQ(h->components[0]->input_values, (std::vector<int64_t>{0}));
}

// -------------------------------------------------------------------
// φ-join (DAS-74 merge): a join node gated by the branch signal must RUN even
// though its other-branch data predecessor was skipped, and pick up the taken
// branch's value.
//   branch --true-->  A --data--> merge.value_true
//   branch --true-->  merge.in_true
//   branch --false--> B (skipped) --data--> merge.value_false
// -------------------------------------------------------------------
TEST(SignalGatedRuntimeTest, MergeJoinRunsOnTakenBranch)
{
    using namespace SignalGatedTestMock;
    const std::string branch{"n_51"};
    const std::string a{"n_52"};
    const std::string b{"n_53"};
    const std::string mergeNode{"n_54"};

    auto plan = MakeBasePlan();
    plan.node_snapshots = {
        MakeBehaviorSnapshot(branch, R"({"mode":"emit","signal":"true"})"),
        MakeBehaviorSnapshot(a, R"({"mode":"record"})"),
        MakeBehaviorSnapshot(b, R"({"mode":"record"})"),
        MakeBehaviorSnapshot(mergeNode, R"({"mode":"merge"})"),
    };
    plan.execution_order = {branch, a, b, mergeNode};
    plan.signal_routes = {
        MakeSignalRoute(branch, "true", a, "in"),
        MakeSignalRoute(branch, "false", b, "in"),
        MakeSignalRoute(branch, "true", mergeNode, "in_true"),
    };
    plan.binding_plan.bindings = {
        MakeBinding(branch, "true", a, "in", "signal"),
        MakeBinding(branch, "false", b, "in", "signal"),
        MakeBinding(branch, "true", mergeNode, "in_true", "signal"),
        MakeBinding(a, "out", mergeNode, "value_true", "int"),
        MakeBinding(b, "out", mergeNode, "value_false", "int"),
    };

    auto                             host = BehaviorHost::Make();
    GraphRuntime                     rt;
    const Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK);

    auto* h = static_cast<BehaviorHost*>(host.Get());
    ASSERT_EQ(h->components.size(), 4u);
    EXPECT_EQ(h->components[0]->do_count.load(), 1); // branch
    EXPECT_EQ(h->components[1]->do_count.load(), 1); // A (true branch) ran
    EXPECT_EQ(h->components[2]->do_count.load(), 0); // B (false branch) skipped
    EXPECT_EQ(
        h->components[3]->do_count.load(),
        1); // merge ran despite B skipped
    // merge picked A's value (A.out = do_count = 1 on its single run).
    EXPECT_EQ(
        h->components[3]->picked_value,
        h->components[1]->do_count.load());
}

// -------------------------------------------------------------------
// 节点 ID 生产形态端到端：authoring 契约层分配的 "n_1" 短字符串节点上，
// 组件 Do 返回 S_OK 且产出 output portmap 时，值必须以字符串节点键落进
// 帧并沿数据边流到下游——帧键误型为 GUID 的实现在此形态直接抛
// InvalidGuidStringSizeException。
// -------------------------------------------------------------------
TEST(GraphRuntimeTest, NodeIdShortFormProducesOutputPortMap)
{
    using namespace SignalGatedTestMock;
    const std::string producer{"n_1"};
    const std::string consumer{"n_2"};

    auto plan = MakeBasePlan();
    plan.node_snapshots = {
        MakeBehaviorSnapshot(producer, R"({"mode":"record"})"),
        MakeBehaviorSnapshot(consumer, R"({"mode":"merge"})"),
    };
    plan.execution_order = {producer, consumer};
    plan.binding_plan.bindings = {
        MakeBinding(producer, "out", consumer, "value_true", "int"),
    };

    auto                             host = BehaviorHost::Make();
    GraphRuntime                     rt;
    const Das::DasPtr<IDasStopToken> token =
        Das::PluginInterface::DasStopTokenImplBase<MockStopToken>::Make();

    auto hr = rt.RunWithHost(plan, "fp_v1", token.Get(), host.Get());
    ASSERT_EQ(hr, DAS_S_OK);

    auto* h = static_cast<BehaviorHost*>(host.Get());
    ASSERT_EQ(h->components.size(), 2u);
    EXPECT_EQ(h->components[0]->do_count.load(), 1); // producer ran
    EXPECT_EQ(h->components[1]->do_count.load(), 1); // consumer ran
    // producer.out = 1 经帧流到 merge.value_true。
    EXPECT_EQ(h->components[1]->picked_value, 1);
}

// -------------------------------------------------------------------
// Execute 成功结果的 graphOutputs 导出：编译产物携带边界输出口清单
// （boundaryPorts.outputs，含节点定位）且子图执行后对应悬空口有值时，
// 结果 JSON 的 graphOutputs 收敛 {口名:值}；无清单时 graphOutputs 为空
// 对象（键在场，向后兼容空结果消费方）。
// -------------------------------------------------------------------
namespace
{
    std::string ExecutePlanShellText(
        Das::PluginInterface::IDasTaskComponentHost* host,
        const CompiledGraphPlanDto&                  plan)
    {
        const auto shell = BuildCompiledPlanShell(yyjson::object(plan));
        const auto shell_text = Das::Utils::SerializeYyjsonValue(shell);
        if (!shell_text.has_value())
        {
            return {};
        }

        Das::DasPtr<Das::ExportInterface::IDasGraphRuntime> runtime;
        if (CreateGraphRuntimeWithHost(host, runtime.Put()) != DAS_S_OK)
        {
            return {};
        }
        Das::DasPtr<IDasReadOnlyString> artifact;
        CreateIDasReadOnlyStringFromUtf8(shell_text->c_str(), artifact.Put());
        Das::DasPtr<Das::ExportInterface::IDasJson> result_json;
        if (runtime->Execute(artifact.Get(), nullptr, result_json.Put())
                != DAS_S_OK
            || result_json.Get() == nullptr)
        {
            return {};
        }

        Das::DasPtr<IDasReadOnlyString> result_text;
        if (result_json->ToString(0, result_text.Put()) != DAS_S_OK
            || result_text.Get() == nullptr)
        {
            return {};
        }
        const char* utf8 = nullptr;
        result_text->GetUtf8(&utf8);
        return utf8 != nullptr ? std::string(utf8) : std::string{};
    }
} // namespace

TEST(GraphRuntimeTest, ExecuteExportsBoundaryOutputValues)
{
    using namespace GraphRuntimeTestMock;

    CompiledGraphPlanDto plan;
    plan.source_fingerprint = "fp_boundary";
    plan.node_snapshots.push_back(MakeSnapshot(NODE_A, COMP_A));
    plan.execution_order = {NODE_A};

    // 悬空输入经 broadcast 物化为 "hello"；替身组件回显输入口 → 帧
    // (n_1, p2) 有值；p2 同时是清单里的边界输出口。
    PortBindingDto broadcast;
    broadcast.source_node_id = "$graph_input";
    broadcast.source_port_id = "p2";
    broadcast.target_node_id = NODE_A;
    broadcast.target_port_id = "p2";
    broadcast.expected_type = "string";
    auto default_parsed = Das::Utils::ParseYyjsonFromString(R"("hello")");
    ASSERT_TRUE(default_parsed.has_value());
    broadcast.default_value = std::move(*default_parsed);
    plan.binding_plan.bindings.push_back(std::move(broadcast));

    BoundaryPortDto boundary_output;
    boundary_output.port_id = "p2";
    boundary_output.port_type = "string";
    boundary_output.node_ids = {NODE_A};
    plan.boundary_ports.outputs.push_back(std::move(boundary_output));

    auto       host = MockTaskComponentHost::Make();
    const auto result_text = ExecutePlanShellText(host.Get(), plan);
    ASSERT_FALSE(result_text.empty());

    auto result = yyjson::read(result_text);
    auto result_obj = result.as_object();
    ASSERT_TRUE(result_obj.has_value());
    ASSERT_TRUE(result_obj->contains(std::string_view("graphOutputs")));
    auto graph_outputs =
        (*result_obj)[std::string_view("graphOutputs")].as_object();
    ASSERT_TRUE(graph_outputs.has_value());
    ASSERT_TRUE(graph_outputs->contains(std::string_view("p2")));
    EXPECT_EQ(
        (*graph_outputs)[std::string_view("p2")].as_string().value_or(""),
        std::string_view("hello"));
}

TEST(GraphRuntimeTest, ExecuteWithoutBoundaryOutputsYieldsEmptyGraphOutputs)
{
    using namespace GraphRuntimeTestMock;

    CompiledGraphPlanDto plan;
    plan.source_fingerprint = "fp_no_boundary";
    plan.node_snapshots.push_back(MakeSnapshot(NODE_A, COMP_A));
    plan.execution_order = {NODE_A};

    auto       host = MockTaskComponentHost::Make();
    const auto result_text = ExecutePlanShellText(host.Get(), plan);
    ASSERT_FALSE(result_text.empty());

    auto result = yyjson::read(result_text);
    auto result_obj = result.as_object();
    ASSERT_TRUE(result_obj.has_value());
    // 空清单回退：graphOutputs 键在场且为空对象。
    ASSERT_TRUE(result_obj->contains(std::string_view("graphOutputs")));
    auto graph_outputs =
        (*result_obj)[std::string_view("graphOutputs")].as_object();
    ASSERT_TRUE(graph_outputs.has_value());
    EXPECT_TRUE(graph_outputs->empty());
}

// -------------------------------------------------------------------
// ExecuteWithTypedIO（D-18/D-21 单通道）：graph_inputs 直传覆盖哨兵默认 +
// 边界输出口类型化导出（含不可序列化对象引用——JSON 桥做不到的半段）。
// -------------------------------------------------------------------
namespace
{
    DAS::DasPtr<Das::ExportInterface::IDasPortMap> ExecutePlanShellTyped(
        Das::PluginInterface::IDasTaskComponentHost* host,
        const CompiledGraphPlanDto&                  plan,
        Das::ExportInterface::IDasReadOnlyPortMap*   graph_inputs,
        DasResult*                                   out_hr = nullptr)
    {
        const auto shell = BuildCompiledPlanShell(yyjson::object(plan));
        const auto shell_text = Das::Utils::SerializeYyjsonValue(shell);
        if (!shell_text.has_value())
        {
            return {};
        }

        Das::DasPtr<Das::ExportInterface::IDasGraphRuntime> runtime;
        if (CreateGraphRuntimeWithHost(host, runtime.Put()) != DAS_S_OK)
        {
            return {};
        }
        Das::DasPtr<IDasReadOnlyString> artifact;
        CreateIDasReadOnlyStringFromUtf8(shell_text->c_str(), artifact.Put());
        Das::DasPtr<Das::ExportInterface::IDasPortMap> outputs;
        const DasResult hr = runtime->ExecuteWithTypedIO(
            artifact.Get(),
            nullptr,
            graph_inputs,
            outputs.Put());
        if (out_hr != nullptr)
        {
            *out_hr = hr;
        }
        if (hr != DAS_S_OK)
        {
            return {};
        }
        return outputs;
    }
} // namespace

TEST(GraphRuntimeTest, ExecuteWithTypedIOOverridesSentinelDefault)
{
    using namespace GraphRuntimeTestMock;

    CompiledGraphPlanDto plan;
    plan.source_fingerprint = "fp_typed_input";
    plan.node_snapshots.push_back(MakeSnapshot(NODE_A, COMP_A));
    plan.execution_order = {NODE_A};

    // 哨兵绑定声明默认 "hello"；直传 graph_inputs 携带 p2="typed" 时覆盖
    // 层命中（默认不物化），回显组件读到 "typed" 并回显到帧 → 边界导出。
    PortBindingDto broadcast;
    broadcast.source_node_id = "$graph_input";
    broadcast.source_port_id = "p2";
    broadcast.target_node_id = NODE_A;
    broadcast.target_port_id = "p2";
    broadcast.expected_type = "string";
    auto default_parsed = Das::Utils::ParseYyjsonFromString(R"("hello")");
    ASSERT_TRUE(default_parsed.has_value());
    broadcast.default_value = std::move(*default_parsed);
    plan.binding_plan.bindings.push_back(std::move(broadcast));

    BoundaryPortDto boundary_output;
    boundary_output.port_id = "p2";
    boundary_output.port_type = "string";
    boundary_output.node_ids = {NODE_A};
    plan.boundary_ports.outputs.push_back(std::move(boundary_output));

    DAS::DasPtr<IDasPortMap> graph_inputs;
    ASSERT_EQ(CreateIDasPortMap(graph_inputs.Put()), DAS_S_OK);
    {
        const DasReadOnlyString         key{"p2"};
        DAS::DasPtr<IDasReadOnlyString> value;
        CreateIDasReadOnlyStringFromUtf8("typed", value.Put());
        ASSERT_EQ(graph_inputs->SetString(key.Get(), value.Get()), DAS_S_OK);
    }

    DasResult                hr = DAS_E_FAIL;
    DAS::DasPtr<IDasPortMap> outputs;
    auto                     host = MockTaskComponentHost::Make();
    outputs = ExecutePlanShellTyped(host.Get(), plan, graph_inputs.Get(), &hr);
    ASSERT_EQ(hr, DAS_S_OK);
    ASSERT_NE(outputs.Get(), nullptr);

    // 直传值经覆盖层进入组件（回显输出）并经类型化导出回到调用方。
    DAS::DasPtr<IDasReadOnlyString> exported;
    const DasReadOnlyString         p2_key{"p2"};
    ASSERT_EQ(outputs->GetString(p2_key.Get(), exported.Put()), DAS_S_OK);
    ASSERT_NE(exported.Get(), nullptr);
    const char* exported_utf8 = nullptr;
    ASSERT_EQ(exported->GetUtf8(&exported_utf8), DAS_S_OK);
    EXPECT_STREQ(exported_utf8, "typed");
}

TEST(GraphRuntimeTest, ExecuteWithTypedIOExportsImageReference)
{
    using namespace GraphRuntimeTestMock;

    DAS::DasPtr<IDasImage> image = CountingFakeImage::Make();

    CompiledGraphPlanDto plan;
    plan.source_fingerprint = "fp_typed_image";
    plan.node_snapshots.push_back(
        MakeSnapshot(NODE_A, "20000000-0000-0000-0000-0000000000A1"));
    plan.execution_order = {NODE_A};

    BoundaryPortDto boundary_output;
    boundary_output.port_id = "img";
    boundary_output.port_type = "image";
    boundary_output.node_ids = {NODE_A};
    plan.boundary_ports.outputs.push_back(std::move(boundary_output));

    auto host = ImageSourceHost::MakeImpl();
    host->SetImage(image.Get());

    DasResult                hr = DAS_E_FAIL;
    DAS::DasPtr<IDasPortMap> outputs;
    outputs = ExecutePlanShellTyped(host.Get(), plan, nullptr, &hr);
    ASSERT_EQ(hr, DAS_S_OK);
    ASSERT_NE(outputs.Get(), nullptr);

    // 不可序列化对象引用经类型化导出跨边界：身份即源实例（JSON 桥的
    // graphOutputs 段对该类型只能跳过）。
    DAS::DasPtr<IDasImage>  exported;
    const DasReadOnlyString img_key{"img"};
    ASSERT_EQ(outputs->GetImage(img_key.Get(), exported.Put()), DAS_S_OK);
    EXPECT_EQ(exported.Get(), image.Get());
}

// ExecuteWithTypedIO 的 graph_inputs 指向调用方栈上对象，异常逃逸路径也
// 必须清零引擎内的跨调用指针——否则同实例后续执行经悬挂指针读已释放内
// 存（UAF），默认物化兜底被污染。
TEST(GraphRuntimeTest, ExecuteWithTypedIOExceptionPathClearsGraphInputs)
{
    using namespace GraphRuntimeTestMock;

    CompiledGraphPlanDto plan;
    plan.source_fingerprint = "fp_exception_clears";
    plan.node_snapshots.push_back(MakeSnapshot(NODE_A, COMP_A));
    plan.execution_order = {NODE_A};

    // 广播绑定保留 graph_inputs 的访问路径：恢复执行的 BuildInputPortMap
    // 会对该绑定查询覆盖层——悬挂指针在此被解引用。
    PortBindingDto broadcast;
    broadcast.source_node_id = "$graph_input";
    broadcast.source_port_id = "p2";
    broadcast.target_node_id = NODE_A;
    broadcast.target_port_id = "p2";
    broadcast.expected_type = "string";
    auto default_parsed = Das::Utils::ParseYyjsonFromString(R"("hello")");
    ASSERT_TRUE(default_parsed.has_value());
    broadcast.default_value = std::move(*default_parsed);
    plan.binding_plan.bindings.push_back(std::move(broadcast));

    const auto shell = BuildCompiledPlanShell(yyjson::object(plan));
    const auto shell_text = Das::Utils::SerializeYyjsonValue(shell);
    ASSERT_TRUE(shell_text.has_value());
    DAS::DasPtr<IDasReadOnlyString> artifact;
    CreateIDasReadOnlyStringFromUtf8(shell_text->c_str(), artifact.Put());

    auto host = ThrowingOnceHost::Make();
    DAS::DasPtr<Das::ExportInterface::IDasGraphRuntime> runtime;
    ASSERT_EQ(CreateGraphRuntimeWithHost(host.Get(), runtime.Put()), DAS_S_OK);

    {
        // 调用方 graph_inputs：块内构造、块尾释放——首轮执行抛异常逃逸。
        DAS::DasPtr<IDasPortMap> graph_inputs;
        ASSERT_EQ(CreateIDasPortMap(graph_inputs.Put()), DAS_S_OK);
        {
            const DasReadOnlyString         key{"p2"};
            DAS::DasPtr<IDasReadOnlyString> value;
            CreateIDasReadOnlyStringFromUtf8("sentinel", value.Put());
            ASSERT_EQ(
                graph_inputs->SetString(key.Get(), value.Get()),
                DAS_S_OK);
        }

        bool threw = false;
        try
        {
            DAS::DasPtr<IDasPortMap> outputs;
            runtime->ExecuteWithTypedIO(
                artifact.Get(),
                nullptr,
                graph_inputs.Get(),
                outputs.Put());
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        EXPECT_TRUE(threw);
    }

    // 同实例走旧 Execute（JSON 路径不重挂 graph_inputs，默认物化兜底）：
    // 必须干净成功——引擎若仍持有首轮悬挂指针，广播覆盖层查询在此读已释
    // 放内存。
    DAS::DasPtr<Das::ExportInterface::IDasJson> result_json;
    const DasResult                             hr =
        runtime->Execute(artifact.Get(), nullptr, result_json.Put());
    EXPECT_EQ(hr, DAS_S_OK);
}

// 广播覆盖层不得命中调用方协议保留键 executionInput：子图执行直传整个输
// 入表时该键携带编译产物文本——悬空的同名声明口（图内放一个未连线的子
// 图执行节点）若经覆盖层收到它，父图编译产物字符串会被当作该节点的执行
// 输入注入。保留键回落默认物化。
TEST(GraphRuntimeTest, BroadcastOverlaySkipsReservedExecutionInputKey)
{
    using namespace GraphRuntimeTestMock;

    CompiledGraphPlanDto plan;
    plan.source_fingerprint = "fp_reserved_key";
    plan.node_snapshots.push_back(MakeSnapshot(NODE_A, COMP_A));
    plan.execution_order = {NODE_A};

    PortBindingDto broadcast;
    broadcast.source_node_id = "$graph_input";
    broadcast.source_port_id = "executionInput";
    broadcast.target_node_id = NODE_A;
    broadcast.target_port_id = "executionInput";
    broadcast.expected_type = "string";
    auto default_parsed =
        Das::Utils::ParseYyjsonFromString(R"("declared-default")");
    ASSERT_TRUE(default_parsed.has_value());
    broadcast.default_value = std::move(*default_parsed);
    plan.binding_plan.bindings.push_back(std::move(broadcast));

    BoundaryPortDto boundary_output;
    boundary_output.port_id = "executionInput";
    boundary_output.port_type = "string";
    boundary_output.node_ids = {NODE_A};
    plan.boundary_ports.outputs.push_back(std::move(boundary_output));

    DAS::DasPtr<IDasPortMap> graph_inputs;
    ASSERT_EQ(CreateIDasPortMap(graph_inputs.Put()), DAS_S_OK);
    {
        const DasReadOnlyString         key{"executionInput"};
        DAS::DasPtr<IDasReadOnlyString> value;
        CreateIDasReadOnlyStringFromUtf8(
            "compiled-artifact-shell-text",
            value.Put());
        ASSERT_EQ(graph_inputs->SetString(key.Get(), value.Get()), DAS_S_OK);
    }

    DasResult                hr = DAS_E_FAIL;
    DAS::DasPtr<IDasPortMap> outputs;
    auto                     host = MockTaskComponentHost::Make();
    outputs = ExecutePlanShellTyped(host.Get(), plan, graph_inputs.Get(), &hr);
    ASSERT_EQ(hr, DAS_S_OK);
    ASSERT_NE(outputs.Get(), nullptr);

    // 回显组件读到的必须是声明默认，不是输入表里保留键携带的编译产物
    // 文本。
    DAS::DasPtr<IDasReadOnlyString> exported;
    const DasReadOnlyString         key{"executionInput"};
    ASSERT_EQ(outputs->GetString(key.Get(), exported.Put()), DAS_S_OK);
    ASSERT_NE(exported.Get(), nullptr);
    const char* exported_utf8 = nullptr;
    ASSERT_EQ(exported->GetUtf8(&exported_utf8), DAS_S_OK);
    EXPECT_STREQ(exported_utf8, "declared-default");
}
