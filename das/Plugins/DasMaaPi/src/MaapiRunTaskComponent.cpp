#define DAS_BUILD_SHARED

#include "MaapiRunTaskComponent.h"

#include "PluginUtils.h"

#include <das/DasString.hpp>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Utils/DasJsonCore.h>

#include <new>
#include <string_view>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    namespace
    {
        yyjson::value MakeOwnedJson(yyjson::value value)
        {
            return Das::Utils::CloneYyjsonValue(value);
        }

        yyjson::value MakeRunResult(
            std::string                            status,
            MaapiRunTaskOutputsDto                 outputs = {},
            std::vector<MaapiRunTaskDiagnosticDto> diagnostics = {})
        {
            MaapiRunTaskResultDto result;
            result.status = std::move(status);
            result.outputs = std::move(outputs);
            result.diagnostics = std::move(diagnostics);
            result.signals.succeeded = result.status == "completed";
            result.signals.failed = result.status == "failed";
            result.signals.cancelled = result.status == "cancelled";
            return MakeOwnedJson(yyjson::object(result));
        }

        MaapiRunTaskDiagnosticDto Diagnostic(
            std::string                code,
            std::string                message,
            std::optional<std::string> path = std::nullopt)
        {
            return MaapiRunTaskDiagnosticDto{
                .severity = "error",
                .code = std::move(code),
                .message = std::move(message),
                .path = std::move(path),
                .provider_code = std::nullopt};
        }

        std::optional<yyjson::value> ExtractExecutionEnvelopeInput(
            ExportInterface::IDasJson* p_input_json)
        {
            auto input = ReadJson(p_input_json);
            if (!input)
            {
                return std::nullopt;
            }

            if (auto object = input->as_object();
                object && object->contains(std::string_view("executionInput")))
            {
                return MakeOwnedJson(
                    (*object)[std::string_view("executionInput")]);
            }
            return MakeOwnedJson(*input);
        }

        std::vector<MaapiRunTaskDiagnosticDto> RuntimeDiagnostics(
            const MaaRuntimeResult& result)
        {
            std::vector<MaapiRunTaskDiagnosticDto> diagnostics;
            diagnostics.reserve(result.diagnostics.size());
            for (const auto& diagnostic : result.diagnostics)
            {
                diagnostics.push_back(
                    MaapiRunTaskDiagnosticDto{
                        .severity = diagnostic.severity,
                        .code = diagnostic.code,
                        .message = diagnostic.message,
                        .path = std::nullopt,
                        .provider_code = diagnostic.provider_code});
            }
            return diagnostics;
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
    } // namespace

    DasResult MaapiRunTaskComponent::GetGuid(DasGuid* p_out_guid)
    {
        if (p_out_guid == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_out_guid = DasIidOf<MaapiRunTaskComponent>();
        return DAS_S_OK;
    }

    DasResult MaapiRunTaskComponent::GetRuntimeClassName(
        IDasReadOnlyString** pp_out_name)
    {
        if (pp_out_name == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "Das.MaaPi.RunTaskComponent",
            pp_out_name);
    }

    DasResult MaapiRunTaskComponent::ApplySettingsChange(
        ExportInterface::IDasJson*,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (pp_out_result_json == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_result_json = nullptr;

        auto result = Das::Utils::MakeYyjsonObject();
        (*result.as_object())[std::string_view("acceptedSettings")] =
            Das::Utils::MakeYyjsonObject();
        return ReturnJson(std::move(result), pp_out_result_json);
    }

    DasResult MaapiRunTaskComponent::Do(
        PluginInterface::IDasStopToken* stop_token,
        ExportInterface::IDasJson*,
        ExportInterface::IDasJson*,
        ExportInterface::IDasJson*  p_input_json,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (pp_out_result_json == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_result_json = nullptr;

        auto execution_input = ExtractExecutionEnvelopeInput(p_input_json);
        if (!execution_input)
        {
            return ReturnJson(
                MakeRunResult(
                    "failed",
                    {},
                    {Diagnostic(
                        "invalid-envelope",
                        "MAAPI run requires a compiled execution envelope.",
                        "executionInput")}),
                pp_out_result_json);
        }

        auto parsed = ParseExecutionEnvelope(*execution_input);
        if (DAS::IsFailed(parsed.result))
        {
            return ReturnJson(
                MakeRunResult(
                    "failed",
                    {},
                    {Diagnostic(
                        "invalid-envelope",
                        parsed.message.empty()
                            ? "Compiled execution envelope is invalid."
                            : parsed.message,
                        "executionInput")}),
                pp_out_result_json);
        }

        auto runtime_result = MaaRuntime::Run(
            parsed.envelope,
            MaaApiBoundaryForRuntime(),
            stop_token);

        MaapiRunTaskOutputsDto outputs;
        outputs.completed_tasks = runtime_result.completed_tasks;
        outputs.stopped = runtime_result.stopped;

        std::string status = "completed";
        if (runtime_result.stopped)
        {
            status = "cancelled";
        }
        else if (DAS::IsFailed(runtime_result.das_result))
        {
            status = "failed";
        }

        return ReturnJson(
            MakeRunResult(
                std::move(status),
                std::move(outputs),
                RuntimeDiagnostics(runtime_result)),
            pp_out_result_json);
    }

    DasResult MaapiRunTaskComponentFactory::GetGuid(DasGuid* p_out_guid)
    {
        if (p_out_guid == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_out_guid = DasIidOf<MaapiRunTaskComponentFactory>();
        return DAS_S_OK;
    }

    DasResult MaapiRunTaskComponentFactory::GetRuntimeClassName(
        IDasReadOnlyString** pp_out_name)
    {
        if (pp_out_name == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "Das.MaaPi.RunTaskComponentFactory",
            pp_out_name);
    }

    DasResult MaapiRunTaskComponentFactory::CreateComponent(
        const DasGuid&                       component_guid,
        PluginInterface::IDasTaskComponent** pp_out_component)
    {
        if (pp_out_component == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_component = nullptr;
        if (component_guid != DasIidOf<MaapiRunTaskComponent>())
        {
            return DAS_E_NOT_FOUND;
        }

        try
        {
            auto* component = new MaapiRunTaskComponent();
            component->AddRef();
            *pp_out_component = component;
            return DAS_S_OK;
        }
        catch (const std::bad_alloc&)
        {
            return DAS_E_OUT_OF_MEMORY;
        }
    }

    DasResult MaapiRunTaskComponentFactory::SetTaskComponentHost(
        PluginInterface::IDasTaskComponentHost* p_host)
    {
        host_ = DasPtr<PluginInterface::IDasTaskComponentHost>(p_host);
        return DAS_S_OK;
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END
