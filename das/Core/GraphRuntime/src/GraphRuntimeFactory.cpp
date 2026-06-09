#include <das/Core/GraphRuntime/GraphRuntimeFactory.h>

#include <das/Core/Logger/Logger.h>
#include <das/DasApi.h>
#include <new>

DAS_CORE_GRAPHRUNTIME_NS_BEGIN

// --- GraphRuntimeImpl implementation ---

GraphRuntimeImpl::GraphRuntimeImpl(
    Das::PluginInterface::IDasTaskComponentHost* p_host)
    : host_(p_host)
{
}

DasResult GraphRuntimeImpl::GetErrorMessage(
    IDasReadOnlyString** pp_out_error_message)
{
    if (!pp_out_error_message)
    {
        return DAS_E_INVALID_POINTER;
    }

    return CreateIDasReadOnlyStringFromUtf8(
        last_error_.c_str(),
        pp_out_error_message);
}

DasResult GraphRuntimeImpl::Execute(
    IDasReadOnlyString*                  p_compiled_artifact_json,
    Das::PluginInterface::IDasStopToken* p_stop_token,
    Das::ExportInterface::IDasJson**     pp_out_result_json)
{
    if (!pp_out_result_json)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_result_json = nullptr;

    // Parse compiled graph plan from JSON string.
    Dto::CompiledGraphPlanDto plan;
    if (p_compiled_artifact_json)
    {
        const char* utf8 = nullptr;
        DasResult   hr = p_compiled_artifact_json->GetUtf8(&utf8);
        if (DAS::IsFailed(hr) || !utf8)
        {
            last_error_ = "Execute: failed to read compiled artifact JSON";
            DAS_CORE_LOG_ERROR("{}", last_error_);
            return DAS_E_INVALID_POINTER;
        }

        std::string json_str(utf8);
        if (!json_str.empty())
        {
            auto doc = yyjson::read(json_str);
            plan = yyjson::cast<Dto::CompiledGraphPlanDto>(doc);
        }
    }

    // Execute via engine.
    DasResult result = engine_.RunWithHost(
        plan,
        plan.compiled_fingerprint,
        p_stop_token,
        host_.Get());

    if (DAS::IsFailed(result))
    {
        last_error_ = engine_.GetLastErrorMessage();
        if (last_error_.empty())
        {
            last_error_ = "Graph execution failed";
        }
        DAS_CORE_LOG_ERROR("{}", last_error_);
        return result;
    }

    // Return an empty result JSON on success.
    return CreateEmptyDasJson(pp_out_result_json);
}

DAS_CORE_GRAPHRUNTIME_NS_END

// ============================================================================
// C ABI factory function
// ============================================================================

DAS_C_API DasResult
CreateGraphRuntime(Das::ExportInterface::IDasGraphRuntime** pp_out_runtime)
{
    if (!pp_out_runtime)
    {
        return DAS_E_INVALID_POINTER;
    }

    try
    {
        auto* impl = new Das::Core::GraphRuntime::GraphRuntimeImpl{nullptr};
        impl->AddRef();
        *pp_out_runtime = impl;
        DAS_CORE_LOG_INFO("CreateGraphRuntime: runtime created successfully");
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        DAS_CORE_LOG_ERROR("CreateGraphRuntime: out of memory");
        return DAS_E_OUT_OF_MEMORY;
    }
}

DAS_C_API DasResult CreateGraphRuntimeWithHost(
    Das::PluginInterface::IDasTaskComponentHost* p_host,
    Das::ExportInterface::IDasGraphRuntime**     pp_out_runtime)
{
    if (!pp_out_runtime)
    {
        return DAS_E_INVALID_POINTER;
    }
    if (!p_host)
    {
        return DAS_E_INVALID_POINTER;
    }

    try
    {
        auto* impl = new Das::Core::GraphRuntime::GraphRuntimeImpl{p_host};
        impl->AddRef();
        *pp_out_runtime = impl;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}
