#include "AiCpuImpl.h"
#include <das/Core/Logger/Logger.h>
#include <das/Core/OrtWrapper/AiServiceRegistrar.h>
#include <das/Core/OrtWrapper/Config.h>

DAS_CORE_ORTWRAPPER_NS_BEGIN

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
    // ai_cpu ownership transfers to the service table, no Release needed
    return DAS_S_OK;
}

DAS_CORE_ORTWRAPPER_NS_END
