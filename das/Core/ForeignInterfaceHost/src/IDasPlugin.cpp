#include "TemporaryPluginObjectStorage.h"
#include <das/Core/ForeignInterfaceHost/CppSwigInterop.h>
#include <das/Core/Logger/Logger.h>
#include <das/PluginInterface/IDasPlugin.h>

DasResult DasRegisterPluginObject(DasResult error_code, IDasSwigPlugin* p_swig_plugin)
{
    DasResult result{DAS_S_OK};

    if (DAS::IsFailed(error_code))
    {
        return error_code;
    }

    DAS::DasPtr p_plugin{p_swig_plugin};

    switch (const auto ref_count = p_plugin->AddRef())
    {
    case 1:
        DAS_CORE_LOG_WARN(
            "The reference count inside the plugin object is too small.\n"
            "Maybe the plugin author forget to call AddRef for plugin object.\n"
            "DasCore will try to fix it.");
        break;
    case 2:
        p_plugin->Release();
        break;
    default:
        DAS_CORE_LOG_ERROR(
            "Unexpected reference count inside the plugin object.\n"
            "Expected 3 but {} found.",
            ref_count);
        result = DAS_E_INTERNAL_FATAL_ERROR;
    }

    // See
    // das/Core/ForeignInterfaceHost/src/TemporaryPluginObjectStorage.h:16
    // The mutex has been already locked at
    // PythonRuntime::GetPluginInitializer().
    DAS::Core::ForeignInterfaceHost::g_plugin_object.p_plugin_ = p_plugin;

    return result;
}