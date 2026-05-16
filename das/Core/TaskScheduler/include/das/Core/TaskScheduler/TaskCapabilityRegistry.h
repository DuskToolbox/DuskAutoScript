#pragma once

#include <das/Core/ForeignInterfaceHost/ForeignInterfaceHost.h>
#include <das/Core/ForeignInterfaceHost/DasGuid.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Das::Core::TaskScheduler
{
    struct TaskAuthoringCapability
    {
        DasGuid                  task_guid{};
        DasGuid                  plugin_guid{};
        uint64_t                 task_feature_index = 0;
        DasGuid                  authoring_factory_guid{};
        std::vector<std::string> supported_kinds;
    };

    struct TaskCapabilityRecord
    {
        DasGuid                                    task_guid{};
        DasGuid                                    plugin_guid{};
        uint64_t                                   task_feature_index = 0;
        std::optional<TaskAuthoringCapability>     authoring;
        std::optional<DasGuid>                     execution_component_guid;
    };

    class TaskCapabilityRegistry
    {
    public:
        void Clear();

        void AddTaskDescriptor(
            const DasGuid& plugin_guid,
            const DasGuid& task_guid,
            uint64_t task_feature_index,
            const Das::Core::ForeignInterfaceHost::TaskDescriptor& descriptor);

        const TaskCapabilityRecord* FindTask(const DasGuid& task_guid) const;
        const TaskAuthoringCapability* FindAuthoring(
            const DasGuid& task_guid) const;
        const DasGuid* FindExecutionComponent(
            const DasGuid& task_guid) const;

    private:
        std::unordered_map<DasGuid, TaskCapabilityRecord> records_;
    };
}
