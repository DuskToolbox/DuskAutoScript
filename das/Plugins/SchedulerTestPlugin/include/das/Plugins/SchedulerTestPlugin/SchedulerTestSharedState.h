#ifndef DAS_PLUGINS_SCHEDULERTESTPLUGIN_SCHEDULERTESTSHAREDSTATE_H
#define DAS_PLUGINS_SCHEDULERTESTPLUGIN_SCHEDULERTESTSHAREDSTATE_H

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <das/DasExport.h>
#include <das/IDasBase.h>

// GUID 常量：SchedulerTestPlugin 与 TaskScheduler 测试共享的唯一来源。
// exe (TaskSchedulerTest) 与 dll (SchedulerTestPlugin) 通过此头共享同一组
// GUID 与 FactoryTaskSharedState 布局，dll 内对象通过裸指针观察 exe 持有的
// shared_state。

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

struct FactoryTaskSharedState
{
    std::mutex              mutex;
    std::condition_variable cv;
    int                     created_instance_count = 0;
    std::vector<int>        executed_instance_ids;
    int                     authoring_session_count = 0;
    int                     get_document_count = 0;
    int                     apply_change_count = 0;
    int                     compile_count = 0;
    int                     decoy_authoring_create_count = 0;
    bool                    block_do = false;
    bool                    do_entered = false;
    bool                    unblock_do = false;
    int64_t                 last_context_entry_id = -1;
    int64_t                 last_context_task_id = -1;
    bool                    last_context_had_task_id = false;
    int64_t                 last_context_revision = -1;
    std::string             last_props_key1_value;
    std::string             last_compile_purpose;
    bool                    apply_ok = true;
    bool                    compile_ok = true;
};

// dll 导出注入函数：exe 在加载 dll 后立即调用，把 exe 持有的
// FactoryTaskSharedState 裸指针注入 dll 的全局变量。dll 内所有对象通过此
// 指针观察 exe 的状态，不拥有、不释放。
extern "C" DAS_EXPORT void DasTestPlugin_SetSharedState(
    FactoryTaskSharedState* p_shared_state);

#endif // DAS_PLUGINS_SCHEDULERTESTPLUGIN_SCHEDULERTESTSHAREDSTATE_H
