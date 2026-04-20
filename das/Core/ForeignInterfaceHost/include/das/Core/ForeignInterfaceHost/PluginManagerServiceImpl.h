#ifndef DAS_CORE_FOREIGNINTERFACEHOST_PLUGINMANAGERSERVICEIMPL_H
#define DAS_CORE_FOREIGNINTERFACEHOST_PLUGINMANAGERSERVICEIMPL_H

#include <atomic>
#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/IDasPluginManagerService.h>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class PluginManagerServiceImpl final : public IDasPluginManagerService
{
public:
    explicit PluginManagerServiceImpl(PluginManager& mgr);
    ~PluginManagerServiceImpl() = default;

    // IDasBase
    uint32_t DAS_STD_CALL AddRef() override;
    uint32_t DAS_STD_CALL Release() override;
    DasResult DAS_STD_CALL
    QueryInterface(const DasGuid& iid, void** pp_out) override;

    // IDasPluginManagerService
    DasResult CreateComponent(const DasGuid& iid, void** pp_out) override;
    std::vector<std::string> GetPluginSettingsFieldNames(
        const DasGuid& plugin_guid) const override;
    DasResult CreateCaptureManager(
        void*  p_environment_config,
        void*  p_settings_service,
        void** pp_out) override;

private:
    std::atomic<uint32_t> ref_count_{0};
    PluginManager&        mgr_;
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_PLUGINMANAGERSERVICEIMPL_H
