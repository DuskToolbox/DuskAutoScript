#ifndef DAS_SCHEDULER_H
#define DAS_SCHEDULER_H

#include <das/DasString.hpp>
#include <das/IDasBase.h>

static const char* const DAS_TASK_INFO_PROPERTIES[] =
    {"name", "description", "label", "typeName"};

const size_t DAS_TASK_INFO_PROPERTIES_NAME_INDEX = 0;
const size_t DAS_TASK_INFO_PROPERTIES_DESCRIPTION_INDEX = 1;
const size_t DAS_TASK_INFO_PROPERTIES_LABEL_INDEX = 2;
const size_t DAS_TASK_INFO_PROPERTIES_TYPE_NAME_INDEX = 3;

// {CAD61DC0-CFFF-4069-BBE5-349D371189FB}
DAS_DEFINE_GUID(
    DAS_IID_TASK_INFO,
    IDasTaskInfo,
    0xcad61dc0,
    0xcfff,
    0x4069,
    0xbb,
    0xe5,
    0x34,
    0x9d,
    0x37,
    0x11,
    0x89,
    0xfb);
SWIG_IGNORE(IDasTaskInfo)
// pure C++ API
DAS_INTERFACE IDasTaskInfo : public IDasWeakReferenceSource
{
    DAS_METHOD GetProperty(
        const char*  property_name,
        const char** pp_out_value) = 0;
    DAS_METHOD GetInitializeState() = 0;
    DAS_METHOD GetIid(DasGuid * p_out_iid) = 0;
};

// {E997A124-CCCD-47A8-A632-91C5991FC639}
DAS_DEFINE_GUID(
    DAS_IID_TASK_INFO_VECTOR,
    IDasTaskInfoVector,
    0xe997a124,
    0xcccd,
    0x47a8,
    0xa6,
    0x32,
    0x91,
    0xc5,
    0x99,
    0x1f,
    0xc6,
    0x39);
SWIG_IGNORE(IDasTaskInfoVector)
DAS_INTERFACE IDasTaskInfoVector : public IDasBase
{
    DAS_METHOD EnumByIndex(size_t index, IDasTaskInfo** pp_out_info) = 0;
    DAS_METHOD EnumNextExecuteTimeByIndex(size_t index, time_t* p_out_time) = 0;
};

// {28DCD3C8-E528-414A-8649-F7E63C3C1715}
DAS_DEFINE_GUID(
    DAS_IID_TASK_SCHEDULER,
    IDasTaskScheduler,
    0x28dcd3c8,
    0xe528,
    0x414a,
    0x86,
    0x49,
    0xf7,
    0xe6,
    0x3c,
    0x3c,
    0x17,
    0x15);
SWIG_IGNORE(IDasTaskScheduler)
// pure C++ API
DAS_INTERFACE IDasTaskScheduler : public IDasBase
{
    DAS_METHOD GetAllWorkingTasks(
        IDasTaskInfoVector * *pp_out_task_info_vector) = 0;
    DAS_METHOD AddTask(IDasTaskInfo * p_task_info) = 0;
    DAS_METHOD RemoveTask(IDasTaskInfo * p_task_info) = 0;
    DAS_METHOD UpdateEnvironmentConfig(IDasReadOnlyString * p_config_json) = 0;
    DAS_BOOL_METHOD IsTaskExecuting() = 0;
    DAS_METHOD      SetEnabled(DasBool enabled) = 0;
    DAS_BOOL_METHOD GetEnabled() = 0;
    DAS_METHOD ForceStart() = 0;
    DAS_METHOD RequestStop() = 0;
};

DAS_C_API DasResult GetIDasTaskScheduler(IDasTaskScheduler ** pp_out_task_scheduler);

#endif // DAS_SCHEDULER_H
