#define DAS_BUILD_SHARED

#include "FlowControlTaskComponents.h"

#include <das/Core/TaskScheduler/RepositoryInvokeDtos.h>
#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Utils/DasJsonCore.h>

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

    constexpr std::array<ComponentSpec, 7> kComponents{
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
                0x68f10002,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02}},
            "68F10002-0000-4000-8000-000000000002",
            "das.flow.sequence"},
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
                0x68f10006,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06}},
            "68F10006-0000-4000-8000-000000000006",
            "das.flow.goto"},
        ComponentSpec{
            DasGuid{
                0x68f10007,
                0x0000,
                0x4000,
                {0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07}},
            "68F10007-0000-4000-8000-000000000007",
            "das.flow.invokeRepository"},
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

    yyjson::value MakeOwnedJson(yyjson::value value)
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

    std::optional<yyjson::value> ReadDasJson(ExportInterface::IDasJson* p_json)
    {
        if (p_json == nullptr)
        {
            return std::nullopt;
        }

        DasPtr<IDasReadOnlyString> json_string;
        if (DAS::IsFailed(p_json->ToString(0, json_string.Put()))
            || !json_string)
        {
            return std::nullopt;
        }

        const char* json_u8 = nullptr;
        if (DAS::IsFailed(json_string->GetUtf8(&json_u8)) || json_u8 == nullptr)
        {
            return std::nullopt;
        }
        return Das::Utils::ParseYyjsonFromString(json_u8);
    }

    yyjson::value MakeTaskComponentResult(
        std::string_view status,
        yyjson::value    outputs,
        yyjson::value    signals)
    {
        auto result = Das::Utils::MakeYyjsonObject();
        auto obj = *result.as_object();
        obj[std::string_view("status")] = status;
        obj[std::string_view("outputs")] = std::move(outputs);
        obj[std::string_view("diagnostics")] = Das::Utils::MakeYyjsonArray();
        obj[std::string_view("signals")] = std::move(signals);
        return result;
    }

    yyjson::value MakeRepositoryInvokeResult(
        std::string_view           status,
        std::optional<std::string> child_status,
        yyjson::value              child_outputs,
        std::vector<RepositoryInvokeDto::InvokeRepositoryTaskDiagnosticDto>
            diagnostics = {})
    {
        RepositoryInvokeDto::InvokeRepositoryTaskResultDto result;
        result.status = std::string{status};
        result.outputs.child_status = std::move(child_status);
        result.outputs.child_outputs = std::move(child_outputs);
        result.diagnostics = std::move(diagnostics);
        result.signals.succeeded = status == "completed";
        result.signals.failed = status == "failed";
        result.signals.cancelled = status == "cancelled";
        return MakeOwnedJson(yyjson::object(result));
    }

    yyjson::value MakeRepositoryInvokeDiagnosticResult(
        std::string_view code,
        std::string_view message)
    {
        std::vector<RepositoryInvokeDto::InvokeRepositoryTaskDiagnosticDto>
            diagnostics;
        diagnostics.push_back(
            RepositoryInvokeDto::InvokeRepositoryTaskDiagnosticDto{
                .severity = "error",
                .code = std::string{code},
                .message = std::string{message},
                .path = std::nullopt});
        return MakeRepositoryInvokeResult(
            "failed",
            std::nullopt,
            Das::Utils::MakeYyjsonObject(),
            std::move(diagnostics));
    }

    DasResult ReturnJson(
        yyjson::value                     value,
        ExportInterface::IDasJson** const pp_out_result_json)
    {
        auto wrapped = WrapJson(std::move(value));
        if (!wrapped)
        {
            return DAS_E_INVALID_JSON;
        }

        *pp_out_result_json = wrapped.Get();
        (*pp_out_result_json)->AddRef();
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

    bool ReadBranchCondition(ExportInterface::IDasJson* p_input_json)
    {
        if (p_input_json == nullptr)
        {
            return false;
        }

        DasPtr<IDasReadOnlyString> input_string;
        if (DAS::IsFailed(p_input_json->ToString(0, input_string.Put()))
            || !input_string)
        {
            return false;
        }

        const char* input_u8 = nullptr;
        if (DAS::IsFailed(input_string->GetUtf8(&input_u8))
            || input_u8 == nullptr)
        {
            return false;
        }

        auto input = Das::Utils::ParseYyjsonFromString(input_u8);
        if (!input || !input->is_object())
        {
            return false;
        }

        auto obj = *input->as_object();
        auto value = obj[std::string_view("condition")].as_bool();
        return value.value_or(false);
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
    PluginInterface::IDasStopToken* stop_token,
    ExportInterface::IDasJson*      p_environment_json,
    ExportInterface::IDasJson*      p_settings_json,
    ExportInterface::IDasJson*      p_input_json,
    ExportInterface::IDasJson**     pp_out_result_json)
{
    bool stop_requested = false;
    if (stop_token != nullptr
        && DAS::IsOk(stop_token->StopRequested(&stop_requested))
        && stop_requested)
    {
        return ReturnJson(
            MakeRepositoryInvokeResult(
                "cancelled",
                std::nullopt,
                Das::Utils::MakeYyjsonObject()),
            pp_out_result_json);
    }

    const auto input = ReadDasJson(p_input_json);
    if (!input || !input->is_object())
    {
        return ReturnJson(
            MakeRepositoryInvokeDiagnosticResult(
                "missing-compiled-snapshot",
                "Repository invoke requires a compiled child snapshot."),
            pp_out_result_json);
    }

    const auto input_obj = input->as_object();
    if (!input_obj->contains(std::string_view("compiledSnapshot")))
    {
        return ReturnJson(
            MakeRepositoryInvokeDiagnosticResult(
                "missing-compiled-snapshot",
                "Repository invoke requires a compiled child snapshot."),
            pp_out_result_json);
    }

    const auto snapshot_json =
        (*input_obj)[std::string_view("compiledSnapshot")];
    const auto snapshot_obj = snapshot_json.as_object();
    if (!snapshot_obj)
    {
        return ReturnJson(
            MakeRepositoryInvokeDiagnosticResult(
                "missing-compiled-snapshot",
                "Repository invoke requires a compiled child snapshot."),
            pp_out_result_json);
    }

    RepositoryInvokeDto::ChildExecutionSnapshotDto snapshot;
    try
    {
        snapshot = yyjson::cast<RepositoryInvokeDto::ChildExecutionSnapshotDto>(
            snapshot_json);
    }
    catch (const std::exception&)
    {
        return ReturnJson(
            MakeRepositoryInvokeDiagnosticResult(
                "malformed-compiled-snapshot",
                "Compiled child snapshot JSON is not valid."),
            pp_out_result_json);
    }

    if (snapshot.version != 1)
    {
        return ReturnJson(
            MakeRepositoryInvokeDiagnosticResult(
                "invalid-snapshot-version",
                "Compiled child snapshot version is not supported."),
            pp_out_result_json);
    }

    if (!host_)
    {
        return ReturnJson(
            MakeRepositoryInvokeDiagnosticResult(
                "task-component-host-unavailable",
                "Task component host is unavailable."),
            pp_out_result_json);
    }

    DasGuid    child_component_guid{};
    const auto guid_result =
        DasMakeDasGuid(snapshot.component_guid.c_str(), &child_component_guid);
    if (DAS::IsFailed(guid_result))
    {
        return ReturnJson(
            MakeRepositoryInvokeDiagnosticResult(
                "invalid-child-component-guid",
                "Compiled child snapshot component GUID is invalid."),
            pp_out_result_json);
    }

    DasPtr<PluginInterface::IDasTaskComponent> child_component;
    const auto                                 create_result =
        host_->CreateTaskComponent(child_component_guid, child_component.Put());
    if (DAS::IsFailed(create_result) || !child_component)
    {
        return ReturnJson(
            MakeRepositoryInvokeDiagnosticResult(
                "child-component-unavailable",
                "Child task component could not be created."),
            pp_out_result_json);
    }

    auto child_input = WrapJson(CloneJson(snapshot.execution_input));
    if (!child_input)
    {
        return ReturnJson(
            MakeRepositoryInvokeDiagnosticResult(
                "malformed-compiled-snapshot",
                "Compiled child execution input could not be wrapped."),
            pp_out_result_json);
    }

    DasPtr<ExportInterface::IDasJson> child_result;
    const auto                        do_result = child_component->Do(
        stop_token,
        p_environment_json,
        p_settings_json,
        child_input.Get(),
        child_result.Put());
    if (DAS::IsFailed(do_result) || !child_result)
    {
        return ReturnJson(
            MakeRepositoryInvokeDiagnosticResult(
                "child-do-failed",
                "Child task component returned a failure result."),
            pp_out_result_json);
    }

    const auto child_result_json = ReadDasJson(child_result.Get());
    if (!child_result_json || !child_result_json->is_object())
    {
        return ReturnJson(
            MakeRepositoryInvokeDiagnosticResult(
                "malformed-child-result",
                "Child task component returned malformed result JSON."),
            pp_out_result_json);
    }

    const auto child_result_obj = child_result_json->as_object();
    const auto child_status =
        (*child_result_obj)[std::string_view("status")].as_string();
    if (!child_status)
    {
        return ReturnJson(
            MakeRepositoryInvokeDiagnosticResult(
                "malformed-child-result",
                "Child task component result is missing status."),
            pp_out_result_json);
    }

    yyjson::value child_outputs = Das::Utils::MakeYyjsonObject();
    if (child_result_obj->contains(std::string_view("outputs")))
    {
        child_outputs =
            CloneJson((*child_result_obj)[std::string_view("outputs")]);
    }

    return ReturnJson(
        MakeRepositoryInvokeResult(
            StatusFromChild(*child_status),
            std::string{*child_status},
            std::move(child_outputs)),
        pp_out_result_json);
}

DasResult DasFlowControlTaskComponent::Do(
    PluginInterface::IDasStopToken* stop_token,
    ExportInterface::IDasJson*      p_environment_json,
    ExportInterface::IDasJson*      p_settings_json,
    ExportInterface::IDasJson*      p_input_json,
    ExportInterface::IDasJson**     pp_out_result_json)
{
    if (pp_out_result_json == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_result_json = nullptr;

    if (kind_ == kRepositoryInvokeKind)
    {
        return DoRepositoryInvoke(
            stop_token,
            p_environment_json,
            p_settings_json,
            p_input_json,
            pp_out_result_json);
    }

    bool stop_requested = false;
    if (stop_token != nullptr
        && DAS::IsOk(stop_token->StopRequested(&stop_requested))
        && stop_requested)
    {
        auto wrapped = WrapJson(MakeTaskComponentResult(
            "cancelled",
            Das::Utils::MakeYyjsonObject(),
            Das::Utils::MakeYyjsonArray()));
        if (!wrapped)
        {
            return DAS_E_INVALID_JSON;
        }

        *pp_out_result_json = wrapped.Get();
        (*pp_out_result_json)->AddRef();
        return DAS_S_OK;
    }

    auto outputs = Das::Utils::MakeYyjsonObject();
    auto signals = Das::Utils::MakeYyjsonArray();
    auto signals_arr = *signals.as_array();
    if (kind_ == "das.flow.branch")
    {
        const bool condition = ReadBranchCondition(p_input_json);
        (*outputs.as_object())[std::string_view("selected")] =
            condition ? "true" : "false";
        signals_arr.emplace_back(condition ? "true" : "false");
    }
    else
    {
        signals_arr.emplace_back("next");
    }

    auto wrapped = WrapJson(MakeTaskComponentResult(
        "completed",
        std::move(outputs),
        std::move(signals)));
    if (!wrapped)
    {
        return DAS_E_INVALID_JSON;
    }

    *pp_out_result_json = wrapped.Get();
    (*pp_out_result_json)->AddRef();
    return DAS_S_OK;
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
