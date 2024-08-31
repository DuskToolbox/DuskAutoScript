#ifndef ASR_CORE_FOREIGNINTERFACEHOST_TASKMANAGER_H
#define ASR_CORE_FOREIGNINTERFACEHOST_TASKMANAGER_H

#include <AutoStarRail/Core/ForeignInterfaceHost/AsrGuid.h>
#include <AutoStarRail/Core/ForeignInterfaceHost/Config.h>
#include <AutoStarRail/PluginInterface/IAsrTask.h>
#include <unordered_map>

ASR_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

/**
* @brief task不得被外部调用，因此FindInterface不支持SWIG是符合预期的
*/
class TaskManager
{
    std::unordered_map<AsrGuid, AsrPtr<IAsrTask>> map_;

public:
    AsrResult Register(IAsrTask* p_task, AsrGuid guid);
    AsrResult Register(IAsrSwigTask* p_swig_task, AsrGuid guid);
    AsrResult FindInterface(const AsrGuid& guid, IAsrTask** pp_out_task);
};

ASR_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // ASR_CORE_FOREIGNINTERFACEHOST_TASKMANAGER_H
