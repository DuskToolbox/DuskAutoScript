#define DAS_BUILD_SHARED

#include "MaapiAgentTaskComponent.h"

#include "MaapiAgentRuntimeAdapter.h"

#include <das/DasApi.h>
#include <das/DasString.hpp>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>
#include <das/Utils/DasJsonCore.h>

#include <string_view>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    namespace
    {
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

        yyjson::value CloneJson(const yyjson::value& value)
        {
            return Das::Utils::CloneYyjsonValue(value);
        }

        bool StopRequested(PluginInterface::IDasStopToken* stop_token)
        {
            bool requested = false;
            return stop_token
                   && DAS::IsOk(stop_token->StopRequested(&requested))
                   && requested;
        }
    } // namespace

    MaapiAgentTaskComponent::MaapiAgentTaskComponent()
        : owned_runner_(
              std::make_unique<AgentRuntime::BoostAgentProcessRunner>()),
          owned_service_(
              std::make_unique<AgentRuntime::AgentRuntimeService>(
                  MaaApiBoundaryForRuntime(),
                  AgentProcessRunnerForRuntime()
                      ? *AgentProcessRunnerForRuntime()
                      : static_cast<AgentRuntime::IAgentProcessRunner&>(
                            *owned_runner_))),
          service_(owned_service_.get()),
          settings_(Das::Utils::MakeYyjsonObject())
    {
    }

    MaapiAgentTaskComponent::MaapiAgentTaskComponent(
        AgentRuntime::AgentRuntimeService&   service,
        AgentRuntime::AgentRuntimeMaaContext context)
        : service_(&service), context_(context),
          settings_(Das::Utils::MakeYyjsonObject())
    {
    }

    MaapiAgentTaskComponent::~MaapiAgentTaskComponent() = default;

    DasResult MaapiAgentTaskComponent::GetGuid(DasGuid* p_out_guid)
    {
        if (p_out_guid == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_out_guid = DasIidOf<MaapiAgentTaskComponent>();
        return DAS_S_OK;
    }

    DasResult MaapiAgentTaskComponent::GetRuntimeClassName(
        IDasReadOnlyString** pp_out_name)
    {
        if (pp_out_name == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "Das.MaaPi.AgentTaskComponent",
            pp_out_name);
    }

    DasResult MaapiAgentTaskComponent::ApplySettingsChange(
        ExportInterface::IDasJson*  p_request_json,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (pp_out_result_json == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_result_json = nullptr;

        yyjson::value accepted = CloneJson(settings_);
        if (p_request_json != nullptr)
        {
            const auto request_text = JsonFromDasJson(p_request_json);
            auto       parsed = Das::Utils::ParseYyjsonFromString(request_text);
            if (!parsed || !parsed->is_object())
            {
                return DAS_E_INVALID_JSON;
            }
            accepted = std::move(*parsed);
        }

        settings_ = CloneJson(accepted);
        auto result = Das::Utils::MakeYyjsonObject();
        (*result.as_object())[std::string_view("acceptedSettings")] =
            std::move(accepted);
        auto wrapped = WrapJson(std::move(result));
        if (!wrapped)
        {
            return DAS_E_INVALID_JSON;
        }

        *pp_out_result_json = wrapped.Get();
        (*pp_out_result_json)->AddRef();
        return DAS_S_OK;
    }

    DasResult MaapiAgentTaskComponent::Do(
        PluginInterface::IDasStopToken* stop_token,
        ExportInterface::IDasJson*,
        ExportInterface::IDasJson*  p_settings_json,
        ExportInterface::IDasJson*  p_input_json,
        ExportInterface::IDasJson** pp_out_result_json)
    {
        if (pp_out_result_json == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_result_json = nullptr;

        if (StopRequested(stop_token))
        {
            auto cancelled = WrapAgentRuntimeJson(MakeAgentAdapterCancelled());
            if (!cancelled)
            {
                return DAS_E_INVALID_JSON;
            }
            *pp_out_result_json = cancelled.Get();
            (*pp_out_result_json)->AddRef();
            return DAS_S_OK;
        }

        const auto settings_json =
            p_settings_json != nullptr
                ? JsonFromDasJson(p_settings_json)
                : JsonFromDasJson(WrapJson(CloneJson(settings_)).Get());
        const auto input_json = JsonFromDasJson(p_input_json);
        const auto parsed = AgentRuntime::MergeAgentRuntimeSettingsAndInput(
            settings_json,
            input_json);

        auto wrapped = WrapAgentRuntimeJson(
            ExecuteAgentRuntimeRequest(*service_, context_, parsed));
        if (!wrapped)
        {
            return DAS_E_INVALID_JSON;
        }

        *pp_out_result_json = wrapped.Get();
        (*pp_out_result_json)->AddRef();
        return DAS_S_OK;
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END
