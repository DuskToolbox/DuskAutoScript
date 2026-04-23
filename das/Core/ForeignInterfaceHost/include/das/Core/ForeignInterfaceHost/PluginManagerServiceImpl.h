#ifndef DAS_CORE_FOREIGNINTERFACEHOST_PLUGINMANAGERSERVICEIMPL_H
#define DAS_CORE_FOREIGNINTERFACEHOST_PLUGINMANAGERSERVICEIMPL_H

#include <atomic>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/IDasPluginManagerService.h>
#include <filesystem>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class PluginManagerServiceImpl final : public IDasPluginManagerService
{
public:
    explicit PluginManagerServiceImpl(
        PluginManager&        mgr,
        std::filesystem::path plugin_dir);
    ~PluginManagerServiceImpl() = default;

    // IDasBase
    uint32_t DAS_STD_CALL AddRef() override;
    uint32_t DAS_STD_CALL Release() override;
    DasResult DAS_STD_CALL
    QueryInterface(const DasGuid& iid, void** pp_out) override;

    // IDasPluginManagerService
    DasResult ScanInstalledPlugins(
        Das::ExportInterface::IDasJson** pp_out_plugins) override;
    DasResult InstallPluginPackage(IDasReadOnlyString* p_package_path) override;
    DasResult InstallPluginPackageData(
        const uint8_t* p_package_data,
        uint64_t       package_size) override;
    DasResult MarkPluginPackageForDeletion(
        const DasGuid* p_package_guid) override;
    DasResult CreateComponent(
        const DasGuid* p_component_iid,
        IDasBase**     pp_out_component) override;
    DasResult GetPluginSettingsFieldNames(
        const DasGuid*                           p_plugin_guid,
        Das::ExportInterface::IDasStringVector** pp_out) const override;
    DasResult CreateCaptureManager(
        IDasReadOnlyString*                        p_environment_config,
        Das::ExportInterface::IDasCaptureManager** pp_out) override;
    DasResult SetHostExePath(IDasReadOnlyString* p_host_exe_path) override;

private:
    std::atomic<uint32_t> ref_count_{0};
    PluginManager&        mgr_;
    std::filesystem::path plugin_dir_;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_PLUGINMANAGERSERVICEIMPL_H
