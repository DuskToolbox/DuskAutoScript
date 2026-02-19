#define DAS_BUILD_SHARED

#include "das/IDasBase.h"

#include "PluginImpl.h"
#include <das/DasApi.h>
#include <das/Utils/CommonUtils.hpp>

// Bring PluginInterface types into global namespace for C API
using Das::PluginInterface::IDasPluginPackage;

DAS_C_API DasResult DasCoCreatePlugin(IDasBase** pp_out_plugin)
{
    DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_plugin)
    try
    {
        const auto p_result = new DAS::WindowsCapturePlugin{};
        *pp_out_plugin = p_result;
        p_result->AddRef();
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        DAS_LOG_ERROR("Out of memory");
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (...)
    {
        return DAS_E_INTERNAL_FATAL_ERROR;
    }
}
