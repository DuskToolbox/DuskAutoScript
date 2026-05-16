#define DAS_BUILD_SHARED

#include "MaapiRunTaskComponent.h"

#include "PluginUtils.h"

#include <das/DasString.hpp>
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
            const auto serialized = value.write(yyjson::WriteFlag::NoFlag);
            auto parsed = Das::Utils::ParseYyjsonFromString(
                std::string_view(serialized.data(), serialized.size()));
            return parsed ? std::move(*parsed) : Das::Utils::MakeYyjsonObject();
        }

        yyjson::value MakeRunResult(
            std::string status,
            std::vector<MaapiRunTaskDiagnosticDto> diagnostics = {})
        {
            MaapiRunTaskResultDto result;
            result.status = std::move(status);
            result.diagnostics = std::move(diagnostics);
            result.signals.succeeded = result.status == "completed";
            result.signals.failed = result.status == "failed";
            result.signals.cancelled = result.status == "cancelled";
            return MakeOwnedJson(yyjson::object(result));
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
        PluginInterface::IDasStopToken*,
        ExportInterface::IDasJson*,
        ExportInterface::IDasJson*,
        ExportInterface::IDasJson*,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (pp_out_result_json == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_result_json = nullptr;

        return ReturnJson(
            MakeRunResult(
                "failed",
                {MaapiRunTaskDiagnosticDto{
                    .severity = "error",
                    .code = "not-implemented",
                    .message = "MAAPI run execution is not implemented.",
                    .path = std::nullopt}}),
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
} // namespace Plugins::DasMaaPi
DAS_NS_END
