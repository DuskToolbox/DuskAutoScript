#ifndef DAS_ITASK_H
#define DAS_ITASK_H

#include <das/DasString.hpp>
#include <das/IDasTypeInfo.h>

struct DasDate
{
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
};

// {5C30785F-C2BD-4B9A-B543-955432169F8E}
DAS_DEFINE_GUID(
    DAS_IID_TASK,
    IDasTask,
    0x5c30785f,
    0xc2bd,
    0x4b9a,
    0xb5,
    0x43,
    0x95,
    0x54,
    0x32,
    0x16,
    0x9f,
    0x8e)
SWIG_IGNORE(IDasTask)
DAS_INTERFACE IDasTask : public IDasTypeInfo
{
    DAS_METHOD OnRequestExit() = 0;
    DAS_METHOD Do(
        IDasReadOnlyString * p_environment_json,
        IDasReadOnlyString * p_task_settings_json) = 0;
    DAS_METHOD GetNextExecutionTime(DasDate * p_out_date) = 0;
    DAS_METHOD GetName(IDasReadOnlyString * *pp_out_name) = 0;
    DAS_METHOD GetDescription(IDasReadOnlyString * *pp_out_settings) = 0;
    DAS_METHOD GetLabel(IDasReadOnlyString * *pp_out_label) = 0;
};

DAS_DEFINE_RET_TYPE(DasRetDate, DasDate);

// {3DE2D502-9621-4AF7-B88F-86458E0DDA46}
DAS_DEFINE_GUID(
    DAS_IID_SWIG_TASK,
    IDasSwigTask,
    0x3de2d502,
    0x9621,
    0x4af7,
    0xb8,
    0x8f,
    0x86,
    0x45,
    0x8e,
    0xd,
    0xda,
    0x46)
DAS_SWIG_DIRECTOR_ATTRIBUTE(IDasSwigTask)
DAS_INTERFACE IDasSwigTask : public IDasSwigTypeInfo
{
    virtual DasResult OnRequestExit() = 0;
    virtual DasResult Do(
        DasReadOnlyString environment_json,
        DasReadOnlyString task_settings_json) = 0;
    virtual DasRetDate           GetNextExecutionTime() = 0;
    virtual DasRetReadOnlyString GetName() = 0;
    virtual DasRetReadOnlyString GetDescription() = 0;
    virtual DasRetReadOnlyString GetLabel() = 0;
};

#endif // DAS_ITASK_H
