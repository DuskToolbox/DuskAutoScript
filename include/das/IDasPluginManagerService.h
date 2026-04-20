#ifndef DAS_PLUGIN_MANAGER_SERVICE_H
#define DAS_PLUGIN_MANAGER_SERVICE_H

#include <das/DasString.hpp>
#include <das/IDasBase.h>
#include <das/IDasSettingsService.h>
#include <string>
#include <vector>

namespace Das::ExportInterface
{
    DAS_INTERFACE IDasCaptureManager;
}

DAS_DEFINE_GUID(
    DAS_IID_PLUGIN_MANAGER_SERVICE,
    IDasPluginManagerService,
    0xB2C3D4E5,
    0xF6A7,
    0x4B8C,
    0x9D,
    0x0E,
    0x1F,
    0x2A,
    0x3B,
    0x4C,
    0x5D,
    0x6E)

DAS_SWIG_EXPORT_ATTRIBUTE(IDasPluginManagerService)
DAS_INTERFACE IDasPluginManagerService : public IDasBase
{
    // Component creation (hides ComponentFactoryManager)
    DAS_METHOD CreateComponent(const DasGuid& iid, void** pp_out) = 0;

    // Get settings field names for a plugin (hides PluginPackageDesc)
    DAS_METHOD_(std::vector<std::string>)
    GetPluginSettingsFieldNames(const DasGuid& plugin_guid) const = 0;

    // Create capture manager (hides FeatureInfo/internal iteration)
    DAS_METHOD CreateCaptureManager(
        IDasReadOnlyString * p_environment_config,
        IDasSettingsService * p_settings_service,
        Das::ExportInterface::IDasCaptureManager * *pp_out) = 0;
};

#endif // DAS_PLUGIN_MANAGER_SERVICE_H
