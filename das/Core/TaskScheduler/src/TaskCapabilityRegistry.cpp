#include <das/Core/TaskScheduler/TaskCapabilityRegistry.h>

namespace Das::Core::TaskScheduler
{
    void TaskCapabilityRegistry::Clear() { records_.clear(); }

    void TaskCapabilityRegistry::AddTaskDescriptor(
        const DasGuid& plugin_guid,
        const DasGuid& task_guid,
        uint64_t task_feature_index,
        const Das::Core::ForeignInterfaceHost::TaskDescriptor& descriptor)
    {
        TaskCapabilityRecord record;
        record.task_guid = task_guid;
        record.plugin_guid = plugin_guid;
        record.task_feature_index = task_feature_index;

        if (descriptor.authoring)
        {
            TaskAuthoringCapability authoring;
            authoring.task_guid = task_guid;
            authoring.plugin_guid = plugin_guid;
            authoring.task_feature_index = task_feature_index;
            authoring.authoring_factory_guid =
                descriptor.authoring->factory_guid;
            authoring.supported_kinds = descriptor.authoring->supported_kinds;
            record.authoring = std::move(authoring);
        }

        records_[task_guid] = std::move(record);
    }

    const TaskCapabilityRecord* TaskCapabilityRegistry::FindTask(
        const DasGuid& task_guid) const
    {
        auto it = records_.find(task_guid);
        if (it == records_.end())
        {
            return nullptr;
        }
        return &it->second;
    }

    const TaskAuthoringCapability* TaskCapabilityRegistry::FindAuthoring(
        const DasGuid& task_guid) const
    {
        auto* record = FindTask(task_guid);
        if (!record || !record->authoring)
        {
            return nullptr;
        }
        return &*record->authoring;
    }
}
