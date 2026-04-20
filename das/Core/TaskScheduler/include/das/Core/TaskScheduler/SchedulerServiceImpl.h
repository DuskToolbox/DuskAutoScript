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
            const std::filesystem::path& plugin_dir,
            const std::vector<DasGuid>&  disabled_guids) override;
        DasResult      Enable() override;
        DasResult      Disable() override;
        SchedulerState Status() const override;

    private:
        std::atomic<uint32_t> ref_count_{0};
        SchedulerService&     svc_;
    };

} // namespace Das::Core::TaskScheduler
