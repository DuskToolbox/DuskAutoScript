#define DAS_BUILD_SHARED

#include "PluginImpl.h"
#include <das/DasApi.h>
#include <das/IDasBase.h>
#include <das/Utils/CommonUtils.hpp>

using Das::PluginInterface::IDasPluginPackage;

DAS_C_API DasResult DasCoCreatePlugin(IDasBase** pp_out_plugin)
{
    DAS_LOG_INFO("[DasCoCreatePlugin] IpcTestPlugin2 Entry");
    DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_plugin)
    try
    {
        DAS_LOG_INFO("[DasCoCreatePlugin] Creating IpcTestPlugin2...");
        const auto p_result = new DAS::IpcTestPlugin2{};
        DAS_LOG_INFO("[DasCoCreatePlugin] IpcTestPlugin2 created");
        *pp_out_plugin = p_result;
        DAS_LOG_INFO("[DasCoCreatePlugin] Calling AddRef...");
        p_result->AddRef();
        DAS_LOG_INFO("[DasCoCreatePlugin] AddRef done, returning DAS_S_OK");
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        DAS_LOG_ERROR("Out of memory");
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (...)
    {
        DAS_LOG_ERROR("[DasCoCreatePlugin] Unknown exception");
        return DAS_E_INTERNAL_FATAL_ERROR;
    }
}
