#define DAS_BUILD_SHARED

#include "das/IDasBase.h"
#include <das/_autogen/idl/abi/IDasPluginPackage.h>

// Bring PluginInterface types into global namespace for C API
using Das::PluginInterface::IDasPluginPackage;

DAS_C_API DasResult DasCoCreatePlugin(IDasPluginPackage** pp_out_plugin)
{
    DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_plugin)
    try
    {
        const auto p_result = new DAS::AdbCapturePlugin{};
        *pp_out_plugin = p_result;
        p_result->AddRef();
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        DAS_CORE_LOG_ERROR("Out of memory");
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (...)
    {
        return DAS_E_INTERNAL_FATAL_ERROR;
    }
}