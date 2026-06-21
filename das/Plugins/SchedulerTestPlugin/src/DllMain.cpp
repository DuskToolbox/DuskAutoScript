#define DAS_BUILD_SHARED

#include "SchedulerTestPluginImpl.h"

#include <das/DasConfig.h>
#include <das/DasApi.h>
#include <das/IDasBase.h>
#include <das/Utils/CommonUtils.hpp>

#include <Windows.h>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_BOOST_INTERPROCESS_WARNING
#include <boost/interprocess/managed_shared_memory.hpp>
DAS_DISABLE_WARNING_END

#include <cstdlib>
#include <memory>
#include <string>

using Das::FactoryTaskPluginPackage;

// dll 全局：由 exe 通过 DasTestPlugin_SetSharedMemoryName 设置。
// g_shm 持有打开的共享内存段（生命周期随 dll），g_test_shared_state 指向段内的
// FactoryTaskSharedState 实例。FactoryTaskPluginPackage 默认构造时
// 从 g_test_shared_state 取值。定义置于 DllMain.cpp（此 TU 定义
// DAS_BUILD_SHARED， 使 DAS_EXPORT 解析为 dllexport）。 g_test_shared_state
// 必须为 external linkage：SchedulerTestPluginImpl.cpp 通过 extern
// 声明引用同一全局。g_shm 仅本 TU 使用，保持 static。
static std::unique_ptr<boost::interprocess::managed_shared_memory> g_shm;
FactoryTaskSharedState* g_test_shared_state = nullptr;

// dll 导出：exe 在创建共享内存段后调用本函数，把段名传给 dll。
// dll 用 open_only 打开同名段并 find<FactoryTaskSharedState>("state")
// 取回指针。 重复调用会先释放前一段再打开新段。
extern "C" DAS_EXPORT void DasTestPlugin_SetSharedMemoryName(const char* name)
{
    if (!name || name[0] == '\0')
    {
        return;
    }
    try
    {
        auto new_shm =
            std::make_unique<boost::interprocess::managed_shared_memory>(
                boost::interprocess::open_only_t{},
                name);
        auto result = new_shm->find<FactoryTaskSharedState>("state");
        if (result.first != nullptr)
        {
            g_shm = std::move(new_shm);
            g_test_shared_state = result.first;
        }
    }
    catch (...)
    {
        // 共享内存尚未就绪或名称错误：保持原状，调用方稍后可重试或
        // 通过 DAS_SCHEDULER_TEST_SHM_NAME 环境变量由 dll 自行解析。
    }
}

// 从环境变量回退解析共享内存名。DasHost.exe 子进程通过此机制获取段名：
// 父进程在 spawn 之前设置 DAS_SCHEDULER_TEST_SHM_NAME，子进程内的
// SchedulerTestPlugin 在 DasCoCreatePlugin 时若 g_test_shared_state
// 未被显式设置，则尝试通过环境变量打开段。
static void TryResolveSharedStateFromEnv()
{
    if (g_test_shared_state != nullptr)
    {
        return;
    }
    DAS_DISABLE_WARNING_BEGIN
    DAS_PRAGMA(warning(disable : 4996))
    const char* name = std::getenv("DAS_SCHEDULER_TEST_SHM_NAME");
    DAS_DISABLE_WARNING_END
    if (name != nullptr && name[0] != '\0')
    {
        DasTestPlugin_SetSharedMemoryName(name);
    }
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

    // 子进程场景下 DasCoCreatePlugin 是首个被 PluginManager 调用的入口：
    // 在此尝试从环境变量解析共享内存段，保证 FactoryTaskPluginPackage 构造时
    // state_ 已可用。
    TryResolveSharedStateFromEnv();

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
