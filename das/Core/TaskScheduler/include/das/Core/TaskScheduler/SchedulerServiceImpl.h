#pragma once

#include <atomic>
#include <das/Core/TaskScheduler/SchedulerService.h>
#include <das/IDasSchedulerService.h>

namespace Das::Core::TaskScheduler
{

    class SchedulerServiceImpl final : public IDasSchedulerService
    {
    public:
        explicit SchedulerServiceImpl(SchedulerService& svc);

        // IDasBase
        uint32_t DAS_STD_CALL AddRef() override;
        uint32_t DAS_STD_CALL Release() override;
        DasResult DAS_STD_CALL
        QueryInterface(const DasGuid& iid, void** pp_out) override;

        // IDasSchedulerService
        DasResult Initialize(
            IDasReadOnlyString*                           p_plugin_dir,
            Das::ExportInterface::IDasReadOnlyGuidVector* p_disabled_guids)
            override;
        DasResult Start() override;
        DasResult Stop() override;
        DasResult GetState(SchedulerState* p_out_state) const override;
        DasResult Get(IDasReadOnlyString** pp_out_json) override;
        DasResult AddTask(const DasGuid& task_guid, int64_t* p_out_task_id)
            override;
        DasResult DeleteTask(int64_t task_id) override;
        DasResult UpdateTaskProperties(
            int64_t             task_id,
            IDasReadOnlyString* p_properties_json) override;
        DasResult UpdateTaskInternalProperties(
            int64_t             task_id,
            IDasReadOnlyString* p_properties_json) override;
        DasResult GetTaskAuthoringDocument(
            int64_t              task_id,
            IDasReadOnlyString*  p_request_json,
            IDasReadOnlyString** pp_out_json) override;
        DasResult ApplyTaskAuthoringChange(
            int64_t              task_id,
            IDasReadOnlyString*  p_change_json,
            IDasReadOnlyString** pp_out_json) override;
        DasResult CompileTaskAuthoring(
            int64_t              task_id,
            IDasReadOnlyString*  p_request_json,
            IDasReadOnlyString** pp_out_json) override;
        DasResult SetStateNotifyCallback(
            SchedulerNotifyFunc func,
            void*               user_data) override;

    private:
        std::atomic<uint32_t> ref_count_{0};
        SchedulerService&     svc_;
    };

} // namespace Das::Core::TaskScheduler
