#ifndef DAS_TASKMANAGER_H
#define DAS_TASKMANAGER_H

/**
 * @file IDasTaskManager.h
 * @brief The content in this file will NOT be processed by SWIG.
    The exported interface in this file should only be used by GUI programs.
 * @version 0.1
 * @date 2023-07-18
 *
 * @copyright Copyright (c) 2023
 *
 */

#include "das/DasExport.h"
#include "das/DasString.hpp"
#include <das/IDasBase.h>

DAS_INTERFACE IDasTask;

// {23B3F3A7-40E4-4A04-B1F0-9F2F15B8775C}
DAS_DEFINE_GUID(
    DAS_IID_TASK_MANAGER,
    IDasTaskManager,
    0x23b3f3a7,
    0x40e4,
    0x4a04,
    0xb1,
    0xf0,
    0x9f,
    0x2f,
    0x15,
    0xb8,
    0x77,
    0x5c);
SWIG_IGNORE(IDasTaskManager)
DAS_INTERFACE IDasTaskManager : public IDasBase
{
    DAS_METHOD SetEnable(const DasGuid& plugin_id);

    DAS_METHOD EnumTask(size_t index, IDasTask * p_out_info);

    DAS_METHOD Resume(IDasReadOnlyString * *pp_out_error_string);
    DAS_METHOD Pause(IDasReadOnlyString * *pp_out_error_string);

    DAS_METHOD UpdateConnectionJson(
        IDasReadOnlyString * p_connection_json,
        IDasReadOnlyString * *pp_out_error_string);
};

SWIG_IGNORE(CreateIDasTaskManager)
DAS_C_API DasResult CreateIDasTaskManager(
    IDasReadOnlyString* p_connection_json,
    IDasTaskManager**   ppIDasTaskManager);

#endif // DAS_TASKMANAGER_H
