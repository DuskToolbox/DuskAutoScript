#define DAS_BUILD_SHARED

#include "PluginImpl.h"
#include <das/DasApi.h>
#include <das/IDasBase.h>
#include <das/Utils/CommonUtils.hpp>

#include <Windows.h>

using Das::IpcTestPlugin1;
using Das::PluginInterface::IDasTouch;

BOOL APIENTRY
DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    std::ignore = hModule;
    std::ignore = lpReserved;
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

extern "C" DAS_EXPORT DasResult DasCoCreatePlugin(IDasBase** pp_out_plugin)
{
    DAS_LOG_INFO("[DasCoCreatePlugin] Entry - IpcTestPlugin1");
    DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_plugin)

    try
    {
        DAS_LOG_INFO("[DasCoCreatePlugin] Creating IpcTestPlugin1...");
        auto* p_result = new IpcTestPlugin1{};
        DAS_LOG_INFO("[DasCoCreatePlugin] IpcTestPlugin1 created");
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
