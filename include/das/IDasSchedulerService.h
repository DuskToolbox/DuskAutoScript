#pragma once

#include <das/IDasBase.h>
#include <filesystem>
#include <vector>

DAS_DEFINE_GUID(
    DAS_IID_SCHEDULER_SERVICE,
    IDasSchedulerService,
    0xC3D4E5F6,
    0xA7B8,
    0x4C9D,
    0x0E,
    0x1F,
    0x2A,
    0x3B,
    0x4C,
    0x5D,
    0x6E,
    0x7F)

DAS_SWIG_EXPORT_ATTRIBUTE(IDasSchedulerService)
DAS_INTERFACE IDasSchedulerService : public IDasBase
{
    enum class SchedulerState : int
    {
        Stopped = 0,
        Running = 1,
    };

    virtual DasResult Initialize(
        const std::filesystem::path& plugin_dir,
        const std::vector<DasGuid>&  disabled_guids) = 0;
    virtual DasResult      Enable() = 0;
    virtual DasResult      Disable() = 0;
    virtual SchedulerState Status() const = 0;
};
