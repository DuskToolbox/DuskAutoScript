#define DAS_BUILD_SHARED

#include "MaapiAgentComponent.h"

#include "MaapiAgentRuntimeAdapter.h"

#include <Das.ExportInterface.IDasVariantVector.hpp>
#include <das/DasString.hpp>
#include <das/Plugins/DasMaaPi/MaapiDto.h>
#include <das/Plugins/DasMaaPi/MaaRuntime.h>

#include <string>

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    namespace
    {
        DasResult WriteStringResult(
            std::string_view                       json,
            ExportInterface::IDasVariantVector**   pp_out_result)
        {
            if (pp_out_result == nullptr)
            {
                return DAS_E_INVALID_POINTER;
            }
            *pp_out_result = nullptr;

            DasPtr<ExportInterface::IDasVariantVector> result;
            auto hr = CreateIDasVariantVector(result.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }

            DasPtr<IDasReadOnlyString> text;
            hr = CreateIDasReadOnlyStringFromUtf8WithLength(
                json.data(),
                json.size(),
                text.Put());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }
            hr = result->PushBackString(text.Get());
            if (DAS::IsFailed(hr))
            {
                return hr;
            }

            *pp_out_result = result.Get();
            (*pp_out_result)->AddRef();
            return DAS_S_OK;
        }

        DasResult WriteResult(
            const AgentRuntime::AgentRuntimeResultDto& result,
            ExportInterface::IDasVariantVector**       pp_out_result)
        {
            return WriteStringResult(
                AgentRuntime::SerializeAgentRuntimeResultJson(result),
                pp_out_result);
        }
    } // namespace

    MaapiAgentComponent::MaapiAgentComponent()
        : owned_runner_(
              std::make_unique<AgentRuntime::BoostAgentProcessRunner>()),
          owned_service_(std::make_unique<AgentRuntime::AgentRuntimeService>(
              MaaApiBoundaryForRuntime(),
              AgentProcessRunnerForRuntime()
                  ? *AgentProcessRunnerForRuntime()
                  : static_cast<AgentRuntime::IAgentProcessRunner&>(
                        *owned_runner_))),
          service_(owned_service_.get())
    {
    }

    MaapiAgentComponent::MaapiAgentComponent(
        AgentRuntime::AgentRuntimeService&   service,
        AgentRuntime::AgentRuntimeMaaContext context)
        : service_(&service), context_(context)
    {
    }

    MaapiAgentComponent::~MaapiAgentComponent() = default;

    DasResult MaapiAgentComponent::GetGuid(DasGuid* p_out_guid)
    {
        if (p_out_guid == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *p_out_guid = DasIidOf<MaapiAgentComponent>();
        return DAS_S_OK;
    }

    DasResult MaapiAgentComponent::GetRuntimeClassName(
        IDasReadOnlyString** pp_out_name)
    {
        if (pp_out_name == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        return CreateIDasReadOnlyStringFromUtf8(
            "Das.MaaPi.AgentComponent",
            pp_out_name);
    }

    DasResult MaapiAgentComponent::Dispatch(
        IDasReadOnlyString*                  p_function_name,
        ExportInterface::IDasVariantVector*  p_arguments,
        ExportInterface::IDasVariantVector** pp_out_result)
    {
        if (p_function_name == nullptr || pp_out_result == nullptr)
        {
            return DAS_E_INVALID_POINTER;
        }
        *pp_out_result = nullptr;

        const char* function_name = nullptr;
        if (DAS::IsFailed(p_function_name->GetUtf8(&function_name))
            || function_name == nullptr)
        {
            return WriteResult(
                MakeAgentAdapterFailure(
                    {MakeAgentAdapterDiagnostic(
                        "invalid-function-name",
                        "Dispatch function name must be a UTF-8 string",
                        "functionName")}),
                pp_out_result);
        }

        const auto arg_count =
            p_arguments == nullptr ? 0 : p_arguments->GetSize();
        if (arg_count != 1)
        {
            return WriteResult(
                MakeAgentAdapterFailure(
                    {MakeAgentAdapterDiagnostic(
                        "invalid-argument-count",
                        "Dispatch requires exactly one UTF-8 JSON string "
                        "argument",
                        "arguments")}),
                pp_out_result);
        }

        DasPtr<IDasReadOnlyString> request_text;
        if (DAS::IsFailed(p_arguments->GetString(0, request_text.Put()))
            || !request_text)
        {
            return WriteResult(
                MakeAgentAdapterFailure(
                    {MakeAgentAdapterDiagnostic(
                        "invalid-argument-type",
                        "Dispatch argument 0 must be a UTF-8 JSON string",
                        "arguments.0")}),
                pp_out_result);
        }

        const char* request_json = nullptr;
        if (DAS::IsFailed(request_text->GetUtf8(&request_json))
            || request_json == nullptr)
        {
            return WriteResult(
                MakeAgentAdapterFailure(
                    {MakeAgentAdapterDiagnostic(
                        "invalid-argument-string",
                        "Dispatch argument 0 could not be read as UTF-8",
                        "arguments.0")}),
                pp_out_result);
        }

        const auto parsed = AgentRuntime::NormalizeAgentRuntimeDispatch(
            function_name,
            request_json);
        return WriteResult(
            ExecuteAgentRuntimeRequest(*service_, context_, parsed),
            pp_out_result);
    }
} // namespace Plugins::DasMaaPi
DAS_NS_END
