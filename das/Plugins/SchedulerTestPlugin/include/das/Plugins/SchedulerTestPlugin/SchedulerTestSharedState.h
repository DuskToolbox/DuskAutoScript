#ifndef DAS_PLUGINS_SCHEDULERTESTPLUGIN_SCHEDULERTESTSHAREDSTATE_H
#define DAS_PLUGINS_SCHEDULERTESTPLUGIN_SCHEDULERTESTSHAREDSTATE_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <das/DasConfig.h>
#include <das/DasExport.h>
#include <das/IDasBase.h>

DAS_DISABLE_WARNING_BEGIN
DAS_IGNORE_BOOST_INTERPROCESS_WARNING
#include <boost/interprocess/sync/interprocess_condition.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
DAS_DISABLE_WARNING_END

// GUID 常量：SchedulerTestPlugin 与 TaskScheduler 测试共享的唯一来源。
// exe (TaskSchedulerTest) 与 dll (SchedulerTestPlugin) 通过此头共享同一组
// GUID 与 FactoryTaskSharedState 布局。
//
// 重构：原 std::mutex/std::condition_variable/std::vector/std::string 不能
// 跨进程共享。改为 boost::interprocess 的进程间同步原语和固定大小 POD 容器。
// exe 创建 managed_shared_memory 段，把 FactoryTaskSharedState 构造在其中，
// dll 通过 DasTestPlugin_SetSharedMemoryName 打开同名段，取回指针。
// 进程间共享 ⇒ 不再有 InProcess 模式下的双份 DasCore 全局状态 SEH 问题。

// {12345678-9ABC-4DEF-8123-456789ABCDEF}
inline constexpr DasGuid FactoryPluginGuid = {
    0x12345678,
    0x9ABC,
    0x4DEF,
    {0x81, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}};

// {87654321-CBA9-4FED-9123-FEDCBA987654}
inline constexpr DasGuid FactoryTaskGuid = {
    0x87654321,
    0xCBA9,
    0x4FED,
    {0x91, 0x23, 0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54}};

// {FECAC0D5-E038-4FDB-A6E9-EFCE10BCAE5A}
inline constexpr DasGuid FactoryAuthoringFactoryGuid = {
    0xFECAC0D5,
    0xE038,
    0x4FDB,
    {0xA6, 0xE9, 0xEF, 0xCE, 0x10, 0xBC, 0xAE, 0x5A}};

// {B56D1081-6838-4251-8CBB-BB223012FEF2}
inline constexpr DasGuid DecoyAuthoringFactoryGuid = {
    0xB56D1081,
    0x6838,
    0x4251,
    {0x8C, 0xBB, 0xBB, 0x22, 0x30, 0x12, 0xFE, 0xF2}};

// {2B9CE776-E95F-47F1-BD31-180B9238A94C}
inline constexpr DasGuid FactoryTaskComponentFactoryGuid = {
    0x2B9CE776,
    0xE95F,
    0x47F1,
    {0xBD, 0x31, 0x18, 0x0B, 0x92, 0x38, 0xA9, 0x4C}};

// {97BA1BE0-A199-47CB-9604-440B8BBC6555}
inline constexpr DasGuid FactoryTaskComponentImplGuid = {
    0x97BA1BE0,
    0xA199,
    0x47CB,
    {0x96, 0x04, 0x44, 0x0B, 0x8B, 0xBC, 0x65, 0x55}};

// {BBCAE0D0-5BC5-4B58-9FBD-585003B62B2D}
inline constexpr DasGuid FactoryAuthoringSessionGuid = {
    0xBBCAE0D0,
    0x5BC5,
    0x4B58,
    {0x9F, 0xBD, 0x58, 0x50, 0x03, 0xB6, 0x2B, 0x2D}};

inline constexpr char FactoryPluginGuidString[] =
    "12345678-9ABC-4DEF-8123-456789ABCDEF";
inline constexpr char FactoryTaskGuidString[] =
    "87654321-CBA9-4FED-9123-FEDCBA987654";
inline constexpr char FactoryAuthoringFactoryGuidString[] =
    "FECAC0D5-E038-4FDB-A6E9-EFCE10BCAE5A";
inline constexpr char FactoryTaskComponentFactoryGuidString[] =
    "2B9CE776-E95F-47F1-BD31-180B9238A94C";

// 测试用的非 Factory 常量，不在 dll 内，仅放此处作为 exe 与 dll
// 共享的字符串字面量。
inline constexpr char FactoryExecutionComponentGuidString[] =
    "68F10001-0000-4000-8000-000000000001";

// Convenience aliases for interprocess-safe synchronization.
// ipc_scoped_lock 同时满足 std::lock_guard 与 std::unique_lock 的用途：
// 它在构造时加锁、析构时解锁，并支持 unlock()/lock() 复用，可直接替换
// 原有的 std::lock_guard<std::mutex> 与 std::unique_lock<std::mutex>。
using ipc_mutex = boost::interprocess::interprocess_mutex;
using ipc_condition = boost::interprocess::interprocess_condition;
using ipc_scoped_lock = boost::interprocess::scoped_lock<ipc_mutex>;

struct FactoryTaskSharedState
{
    ipc_mutex     mutex;
    ipc_condition cv;

    int created_instance_count = 0;

    // Fixed-size replacement for std::vector<int>.
    static constexpr size_t MAX_EXECUTED_IDS = 32;
    int                     executed_instance_ids[MAX_EXECUTED_IDS]{};
    size_t                  executed_count = 0;

    void add_executed_id(int id)
    {
        if (executed_count < MAX_EXECUTED_IDS)
        {
            executed_instance_ids[executed_count++] = id;
        }
    }

    int     authoring_session_count = 0;
    int     get_document_count = 0;
    int     apply_change_count = 0;
    int     compile_count = 0;
    int     decoy_authoring_create_count = 0;
    bool    block_do = false;
    bool    do_entered = false;
    bool    unblock_do = false;
    int64_t last_context_entry_id = -1;
    int64_t last_context_task_id = -1;
    bool    last_context_had_task_id = false;
    int64_t last_context_revision = -1;

    // Fixed-size replacement for std::string.
    static constexpr size_t MAX_STRING_LEN = 256;
    char                    last_props_key1_value[MAX_STRING_LEN]{};
    char                    last_compile_purpose[MAX_STRING_LEN]{};

    bool apply_ok = true;
    bool compile_ok = true;

    void set_last_props_key1_value(std::string_view v)
    {
        auto len = std::min(v.size(), MAX_STRING_LEN - 1);
        std::memcpy(last_props_key1_value, v.data(), len);
        last_props_key1_value[len] = '\0';
    }

    void set_last_compile_purpose(std::string_view v)
    {
        auto len = std::min(v.size(), MAX_STRING_LEN - 1);
        std::memcpy(last_compile_purpose, v.data(), len);
        last_compile_purpose[len] = '\0';
    }

    std::string_view get_last_props_key1_value() const
    {
        return last_props_key1_value;
    }

    std::string_view get_last_compile_purpose() const
    {
        return last_compile_purpose;
    }
};

// dll 导出注入函数：exe 创建 managed_shared_memory 段后调用本函数，
// 把段名传给 dll。dll 用 open_only 打开同名段并 find<FactoryTaskSharedState>
// 取回指针。exe 与 dll 通过同一块共享内存观察同一对象。
// 若 exe 未调用本函数（如某些调试场景），dll 会在 DasCoCreatePlugin 时
// 回退到环境变量 DAS_SCHEDULER_TEST_SHM_NAME。
extern "C" DAS_EXPORT void DasTestPlugin_SetSharedMemoryName(const char* name);

#endif // DAS_PLUGINS_SCHEDULERTESTPLUGIN_SCHEDULERTESTSHAREDSTATE_H
