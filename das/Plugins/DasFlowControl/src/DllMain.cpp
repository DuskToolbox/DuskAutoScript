#define DAS_BUILD_SHARED

#include "PluginImpl.h"

#include <das/DasApi.h>
#include <das/IDasBase.h>

#include <Windows.h>

BOOL APIENTRY DllMain(HMODULE h_module, DWORD reason, LPVOID reserved)
{
    (void)h_module;
    (void)reason;
    (void)reserved;
    return TRUE;
}

extern "C" DAS_EXPORT DasResult DasCoCreatePlugin(IDasBase** pp_out_plugin)
{
    if (pp_out_plugin == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }
    *pp_out_plugin = nullptr;

    try
    {
        auto* plugin = new Das::DasFlowControlPlugin{};
        plugin->AddRef();
        *pp_out_plugin =
            static_cast<Das::PluginInterface::IDasPluginPackage*>(plugin);
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}
