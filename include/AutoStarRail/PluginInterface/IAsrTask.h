#ifndef ASR_ITASK_H
#define ASR_ITASK_H

#include <AutoStarRail/AsrString.hpp>
#include <AutoStarRail/IAsrTypeInfo.h>

struct AsrDate
{
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
};

// {5C30785F-C2BD-4B9A-B543-955432169F8E}
ASR_DEFINE_GUID(
    ASR_IID_TASK,
    IAsrTask,
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
SWIG_IGNORE(IAsrTask)
ASR_INTERFACE IAsrTask : public IAsrTypeInfo
{
    ASR_METHOD OnRequestExit() = 0;
    ASR_METHOD Do(
        IAsrReadOnlyString * p_environment_json,
        IAsrReadOnlyString * p_task_settings_json) = 0;
    ASR_METHOD GetNextExecutionTime(AsrDate * p_out_date) = 0;
    ASR_METHOD GetName(IAsrReadOnlyString * *pp_out_name) = 0;
    ASR_METHOD GetDescription(IAsrReadOnlyString * *pp_out_settings) = 0;
    ASR_METHOD GetLabel(IAsrReadOnlyString * *pp_out_label) = 0;
};

ASR_DEFINE_RET_TYPE(AsrRetDate, AsrDate);

// {3DE2D502-9621-4AF7-B88F-86458E0DDA46}
ASR_DEFINE_GUID(
    ASR_IID_SWIG_TASK,
    IAsrSwigTask,
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
ASR_SWIG_DIRECTOR_ATTRIBUTE(IAsrSwigTask)
ASR_INTERFACE IAsrSwigTask : public IAsrSwigTypeInfo
{
    virtual AsrResult OnRequestExit() = 0;
    virtual AsrResult Do(
        AsrReadOnlyString environment_json,
        AsrReadOnlyString task_settings_json) = 0;
    virtual AsrRetDate           GetNextExecutionTime() = 0;
    virtual AsrRetReadOnlyString GetName() = 0;
    virtual AsrRetReadOnlyString GetDescription() = 0;
    virtual AsrRetReadOnlyString GetLabel() = 0;
};

#endif // ASR_ITASK_H
