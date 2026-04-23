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

    private:
        std::atomic<uint32_t> ref_count_{0};
        SchedulerService&     svc_;
    };

} // namespace Das::Core::TaskScheduler
