#define DAS_BUILD_SHARED

#include "FlowControlTaskComponents.h"

#include <das/Core/TaskScheduler/RepositoryInvokeDtos.h>
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>
#include <das/_autogen/idl/abi/IDasPortMap.h>
#include <das/_autogen/idl/header/IDasPortMap.generated.h>

#include <array>
#include <new>
#include <optional>
#include <utility>

DAS_NS_BEGIN

namespace
{
    struct ComponentSpec
    {
        DasGuid          guid;
        std::string_view guid_text;
        std::string_view kind;
    };

    namespace RepositoryInvokeDto =
        Das::Core::TaskScheduler::RepositoryInvoke::Dto;

    constexpr std::string_view kRepositoryInvokeKind =
        "das.flow.invokeRepository";

    constexpr std::string_view kBranchKind = "das.flow.branch";
    constexpr std::string_view kForKind = "das.flow.for";
    constexpr std::string_view kWhileKind = "das.flow.while";
    constexpr std::string_view kMergeKind = "das.flow.merge";

    constexpr std::array<ComponentSpec, 6> kComponents{
        ComponentSpec{
            DasGuid{
                0x68f10001,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}},
            "68F10001-0000-4000-8000-000000000001",
            "das.flow.branch"},
        ComponentSpec{
            DasGuid{
                0x68f10003,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03}},
            "68F10003-0000-4000-8000-000000000003",
            "das.flow.delay"},
        ComponentSpec{
            DasGuid{
                0x68f10004,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04}},
            "68F10004-0000-4000-8000-000000000004",
            "das.flow.for"},
        ComponentSpec{
            DasGuid{
                0x68f10005,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05}},
            "68F10005-0000-4000-8000-000000000005",
            "das.flow.while"},
        ComponentSpec{
            DasGuid{
                0x68f10007,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07}},
            "68F10007-0000-4000-8000-000000000007",
            "das.flow.invokeRepository"},
        ComponentSpec{
            DasGuid{
                0x68f10008,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08}},
            "68F10008-0000-4000-8000-000000000008",
            "das.flow.merge"},
    };

    std::optional<ComponentSpec> FindSpec(const DasGuid& component_guid)
    {
        for (const auto& spec : kComponents)
        {
            if (spec.guid == component_guid)
            {
                return spec;
            }
        }
        return std::nullopt;
    }

    yyjson::value CloneJson(const yyjson::value& value)
    {
        const auto serialized = value.write(yyjson::WriteFlag::NoFlag);
        auto       parsed = Das::Utils::ParseYyjsonFromString(
            std::string_view(serialized.data(), serialized.size()));
        return parsed ? std::move(*parsed) : yyjson::value{};
    }

    DasPtr<ExportInterface::IDasJson> WrapJson(yyjson::value value)
    {
        auto serialized = Das::Utils::SerializeYyjsonValue(value);
        if (!serialized)
        {
            return {};
        }

        DasPtr<ExportInterface::IDasJson> result;
        ParseDasJsonFromString(serialized->c_str(), result.Put());
        return result;
    }

    /// Write a string value into a PortMap port.
    void SetPortString(
        ExportInterface::IDasPortMap* map,
        std::string_view              port_id,
        std::string_view              value)
    {
        DasReadOnlyString key{std::string{port_id}.c_str()};
        DasReadOnlyString val{std::string{value}.c_str()};
        map->SetString(key.Get(), val.Get());
    }

    /// Write a JSON value (as serialized string) into a PortMap port.
    void SetPortJson(
        ExportInterface::IDasPortMap* map,
        std::string_view              port_id,
        const yyjson::value&          value)
    {
        auto serialized = Das::Utils::SerializeYyjsonValue(value);
        if (serialized)
        {
            DasReadOnlyString key{std::string{port_id}.c_str()};
            DasReadOnlyString val{serialized->c_str()};
            map->SetString(key.Get(), val.Get());
        }
    }

    /// Read a JSON value from a PortMap string port.
    std::optional<yyjson::value> GetPortJson(
        ExportInterface::IDasReadOnlyPortMap* map,
        std::string_view                      port_id)
    {
        IDasReadOnlyString* p_str = nullptr;
        DasReadOnlyString   key{std::string{port_id}.c_str()};
        auto                hr = map->GetString(key.Get(), &p_str);
        if (DAS::IsFailed(hr) || p_str == nullptr)
        {
            return std::nullopt;
        }

        const char* utf8 = nullptr;
        p_str->GetUtf8(&utf8);
        std::string str_val(utf8 ? utf8 : "");
        p_str->Release();

        if (str_val.empty())
        {
            return std::nullopt;
        }

        return Das::Utils::ParseYyjsonFromString(str_val);
    }

    /// Read a bool from a PortMap port.
    bool GetPortBool(
        ExportInterface::IDasReadOnlyPortMap* map,
        std::string_view                      port_id,
        bool                                  default_val = false)
    {
        bool              val = default_val;
        DasReadOnlyString key{std::string{port_id}.c_str()};
        auto              hr = map->GetBool(key.Get(), &val);
        return DAS::IsOk(hr) ? val : default_val;
    }

    /// Read an int from a PortMap port.
    int64_t GetPortInt(
        ExportInterface::IDasReadOnlyPortMap* map,
        std::string_view                      port_id,
        int64_t                               default_val = 0)
    {
        int64_t           val = default_val;
        DasReadOnlyString key{std::string{port_id}.c_str()};
        auto              hr = map->GetInt(key.Get(), &val);
        return DAS::IsOk(hr) ? val : default_val;
    }

    /// Write an int value into a PortMap port.
    void SetPortInt(
        ExportInterface::IDasPortMap* map,
        std::string_view              port_id,
        int64_t                       value)
    {
        DasReadOnlyString key{std::string{port_id}.c_str()};
        map->SetInt(key.Get(), value);
    }

    /// Emit a single control-flow signal by listing its port id in the reserved
    /// "signals" JSON string array. The runtime materialises each entry as a
    /// PortValue::Signal() under (node_id, signal_port_id) and gates on it; the
    /// component itself never reads signal values (DAS-60 Stage 3 contract).
    void EmitSignal(
        ExportInterface::IDasPortMap* map,
        std::string_view              signal_port_id)
    {
        std::string json = std::string(R"([")") + std::string(signal_port_id)
                           + R"("])";
        SetPortString(map, "signals", json);
    }

    using DasVariantType = Das::ExportInterface::DasVariantType;
    using Das::ExportInterface::DAS_VARIANT_TYPE_BOOL;
    using Das::ExportInterface::DAS_VARIANT_TYPE_FLOAT;
    using Das::ExportInterface::DAS_VARIANT_TYPE_INT;
    using Das::ExportInterface::DAS_VARIANT_TYPE_NULL;
    using Das::ExportInterface::DAS_VARIANT_TYPE_STRING;

    /// Copy a typed value from @p src_port of @p src into @p dst_port of @p dst,
    /// preserving its runtime variant kind. Returns false when the source port
    /// is absent or null (used by merge to detect which branch produced a value).
    /// Covers the scalar kinds the data plane carries end-to-end; object/image
    /// kinds are not pass-through-able here.
    bool CopyTypedPort(
        ExportInterface::IDasReadOnlyPortMap* src,
        std::string_view                      src_port,
        ExportInterface::IDasPortMap*         dst,
        std::string_view                      dst_port)
    {
        DasReadOnlyString key{std::string{src_port}.c_str()};
        DasVariantType    kind = DAS_VARIANT_TYPE_NULL;
        if (DAS::IsFailed(src->GetType(key.Get(), &kind)))
        {
            return false;
        }
        switch (kind)
        {
        case DAS_VARIANT_TYPE_INT:
        {
            int64_t v{};
            if (DAS::IsOk(src->GetInt(key.Get(), &v)))
            {
                SetPortInt(dst, dst_port, v);
                return true;
            }
            return false;
        }
        case DAS_VARIANT_TYPE_BOOL:
        {
            bool v{};
            if (DAS::IsOk(src->GetBool(key.Get(), &v)))
            {
                DasReadOnlyString dkey{std::string{dst_port}.c_str()};
                dst->SetBool(dkey.Get(), v);
                return true;
            }
            return false;
        }
        case DAS_VARIANT_TYPE_FLOAT:
        {
            double v{};
            if (DAS::IsOk(src->GetFloat(key.Get(), &v)))
            {
                DasReadOnlyString dkey{std::string{dst_port}.c_str()};
                dst->SetFloat(dkey.Get(), v);
                return true;
            }
            return false;
        }
        case DAS_VARIANT_TYPE_STRING:
        {
            IDasReadOnlyString* p_str = nullptr;
            if (DAS::IsOk(src->GetString(key.Get(), &p_str)) && p_str != nullptr)
            {
                DasReadOnlyString dkey{std::string{dst_port}.c_str()};
                dst->SetString(dkey.Get(), p_str);
                p_str->Release();
                return true;
            }
            return false;
        }
        default:
            return false;
        }
    }

    /// Read a raw string from a PortMap string port (no JSON parsing).
    /// Use this for ports written via SetPortString (e.g. "status"), as
    /// opposed to GetPortJson which assumes the port holds JSON text.
    std::string GetPortString(
        ExportInterface::IDasReadOnlyPortMap* map,
        std::string_view                      port_id,
        std::string_view                      default_val = {})
    {
        IDasReadOnlyString* p_str = nullptr;
        DasReadOnlyString   key{std::string{port_id}.c_str()};
        auto                hr = map->GetString(key.Get(), &p_str);
        if (DAS::IsFailed(hr) || p_str == nullptr)
        {
            return std::string{default_val};
        }
        const char* utf8 = nullptr;
        p_str->GetUtf8(&utf8);
        std::string val(utf8 && *utf8 ? utf8 : std::string{default_val});
        p_str->Release();
        return val;
    }

    /// Build a result PortMap from a status string, outputs JSON, and signals
    /// JSON array.
    DasResult BuildResultPortMap(
        std::string_view               status,
        const yyjson::value&           outputs,
        const yyjson::value&           signals,
        ExportInterface::IDasPortMap** pp_out_port_map)
    {
        DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
        DasResult hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }

        SetPortString(output_map.Get(), "status", status);
        SetPortJson(output_map.Get(), "outputs", outputs);
        SetPortJson(output_map.Get(), "signals", signals);

        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();
        return DAS_S_OK;
    }

    std::string StatusFromChild(std::string_view child_status)
    {
        if (child_status == "failed")
        {
            return "failed";
        }
        if (child_status == "cancelled")
        {
            return "cancelled";
        }
        return "completed";
    }

} // namespace

DasFlowControlTaskComponent::DasFlowControlTaskComponent(
    std::string_view                               kind,
    DasPtr<PluginInterface::IDasTaskComponentHost> host)
    : kind_(kind), settings_(Das::Utils::MakeYyjsonObject()),
      host_(std::move(host))
{
}

DasResult DasFlowControlTaskComponent::GetGuid(DasGuid* p_out_guid)
{
    if (p_out_guid == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *p_out_guid = DasIidOf<DasFlowControlTaskComponent>();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::GetRuntimeClassName(
    IDasReadOnlyString** pp_out_name)
{
    if (pp_out_name == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    return CreateIDasReadOnlyStringFromUtf8(
        "Das.FlowControlTaskComponent",
        pp_out_name);
}

DasResult DasFlowControlTaskComponent::ApplySettingsChange(
    ExportInterface::IDasJson*  p_request_json,
    ExportInterface::IDasJson** pp_out_result_json)
{
    if (pp_out_result_json == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    yyjson::value accepted = CloneJson(settings_);
    if (p_request_json != nullptr)
    {
        DasPtr<IDasReadOnlyString> request_string;
        if (DAS::IsOk(p_request_json->ToString(0, request_string.Put()))
            && request_string)
        {
            const char* request_u8 = nullptr;
            request_string->GetUtf8(&request_u8);
            auto parsed = Das::Utils::ParseYyjsonFromString(
                request_u8 != nullptr ? request_u8 : "");
            if (parsed && parsed->is_object())
            {
                auto request_obj = *parsed->as_object();
                auto kind = request_obj[std::string_view("kind")].as_string();
                if (kind && *kind == "setValue")
                {
                    auto payload =
                        request_obj[std::string_view("payload")].as_object();
                    if (payload)
                    {
                        auto key =
                            (*payload)[std::string_view("path")].as_string();
                        if (key && payload->contains(std::string_view("value")))
                        {
                            (*accepted.as_object())[std::string_view(*key)] =
                                CloneJson(
                                    (*payload)[std::string_view("value")]);
                        }
                    }
                }
            }
        }
    }

    settings_ = CloneJson(accepted);
    auto result = Das::Utils::MakeYyjsonObject();
    auto obj = *result.as_object();
    obj[std::string_view("acceptedSettings")] = std::move(accepted);
    auto wrapped = WrapJson(std::move(result));
    if (!wrapped)
    {
        return DAS_E_INVALID_JSON;
    }

    *pp_out_result_json = wrapped.Get();
    (*pp_out_result_json)->AddRef();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::DoRepositoryInvoke(
    PluginInterface::IDasStopToken*       stop_token,
    ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
    ExportInterface::IDasPortMap**        pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    bool stop_requested = false;
    if (stop_token != nullptr
        && DAS::IsOk(stop_token->StopRequested(&stop_requested))
        && stop_requested)
    {
        return BuildResultPortMap(
            "cancelled",
            Das::Utils::MakeYyjsonObject(),
            Das::Utils::MakeYyjsonArray(),
            pp_out_port_map);
    }

    // Read compiledSnapshot from input port map
    auto snapshot_json = GetPortJson(p_input_port_map, "compiledSnapshot");
    if (!snapshot_json || !snapshot_json->is_object())
    {
        // Return a result with diagnostic information
        DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
        DasResult hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }
        SetPortString(output_map.Get(), "status", "failed");
        SetPortString(
            output_map.Get(),
            "diagnostic",
            "Repository invoke requires a compiled child snapshot.");
        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();
        return DAS_S_OK;
    }

    RepositoryInvokeDto::ChildExecutionSnapshotDto snapshot;
    try
    {
        snapshot = yyjson::cast<RepositoryInvokeDto::ChildExecutionSnapshotDto>(
            *snapshot_json);
    }
    catch (const std::exception&)
    {
        DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
        DasResult hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }
        SetPortString(output_map.Get(), "status", "failed");
        SetPortString(
            output_map.Get(),
            "diagnostic",
            "Compiled child snapshot JSON is not valid.");
        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();
        return DAS_S_OK;
    }

    if (snapshot.version != 1)
    {
        DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
        DasResult hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }
        SetPortString(output_map.Get(), "status", "failed");
        SetPortString(
            output_map.Get(),
            "diagnostic",
            "Compiled child snapshot version is not supported.");
        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();
        return DAS_S_OK;
    }

    if (!host_)
    {
        DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
        DasResult hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }
        SetPortString(output_map.Get(), "status", "failed");
        SetPortString(
            output_map.Get(),
            "diagnostic",
            "Task component host is unavailable.");
        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();
        return DAS_S_OK;
    }

    DasGuid    child_component_guid{};
    const auto guid_result =
        DasMakeDasGuid(snapshot.component_guid.c_str(), &child_component_guid);
    if (DAS::IsFailed(guid_result))
    {
        DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
        DasResult hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }
        SetPortString(output_map.Get(), "status", "failed");
        SetPortString(
            output_map.Get(),
            "diagnostic",
            "Compiled child snapshot component GUID is invalid.");
        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();
        return DAS_S_OK;
    }

    DasPtr<PluginInterface::IDasTaskComponent> child_component;
    const auto                                 create_result =
        host_->CreateTaskComponent(child_component_guid, child_component.Put());
    if (DAS::IsFailed(create_result) || !child_component)
    {
        DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
        DasResult hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }
        SetPortString(output_map.Get(), "status", "failed");
        SetPortString(
            output_map.Get(),
            "diagnostic",
            "Child task component could not be created.");
        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();
        return DAS_S_OK;
    }

    // Build child input PortMap from snapshot execution_input
    DAS::DasPtr<ExportInterface::IDasPortMap> child_input;
    DasResult hr = CreateIDasPortMap(child_input.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }

    if (!snapshot.execution_input.is_null())
    {
        auto input_serialized =
            Das::Utils::SerializeYyjsonValue(snapshot.execution_input);
        if (input_serialized)
        {
            DasReadOnlyString val{input_serialized->c_str()};
            SetPortString(
                child_input.Get(),
                "executionInput",
                *input_serialized);
        }
    }

    // Call child Do() with PortMap
    DAS::DasPtr<ExportInterface::IDasPortMap> child_output;
    hr = child_component->Do(stop_token, child_input.Get(), child_output.Put());
    if (DAS::IsFailed(hr) || !child_output)
    {
        DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
        DasResult err_hr = CreateIDasPortMap(output_map.Put());
        if (DAS::IsFailed(err_hr))
        {
            return err_hr;
        }
        SetPortString(output_map.Get(), "status", "failed");
        SetPortString(
            output_map.Get(),
            "diagnostic",
            "Child task component returned a failure result.");
        *pp_out_port_map = output_map.Get();
        output_map.Get()->AddRef();
        return DAS_S_OK;
    }

    // Extract child status from output PortMap. status 是 raw string port
    // （component 用 SetPortString 写），用 GetPortString 直接读取，不能用
    // GetPortJson（会把裸字符串当 JSON 解析而失败，导致 child 失败被误报成
    // completed）。
    std::string child_status =
        GetPortString(child_output.Get(), "status", "completed");

    auto child_outputs_json = GetPortJson(child_output.Get(), "outputs");
    if (!child_outputs_json)
    {
        child_outputs_json = Das::Utils::MakeYyjsonObject();
    }

    std::string status = StatusFromChild(child_status);

    DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
    hr = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }

    SetPortString(output_map.Get(), "status", status);
    SetPortString(output_map.Get(), "childStatus", child_status);
    SetPortJson(output_map.Get(), "childOutputs", *child_outputs_json);

    *pp_out_port_map = output_map.Get();
    output_map.Get()->AddRef();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::DoBranch(
    PluginInterface::IDasStopToken*       stop_token,
    ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
    ExportInterface::IDasPortMap**        pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    bool stop_requested = false;
    if (stop_token != nullptr
        && DAS::IsOk(stop_token->StopRequested(&stop_requested))
        && stop_requested)
    {
        return BuildResultPortMap(
            "cancelled",
            Das::Utils::MakeYyjsonObject(),
            Das::Utils::MakeYyjsonArray(),
            pp_out_port_map);
    }

    // 纯 signal 路由：读 condition → emit true/false。runtime 据 signal_routes
    // gate 分支；组件不感知自己因哪个 signal 被激活（DAS-60 Stage 4）。
    // 内嵌 compiledSnapshot 子图模型已移除，分支体是主图独立节点。
    const bool             condition =
        p_input_port_map ? GetPortBool(p_input_port_map, "condition", false)
                         : false;
    const std::string_view selected = condition ? "true" : "false";

    auto outputs = Das::Utils::MakeYyjsonObject();
    (*outputs.as_object())[std::string_view("selected")] = selected;
    auto signals = Das::Utils::MakeYyjsonArray();
    (*signals.as_array()).emplace_back(selected);

    return BuildResultPortMap(
        "completed",
        std::move(outputs),
        std::move(signals),
        pp_out_port_map);
}

DasResult DasFlowControlTaskComponent::DoFor(
    PluginInterface::IDasStopToken*       stop_token,
    ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
    ExportInterface::IDasPortMap**        pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    bool stop_requested = false;
    if (stop_token != nullptr
        && DAS::IsOk(stop_token->StopRequested(&stop_requested))
        && stop_requested)
    {
        DAS::DasPtr<ExportInterface::IDasPortMap> cancelled_map;
        DasResult                                 hr =
            CreateIDasPortMap(cancelled_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }
        SetPortString(cancelled_map.Get(), "status", "cancelled");
        *pp_out_port_map = cancelled_map.Get();
        cancelled_map.Get()->AddRef();
        return DAS_S_OK;
    }

    // 循环参数走 input port（binding 装配：settings 默认值或上游 data edge）。
    const int64_t start = p_input_port_map
        ? GetPortInt(p_input_port_map, "start", 0)
        : 0;
    const int64_t end = p_input_port_map
        ? GetPortInt(p_input_port_map, "end", 0)
        : 0;
    int64_t       step = p_input_port_map
        ? GetPortInt(p_input_port_map, "step", 1)
        : 1;
    if (step == 0)
    {
        step = 1; // 0 步长无意义且会死循环，兜底为 1。
    }

    // 首次进入初始化计数器；循环状态是组件成员，跨多次 Do() 保持。组件实例
    // 每次 GraphRuntime::Configure 重建，自然按图执行重置（不进 PortFrame）。
    if (!loop_started_)
    {
        loop_index_ = start;
        loop_started_ = true;
    }

    DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
    DasResult                                 hr = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    SetPortString(output_map.Get(), "status", "completed");

    const bool in_range =
        (step > 0) ? (loop_index_ < end) : (loop_index_ > end);
    if (in_range)
    {
        // 写循环变量 index（data 输出端口，循环体经 binding 读取），emit
        // continue 激活循环体；循环体完成后 back-edge 回到 for 重新 Do()。
        SetPortInt(output_map.Get(), "index", loop_index_);
        EmitSignal(output_map.Get(), "continue");
        loop_index_ += step;
    }
    else
    {
        // 范围耗尽，emit break 退出循环（不再激活循环体）。
        EmitSignal(output_map.Get(), "break");
    }

    *pp_out_port_map = output_map.Get();
    output_map.Get()->AddRef();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::DoWhile(
    PluginInterface::IDasStopToken*       stop_token,
    ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
    ExportInterface::IDasPortMap**        pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    bool stop_requested = false;
    if (stop_token != nullptr
        && DAS::IsOk(stop_token->StopRequested(&stop_requested))
        && stop_requested)
    {
        DAS::DasPtr<ExportInterface::IDasPortMap> cancelled_map;
        DasResult                                 hr =
            CreateIDasPortMap(cancelled_map.Put());
        if (DAS::IsFailed(hr))
        {
            return hr;
        }
        SetPortString(cancelled_map.Get(), "status", "cancelled");
        *pp_out_port_map = cancelled_map.Get();
        cancelled_map.Get()->AddRef();
        return DAS_S_OK;
    }

    // 每轮读动态 condition（由循环体/外部每轮回边后重算并写入 PortFrame）。
    const bool condition =
        p_input_port_map ? GetPortBool(p_input_port_map, "condition", false)
                         : false;

    DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
    DasResult                                 hr = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    SetPortString(output_map.Get(), "status", "completed");
    EmitSignal(output_map.Get(), condition ? "continue" : "break");

    *pp_out_port_map = output_map.Get();
    output_map.Get()->AddRef();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::DoMerge(
    PluginInterface::IDasStopToken*       stop_token,
    ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
    ExportInterface::IDasPortMap**        pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    // merge 是纯数据汇合原语，无子调用，不轮询取消。
    (void)stop_token;

    DAS::DasPtr<ExportInterface::IDasPortMap> output_map;
    DasResult                                 hr = CreateIDasPortMap(output_map.Put());
    if (DAS::IsFailed(hr))
    {
        return hr;
    }
    SetPortString(output_map.Get(), "status", "completed");

    // φ-join：merge 由 in_true/in_false signal gate 激活（runtime 保证只有命中
    // 分支到达 merge）。被 gate 掉的分支不产出 → 其 value_* 在输入 PortMap 缺席。
    // 取在场的那路 value 输出到 result；组件自身不读 signal（零感知）。
    if (p_input_port_map == nullptr
        || !CopyTypedPort(
               p_input_port_map, "value_true", output_map.Get(), "result"))
    {
        // value_true 缺席（true 分支未命中）→ 尝试 value_false。
        if (p_input_port_map != nullptr)
        {
            CopyTypedPort(
                p_input_port_map, "value_false", output_map.Get(), "result");
        }
    }

    *pp_out_port_map = output_map.Get();
    output_map.Get()->AddRef();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponent::Do(
    PluginInterface::IDasStopToken*       stop_token,
    ExportInterface::IDasReadOnlyPortMap* p_input_port_map,
    ExportInterface::IDasPortMap**        pp_out_port_map)
{
    if (pp_out_port_map == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    if (kind_ == kRepositoryInvokeKind)
    {
        return DoRepositoryInvoke(
            stop_token,
            p_input_port_map,
            pp_out_port_map);
    }

    if (kind_ == kBranchKind)
    {
        return DoBranch(
            stop_token,
            p_input_port_map,
            pp_out_port_map);
    }

    if (kind_ == kForKind)
    {
        return DoFor(stop_token, p_input_port_map, pp_out_port_map);
    }

    if (kind_ == kWhileKind)
    {
        return DoWhile(stop_token, p_input_port_map, pp_out_port_map);
    }

    if (kind_ == kMergeKind)
    {
        return DoMerge(stop_token, p_input_port_map, pp_out_port_map);
    }

    bool stop_requested = false;
    if (stop_token != nullptr
        && DAS::IsOk(stop_token->StopRequested(&stop_requested))
        && stop_requested)
    {
        return BuildResultPortMap(
            "cancelled",
            Das::Utils::MakeYyjsonObject(),
            Das::Utils::MakeYyjsonArray(),
            pp_out_port_map);
    }

    // delay 当前为桩：仅发 "next" 信号（真实可取消等待在 DAS-52 实现）。
    auto outputs = Das::Utils::MakeYyjsonObject();
    auto signals = Das::Utils::MakeYyjsonArray();
    auto signals_arr = *signals.as_array();
    signals_arr.emplace_back("next");

    return BuildResultPortMap(
        "completed",
        std::move(outputs),
        std::move(signals),
        pp_out_port_map);
}

DasResult DasFlowControlTaskComponentFactory::GetGuid(DasGuid* p_out_guid)
{
    if (p_out_guid == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *p_out_guid = DasIidOf<DasFlowControlTaskComponentFactory>();
    return DAS_S_OK;
}

DasResult DasFlowControlTaskComponentFactory::GetRuntimeClassName(
    IDasReadOnlyString** pp_out_name)
{
    if (pp_out_name == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    return CreateIDasReadOnlyStringFromUtf8(
        "Das.FlowControlTaskComponentFactory",
        pp_out_name);
}

DasResult DasFlowControlTaskComponentFactory::CreateComponent(
    const DasGuid&                       component_guid,
    PluginInterface::IDasTaskComponent** pp_out_component)
{
    if (pp_out_component == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_component = nullptr;

    const auto spec = FindSpec(component_guid);
    if (!spec)
    {
        return DAS_E_NOT_FOUND;
    }

    try
    {
        auto* component = new DasFlowControlTaskComponent(spec->kind, host_);
        component->AddRef();
        *pp_out_component =
            static_cast<PluginInterface::IDasTaskComponent*>(component);
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult DasFlowControlTaskComponentFactory::SetTaskComponentHost(
    PluginInterface::IDasTaskComponentHost* p_host)
{
    host_ = DasPtr<PluginInterface::IDasTaskComponentHost>(p_host);
    return DAS_S_OK;
}

DAS_NS_END
