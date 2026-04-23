#ifndef DAS_CORE_SERVICES_H
#define DAS_CORE_SERVICES_H

#include <das/IDasBase.h>

DAS_INTERFACE IDasSettingsService;
DAS_INTERFACE IDasPluginManagerService;
struct IDasSchedulerService;

DAS_DEFINE_GUID(
    DAS_IID_CORE_SERVICES,
    IDasCoreServices,
    0xD4E5F6A7,
    0xB8C9,
    0x4D0E,
    0x9F,
    0x1A,
    0x2B,
    0x3C,
    0x4D,
    0x5E,
    0x6F,
    0x70)

DAS_SWIG_EXPORT_ATTRIBUTE(IDasCoreServices)
DAS_INTERFACE IDasCoreServices : public IDasBase
{
    DAS_METHOD GetSettingsService(IDasSettingsService * *pp_out) = 0;
    DAS_METHOD GetPluginManagerService(IDasPluginManagerService * *pp_out) = 0;
    DAS_METHOD GetSchedulerService(IDasSchedulerService * *pp_out) = 0;
};

#endif // DAS_CORE_SERVICES_H
