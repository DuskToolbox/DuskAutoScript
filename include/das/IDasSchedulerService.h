#pragma once

#include <das/DasTypes.hpp>
#include <filesystem>
#include <vector>

namespace Das::Core::TaskScheduler
{

    enum class SchedulerState : int
    {
        Stopped = 0,
        Running = 1,
    };

    struct IDasSchedulerService
    {
        virtual ~IDasSchedulerService() = default;

        virtual DasResult Initialize(
            const std::filesystem::path& plugin_dir,
            const std::vector<DasGuid>&  disabled_guids) = 0;
        virtual DasResult      Enable() = 0;
        virtual DasResult      Disable() = 0;
        virtual SchedulerState Status() const = 0;

    protected:
        IDasSchedulerService() = default;
    };

} // namespace Das::Core::TaskScheduler
