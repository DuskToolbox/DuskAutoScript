#ifndef DAS_CORE_FOREIGNINTERFACEHOST_PLUGINMANAGERSERVICEIMPL_H
#define DAS_CORE_FOREIGNINTERFACEHOST_PLUGINMANAGERSERVICEIMPL_H

#include <atomic>
#include <condition_variable>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/IDasPluginManagerService.h>
#include <filesystem>
#include <mutex>
#include <unordered_set>

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

    /**
     * @name Per-plugin inflight guard
     *
     * Protects InstallPluginPackageData and MarkPluginPackageForDeletion
     * for the same plugin GUID or name. Dual-key RAII InflightGuard
     * acquires both "guid:{guid}" and "name:{name}" keys in sorted
     * order to prevent deadlock.
     *
     * Same GUID or same name operations serialize; different GUID AND
     * different name operations proceed independently.
     *
     * @architecture HTTP config domain (Phase 52). Multiple HTTP workers
     * may call these methods concurrently; the inflight guard ensures
     * same-plugin operations are serialized without blocking different
     * plugins.
     * @{
     */
    mutable std::mutex              inflight_mutex_;
    std::condition_variable         inflight_cv_;
    std::unordered_set<std::string> inflight_plugin_ops_;
    /** @} */
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_PLUGINMANAGERSERVICEIMPL_H
