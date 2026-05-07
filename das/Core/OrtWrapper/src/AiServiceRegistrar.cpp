#include "AiCpuImpl.h"
#include "AiCudaImpl.h"
#include <das/Core/Logger/Logger.h>
#include <das/Core/OrtWrapper/AiServiceRegistrar.h>
#include <das/Core/OrtWrapper/Config.h>

#include <algorithm>
#include <vector>

DAS_CORE_ORTWRAPPER_NS_BEGIN

static bool IsCudaEpAvailable()
{
    try
    {
        std::vector<std::string> providers = Ort::GetAvailableProviders();
        return std::find(
                   providers.begin(),
                   providers.end(),
                   "CUDAExecutionProvider")
               != providers.end();
    }
    catch (...)
    {
        return false;
    }
}

DasResult RegisterAiServices(IPC::MainProcess::IIpcContext& ipc_context)
{
    // CPU EP is always available
    auto* ai_cpu = new AiCpuImpl{};
    auto  result = ipc_context.RegisterServiceByName(
        ai_cpu,
        DasIidOf<Das::ExportInterface::IDasAI>(),
        "ai.cpu");

    if (Das::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR(
            "Failed to register ai.cpu service: result={}",
            result);
        ai_cpu->Release();
        return result;
    }

    DAS_CORE_LOG_INFO("Registered ai.cpu service");

    // ai.cuda — only if CUDA EP is available (D-11)
    if (IsCudaEpAvailable())
    {
        auto* ai_cuda = new AiCudaImpl{};
        result = ipc_context.RegisterServiceByName(
            ai_cuda,
            DasIidOf<Das::ExportInterface::IDasAI>(),
            "ai.cuda");

        if (Das::IsFailed(result))
        {
            DAS_CORE_LOG_ERROR("Failed to register ai.cuda: result={}", result);
            ai_cuda->Release();
        }
        else
        {
            DAS_CORE_LOG_INFO("Registered AI service: ai.cuda");
        }
    }
    else
    {
        DAS_CORE_LOG_INFO(
            "CUDA EP not available, skipping ai.cuda registration");
    }

    return DAS_S_OK;
}

DAS_CORE_ORTWRAPPER_NS_END
