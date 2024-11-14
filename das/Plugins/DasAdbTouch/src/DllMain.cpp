
#define DAS_BUILD_SHARED

#include "PluginImpl.h"
#include <new>

DAS_C_API DasResult DasCoCreatePlugin(IDasPlugin** pp_out_plugin)
{
    if (pp_out_plugin == nullptr)
    {
        return DAS_E_INVALID_POINTER;
    }

    try
    {
        const auto p_result = new DAS::DasAdbTouchPlugin{};
        *pp_out_plugin = p_result;
        p_result->AddRef();
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}