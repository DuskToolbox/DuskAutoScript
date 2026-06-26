#pragma once

#include <das/IDasBase.h>

struct IDasReadOnlyString;

typedef void (*SchedulerNotifyFunc)(const char* json, void* user_data);

namespace Das::ExportInterface
{
    DAS_INTERFACE IDasReadOnlyGuidVector;
} // namespace Das::ExportInterface

DAS_DEFINE_GUID(
    DAS_IID_SCHEDULER_SERVICE,
    IDasSchedulerService,
    0xC3D4E5F6,
    0xA7B8,
    0x4C9D,
    0x0E,
    0x1F,
    0x2A,
    0x3B,
    0x4C,
    0x5D,
    0x6E,
    0x7F)

DAS_SWIG_EXPORT_ATTRIBUTE(IDasSchedulerService)
DAS_INTERFACE IDasSchedulerService : public IDasBase
{
    enum class SchedulerState : int
    {
        Stopped = 0,
        Running = 1,
        Stopping = 2,
    };

    DAS_METHOD Initialize(
        IDasReadOnlyString * p_plugin_dir,
        Das::ExportInterface::IDasReadOnlyGuidVector * p_disabled_guids) = 0;
    DAS_METHOD Start() = 0;
    DAS_METHOD Stop() = 0;
    DAS_METHOD GetState(SchedulerState * p_out_state) const = 0;
    DAS_METHOD Get(IDasReadOnlyString * *pp_out_json) = 0;
    DAS_METHOD GetTaskRepository(IDasReadOnlyString * *pp_out_json) = 0;
    DAS_METHOD CreateRepositoryEntry(
        IDasReadOnlyString * p_request_json,
        IDasReadOnlyString * *pp_out_json) = 0;
    DAS_METHOD DeleteRepositoryEntry(int64_t entry_id) = 0;
    DAS_METHOD RenameRepositoryEntry(
        int64_t              entry_id,
        IDasReadOnlyString*  p_request_json,
        IDasReadOnlyString** pp_out_json) = 0;
    DAS_METHOD GetRepositoryEntryAuthoringDocument(
        int64_t              entry_id,
        IDasReadOnlyString*  p_request_json,
        IDasReadOnlyString** pp_out_json) = 0;
    DAS_METHOD ApplyRepositoryEntryAuthoringChange(
        int64_t              entry_id,
        IDasReadOnlyString*  p_change_json,
        IDasReadOnlyString** pp_out_json) = 0;
    DAS_METHOD CompileRepositoryEntryAuthoring(
        int64_t              entry_id,
        IDasReadOnlyString*  p_request_json,
        IDasReadOnlyString** pp_out_json) = 0;
    DAS_METHOD AddTask(const DasGuid& task_guid, int64_t* p_out_task_id) = 0;
    DAS_METHOD DeleteTask(int64_t task_id) = 0;
    DAS_METHOD UpdateTaskProperties(
        int64_t             task_id,
        IDasReadOnlyString* p_properties_json) = 0;
    DAS_METHOD UpdateTaskInternalProperties(
        int64_t             task_id,
        IDasReadOnlyString* p_properties_json) = 0;
    DAS_METHOD SetTaskEnabled(int64_t task_id, DasBool enabled) = 0;
    DAS_METHOD GetTaskAuthoringDocument(
        int64_t              task_id,
        IDasReadOnlyString*  p_request_json,
        IDasReadOnlyString** pp_out_json) = 0;
    DAS_METHOD ApplyTaskAuthoringChange(
        int64_t              task_id,
        IDasReadOnlyString*  p_change_json,
        IDasReadOnlyString** pp_out_json) = 0;
    DAS_METHOD CompileTaskAuthoring(
        int64_t              task_id,
        IDasReadOnlyString*  p_request_json,
        IDasReadOnlyString** pp_out_json) = 0;
    DAS_METHOD SetStateNotifyCallback(
        SchedulerNotifyFunc func,
        void*               user_data) = 0;
};
