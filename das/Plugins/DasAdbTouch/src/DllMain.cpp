
#define DAS_BUILD_SHARED

#include "das/IDasBase.h"
#include <das/_autogen/idl/abi/IDasPluginPackage.h>

// 任务5扩展：添加必要的头文件以定义使用的宏
#include <das/DasApi.h>
#include <das/Utils/CommonUtils.hpp> // 提供 DAS_UTILS_CHECK_POINTER_FOR_PLUGIN

// Bring PluginInterface types into global namespace for C API
using Das::PluginInterface::IDasPluginPackage;

#include "PluginImpl.h"
#include <new>

DAS_C_API DasResult DasCoCreatePlugin(IDasPluginPackage** pp_out_plugin)
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