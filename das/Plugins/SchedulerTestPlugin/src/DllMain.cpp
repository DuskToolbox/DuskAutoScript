#define DAS_BUILD_SHARED

#include "SchedulerTestPluginImpl.h"

#include <das/DasApi.h>
#include <das/IDasBase.h>
#include <das/Utils/CommonUtils.hpp>

#include <Windows.h>

using Das::FactoryTaskPluginPackage;

// dll 全局：由 exe 通过 DasTestPlugin_SetSharedState 注入。
// FactoryTaskPluginPackage 默认构造时取自此全局。定义置于 DllMain.cpp
// （此 TU 定义 DAS_BUILD_SHARED，使 DAS_EXPORT 解析为 dllexport）。
FactoryTaskSharedState* g_test_shared_state = nullptr;

// dll 导出：注入 exe 持有的 FactoryTaskSharedState 裸指针。
extern "C" DAS_EXPORT void DasTestPlugin_SetSharedState(
    FactoryTaskSharedState* p_shared_state)
{
    g_test_shared_state = p_shared_state;
}

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
    DAS_UTILS_CHECK_POINTER_FOR_PLUGIN(pp_out_plugin)

    try
    {
        auto* p_result = new FactoryTaskPluginPackage{};
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
        DAS_LOG_ERROR(
            "[DasCoCreatePlugin] SchedulerTestPlugin unknown exception");
        return DAS_E_INTERNAL_FATAL_ERROR;
    }
}
