#ifndef DAS_PLUGIN_MANAGER_SERVICE_H
#define DAS_PLUGIN_MANAGER_SERVICE_H

#include <das/DasString.hpp>
#include <das/IDasBase.h>

struct IDasBase;

namespace Das::ExportInterface
{
    DAS_INTERFACE IDasStringVector;
    DAS_INTERFACE IDasCaptureManager;
    DAS_INTERFACE IDasJson;
} // namespace Das::ExportInterface

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
    // Plugin discovery
    DAS_METHOD ScanInstalledPlugins(
        Das::ExportInterface::IDasJson * *pp_out_plugins) = 0;

    // Plugin package management
    DAS_METHOD InstallPluginPackage(IDasReadOnlyString * p_package_path) = 0;

    DAS_METHOD MarkPluginPackageForDeletion(const DasGuid* p_package_guid) = 0;

    // Component creation (hides ComponentFactoryManager)
    DAS_METHOD CreateComponent(
        const DasGuid* p_component_iid,
        IDasBase**     pp_out_component) = 0;

    // Get settings field names for a plugin (hides PluginPackageDesc)
    DAS_METHOD GetPluginSettingsFieldNames(
        const DasGuid*                           p_plugin_guid,
        Das::ExportInterface::IDasStringVector** pp_out) const = 0;

    // Create capture manager (hides FeatureInfo/internal iteration)
    DAS_METHOD CreateCaptureManager(
        IDasReadOnlyString * p_environment_config,
        Das::ExportInterface::IDasCaptureManager * *pp_out) = 0;

    // Host executable path
    DAS_METHOD SetHostExePath(IDasReadOnlyString * p_host_exe_path) = 0;
};

#endif // DAS_PLUGIN_MANAGER_SERVICE_H
