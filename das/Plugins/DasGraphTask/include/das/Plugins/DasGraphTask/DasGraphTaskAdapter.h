#ifndef DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKADAPTER_H
#define DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKADAPTER_H

#include <das/DasPtr.hpp>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTask.Implements.hpp>

// Task type GUID: {F4A2C9D1-7B3E-4D8A-A15F-C2E86B4D91A3}
// Must equal DasGraphTask.json tasks.<key> / executionComponent.componentGuid,
// otherwise SchedulerService::Initialize cannot match task_descriptors to fill
// name="das.graph" (SchedulerService.cpp task_descriptors.find(task_guid)).
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Plugins::DasGraphTask,
    DasGraphTaskAdapter,
    0xF4A2C9D1,
    0x7B3E,
    0x4D8A,
    0xA1,
    0x5F,
    0xC2,
    0xE8,
    0x6B,
    0x4D,
    0x91,
    0xA3);

DAS_NS_BEGIN
namespace Plugins::DasGraphTask
{
    // IDasTask thin adapter (ROADMAP Phase 68: Scheduler top-level executor).
    // Receives the CompiledGraphPlanDto snapshot produced by the scheduler's
    // authoring Compile (task_settings) and passes it straight through to
    // GraphRuntime::Execute — zero business logic. host 经构造注入（包默认
    // 存储，加载器保证注入先于本 adapter 的创建）。
    class DasGraphTaskAdapter final
        : public PluginInterface::DasTaskImplBase<DasGraphTaskAdapter>
    {
        DasPtr<PluginInterface::IDasTaskComponentHost> host_;

    public:
        explicit DasGraphTaskAdapter(
            PluginInterface::IDasTaskComponentHost* p_host);

        DAS_IMPL Do(
            PluginInterface::IDasStopToken* stop_token,
            ExportInterface::IDasJson*      p_environment_json,
            ExportInterface::IDasJson*      p_task_settings_json) override;

        DAS_IMPL GetNextExecutionTime(
            ExportInterface::IDasJson* p_task_settings_json,
            ExportInterface::DasDate*  p_out_date) override;
    };
} // namespace Plugins::DasGraphTask
DAS_NS_END

#endif // DAS_PLUGINS_DASGRAPHTASK_DASGRAPHTASKADAPTER_H
